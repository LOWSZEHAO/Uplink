// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// viewport_screenshot - synchronous pixel readback of the active viewport
// (PIE game viewport during play, else the active editor viewport).
// Direct ReadPixels is used instead of FScreenshotRequest because the
// screenshot-captured delegate does not fire for PIE-in-editor-viewport
// sessions (found during live testing).

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "UnrealClient.h"
#include "UplinkObservation.h"
#include "UplinkSlateScreenshot.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SViewport.h"

namespace
{
	/** Compress BGRA pixels to PNG and package them as a tool result. */
	FUplinkToolResult EncodePng(const TArray<FColor>& Pixels, const FIntPoint& Size, const TCHAR* SourceLabel)
	{
		IImageWrapperModule& ImageWrapperModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Png = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!Png.IsValid() || !Png->SetRaw(
			Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size.X, Size.Y, ERGBFormat::BGRA, 8))
		{
			return FUplinkToolResult::Error(TEXT("PNG encoding failed"));
		}

		FUplinkToolResult Out = FUplinkToolResult::Ok();
		Out.Png = Png->GetCompressed(90);
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("width"), Size.X);
		Data->SetNumberField(TEXT("height"), Size.Y);
		Data->SetStringField(TEXT("source"), SourceLabel);
		Out.Data = Data;
		return Out;
	}
}

using namespace UplinkToolUtil;

namespace UplinkObservation
{
	bool CaptureViewportPng(TArray64<uint8>& OutPng, FIntPoint& OutSize, FString& OutSource, FString& OutError)
	{
		FViewport* Viewport = nullptr;
		if (GEditor && GEditor->PlayWorld && GEngine && GEngine->GameViewport)
		{
			Viewport = GEngine->GameViewport->Viewport;
			OutSource = TEXT("pie_game_viewport");
		}
		else if (GEditor)
		{
			Viewport = GEditor->GetActiveViewport();
			OutSource = TEXT("editor_viewport");
		}
		if (!Viewport)
		{
			OutError = TEXT("no viewport available to capture");
			return false;
		}
		OutSize = Viewport->GetSizeXY();
		if (OutSize.X <= 0 || OutSize.Y <= 0)
		{
			OutError = TEXT("viewport has zero size");
			return false;
		}
		TArray<FColor> Pixels;
		if (!Viewport->ReadPixels(Pixels) || Pixels.Num() != OutSize.X * OutSize.Y)
		{
			OutError = TEXT("viewport pixel readback failed");
			return false;
		}
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}
		IImageWrapperModule& ImageWrapperModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Png = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!Png.IsValid() || !Png->SetRaw(
			Pixels.GetData(), Pixels.Num() * sizeof(FColor), OutSize.X, OutSize.Y, ERGBFormat::BGRA, 8))
		{
			OutError = TEXT("PNG encoding failed");
			return false;
		}
		OutPng = Png->GetCompressed(90);
		return true;
	}
}

void UplinkTools::RegisterCapture(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("viewport_screenshot"),
		TEXT("Capture what the player sees as a PNG. During play this goes through Slate, so the UMG layer is included - menus, HUD, quest text, prompts. That matters: reading the viewport's pixels directly returns the rendered scene with the entire UI missing, which makes a menu look like an empty room. Outside play it captures the active editor viewport, redrawn first so a window that is not in front does not hand back a stale frame."),
		TEXT(R"json({"type":"object","properties":{"include_ui":{"type":"boolean","default":true,"description":"During play, composite the game's UI layer (what the player sees). false reads the raw scene pixels instead."},"refresh":{"type":"boolean","default":true,"description":"Redraw the editor viewport before reading it. Leave on unless you specifically want whatever is already in the buffer."}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FViewport* Viewport = nullptr;
			FString Source;

			// During play, capture through Slate rather than reading the
			// viewport's pixels. A direct ReadPixels returns the rendered scene
			// only - the whole UMG layer is missing, so a menu, a HUD, a quest
			// marker, anything the player actually reads, is invisible. Slate
			// composites the game layer over the scene, which is what the
			// player sees. Verified on a real game whose entire main menu was
			// absent from a ReadPixels capture.
			bool bWantUi = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("include_ui")), bWantUi);

			if (GEditor && GEditor->PlayWorld && bWantUi)
			{
				TArray<FColor> UiPixels;
				FIntPoint UiSize = FIntPoint::ZeroValue;
				if (UplinkSlateScreenshot::TakeGameViewport(UiPixels, UiSize))
				{
					return EncodePng(UiPixels, UiSize, TEXT("pie_game_viewport_with_ui"));
				}
			}

			if (GEditor && GEditor->PlayWorld && GEngine && GEngine->GameViewport)
			{
				Viewport = GEngine->GameViewport->Viewport;
				Source = TEXT("pie_game_viewport");
			}
			else if (GEditor)
			{
				Viewport = GEditor->GetActiveViewport();
				Source = TEXT("editor_viewport");
			}

			if (!Viewport)
			{
				return FUplinkToolResult::Error(TEXT("no viewport available to capture"));
			}

			const FIntPoint Size = Viewport->GetSizeXY();
			if (Size.X <= 0 || Size.Y <= 0)
			{
				return FUplinkToolResult::Error(TEXT("viewport has zero size"));
			}

			// An editor window that is not in front stops repainting, so the
			// pixels still in the buffer are from whenever it last had focus -
			// the capture looks successful and shows the wrong thing. Draw it
			// first. PIE keeps rendering regardless, so it needs no help.
			bool bRefresh = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("refresh")), bRefresh);
			if (bRefresh && Source == TEXT("editor_viewport"))
			{
				Viewport->Draw(/*bShouldPresent=*/true);
			}

			TArray<FColor> Pixels;
			if (!Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
			{
				return FUplinkToolResult::Error(TEXT("viewport pixel readback failed"));
			}

			// Scene readback often carries zero alpha; force opaque for the PNG.
			for (FColor& Pixel : Pixels)
			{
				Pixel.A = 255;
			}

			return EncodePng(Pixels, Size, *Source);
		});

	{
		// A single screenshot is one instant, so anything that happens BETWEEN
		// calls - a fade, a menu sliding in, a door opening, a hit landing - is
		// invisible. This samples N frames on a timer and returns them as one
		// contact sheet, which shows change in a single round trip. Frames are
		// the expensive part of "watching", so they are tiled and downscaled
		// rather than sent individually.
		FUplinkToolInfo Info;
		Info.Name = TEXT("frame_strip");
		Info.Description = TEXT("Watch the game over time: capture N frames at a fixed interval and return them as ONE contact-sheet image, left to right, top to bottom. Use it when the question is 'what changed' rather than 'what is on screen now' - a transition, an animation, a fade, whether a door actually opened. Includes the UI layer during play, like viewport_screenshot.");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"frames":{"type":"number","default":6,"description":"How many frames to capture (2-16)"},"interval_ms":{"type":"number","default":250,"description":"Milliseconds between frames"},"columns":{"type":"number","description":"Tiles per row; defaults to a roughly square sheet"},"scale":{"type":"number","default":0.5,"description":"Downscale each frame (0.1-1.0) so the sheet stays a sane size"}}})json"));
		Info.bReadOnly = true;
		Info.TimeoutSeconds = 120.0;
		Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
		{
			class FFrameStrip final : public IUplinkInvocation
			{
			public:
				virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
				{
					Frames = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("frames"), 6.0)), 2, 16);
					IntervalSeconds = FMath::Clamp(GetNumber(Ctx.Params, TEXT("interval_ms"), 250.0), 30.0, 5000.0) / 1000.0;
					Scale = FMath::Clamp(GetNumber(Ctx.Params, TEXT("scale"), 0.5), 0.1, 1.0);
					Columns = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("columns"),
						FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Frames))))), 1, 8);
					NextAt = 0.0; // capture the first frame immediately
					return EUplinkToolStep::Pending;
				}

				virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
				{
					const double Now = FPlatformTime::Seconds();
					if (Now < NextAt)
					{
						return EUplinkToolStep::Pending;
					}
					NextAt = Now + IntervalSeconds;

					TArray<FColor> Pixels;
					FIntPoint Size = FIntPoint::ZeroValue;
					if (!CaptureOne(Pixels, Size))
					{
						Out = FUplinkToolResult::Error(TEXT("nothing to capture (no viewport)"));
						return EUplinkToolStep::Done;
					}

					// Downscale by simple point sampling: this is for reading
					// what changed, not for pixel-accurate inspection.
					const FIntPoint Small(FMath::Max(1, FMath::RoundToInt(Size.X * Scale)),
						FMath::Max(1, FMath::RoundToInt(Size.Y * Scale)));
					TArray<FColor> Reduced;
					Reduced.SetNumUninitialized(Small.X * Small.Y);
					for (int32 Y = 0; Y < Small.Y; ++Y)
					{
						const int32 SourceY = FMath::Min(Size.Y - 1, FMath::FloorToInt(Y / Scale));
						for (int32 X = 0; X < Small.X; ++X)
						{
							const int32 SourceX = FMath::Min(Size.X - 1, FMath::FloorToInt(X / Scale));
							Reduced[Y * Small.X + X] = Pixels[SourceY * Size.X + SourceX];
						}
					}
					Captured.Add(MoveTemp(Reduced));
					FrameSize = Small;

					if (Captured.Num() < Frames)
					{
						return EUplinkToolStep::Pending;
					}

					const int32 Rows = FMath::CeilToInt(static_cast<float>(Captured.Num()) / Columns);
					const FIntPoint SheetSize(FrameSize.X * Columns, FrameSize.Y * Rows);
					TArray<FColor> Sheet;
					Sheet.Init(FColor(12, 12, 12, 255), SheetSize.X * SheetSize.Y);
					for (int32 i = 0; i < Captured.Num(); ++i)
					{
						const int32 OriginX = (i % Columns) * FrameSize.X;
						const int32 OriginY = (i / Columns) * FrameSize.Y;
						for (int32 Y = 0; Y < FrameSize.Y; ++Y)
						{
							FMemory::Memcpy(
								&Sheet[(OriginY + Y) * SheetSize.X + OriginX],
								&Captured[i][Y * FrameSize.X],
								FrameSize.X * sizeof(FColor));
						}
					}

					Out = EncodePng(Sheet, SheetSize, TEXT("frame_strip"));
					if (Out.Data.IsValid())
					{
						Out.Data->SetNumberField(TEXT("frames"), Captured.Num());
						Out.Data->SetNumberField(TEXT("columns"), Columns);
						Out.Data->SetNumberField(TEXT("intervalMs"), IntervalSeconds * 1000.0);
						Out.Data->SetNumberField(TEXT("spanSeconds"), (Captured.Num() - 1) * IntervalSeconds);
					}
					return EUplinkToolStep::Done;
				}

			private:
				bool CaptureOne(TArray<FColor>& OutPixels, FIntPoint& OutSize)
				{
					// Prefer the Slate path during play so the UI is included.
					if (GEditor && GEditor->PlayWorld
						&& UplinkSlateScreenshot::TakeGameViewport(OutPixels, OutSize))
					{
						return true;
					}
					FViewport* Viewport = (GEditor && GEditor->PlayWorld && GEngine && GEngine->GameViewport)
						? GEngine->GameViewport->Viewport
						: (GEditor ? GEditor->GetActiveViewport() : nullptr);
					if (!Viewport)
					{
						return false;
					}
					OutSize = Viewport->GetSizeXY();
					if (OutSize.X <= 0 || OutSize.Y <= 0 || !Viewport->ReadPixels(OutPixels))
					{
						return false;
					}
					for (FColor& Pixel : OutPixels) { Pixel.A = 255; }
					return true;
				}

				TArray<TArray<FColor>> Captured;
				FIntPoint FrameSize = FIntPoint::ZeroValue;
				int32 Frames = 6;
				int32 Columns = 3;
				double IntervalSeconds = 0.25;
				double Scale = 0.5;
				double NextAt = 0.0;
			};
			return MakeShared<FFrameStrip>();
		});
	}

	Registry.RegisterQuick(
		TEXT("viewport_annotate"),
		TEXT("Screenshot the running game AND report where each matching actor is on screen - name, class, screen-space rect [x,y,w,h], center, distance - so what's visible is grounded in coordinates, not pixel guessing. Off-screen matches are listed with on_screen:false - which means the projected box does not overlap the viewport, not merely that the actor is behind you. A rect is clamped to the viewport and flagged 'clipped' when the actor extends past its edge, or 'partially_behind_camera' when some of it is behind the near plane; in both cases the rect is smaller than the actor. PIE only (uses the player's camera). include_image:false skips the PNG for a cheap 'what can I see' query."),
		TEXT(R"json({"type":"object","properties":{"class_contains":{"type":"string"},"name_contains":{"type":"string"},"max":{"type":"number","default":25},"include_image":{"type":"boolean","default":true}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			UWorld* PieWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
			APlayerController* PC = PieWorld ? PieWorld->GetFirstPlayerController() : nullptr;
			if (!PC)
			{
				return FUplinkToolResult::Error(TEXT("viewport_annotate needs a running PIE session with a player controller"));
			}

			const FString ClassFilter = GetString(Ctx.Params, TEXT("class_contains"));
			const FString NameFilter = GetString(Ctx.Params, TEXT("name_contains"));
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 25)), 1, 200);
			bool bIncludeImage = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("include_image")), bIncludeImage);

			const FVector CameraLocation = PC->PlayerCameraManager
				? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;

			// Needed to answer "is this on screen" honestly. A projection that
			// succeeds only means the point is in FRONT of the camera, not that
			// it landed anywhere a player could see.
			int32 ViewportX = 0, ViewportY = 0;
			PC->GetViewportSize(ViewportX, ViewportY);
			const double ViewportW = ViewportX > 0 ? static_cast<double>(ViewportX) : 0.0;
			const double ViewportH = ViewportY > 0 ? static_cast<double>(ViewportY) : 0.0;

			TArray<TSharedPtr<FJsonValue>> Annotations;
			for (TActorIterator<AActor> It(PieWorld); It && Annotations.Num() < Max; ++It)
			{
				AActor* Actor = *It;
				if (!Actor || Actor == PC->GetPawn() || Actor->IsA<APlayerController>())
				{
					continue;
				}
				if (!ClassFilter.IsEmpty() && !Actor->GetClass()->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (!NameFilter.IsEmpty()
					&& !Actor->GetName().Contains(NameFilter, ESearchCase::IgnoreCase)
					&& !Actor->GetActorLabel().Contains(NameFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (ClassFilter.IsEmpty() && NameFilter.IsEmpty() && !Actor->GetRootComponent())
				{
					continue; // unfiltered sweeps skip abstract/info actors
				}

				FVector Origin, Extent;
				Actor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
				if (Extent.IsNearlyZero())
				{
					Extent = FVector(10.0);
				}

				FVector2D Min(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
				FVector2D MaxPt(TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest());
				int32 ProjectedCorners = 0;
				for (int32 Corner = 0; Corner < 8; ++Corner)
				{
					const FVector WorldCorner(
						Origin.X + ((Corner & 1) ? Extent.X : -Extent.X),
						Origin.Y + ((Corner & 2) ? Extent.Y : -Extent.Y),
						Origin.Z + ((Corner & 4) ? Extent.Z : -Extent.Z));
					FVector2D Screen;
					if (PC->ProjectWorldLocationToScreen(WorldCorner, Screen, /*bPlayerViewportRelative=*/false))
					{
						Min.X = FMath::Min(Min.X, Screen.X);
						Min.Y = FMath::Min(Min.Y, Screen.Y);
						MaxPt.X = FMath::Max(MaxPt.X, Screen.X);
						MaxPt.Y = FMath::Max(MaxPt.Y, Screen.Y);
						++ProjectedCorners;
					}
				}

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Actor->GetName());
				Row->SetStringField(TEXT("label"), Actor->GetActorLabel());
				Row->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
				Row->SetNumberField(TEXT("distance"), FVector::Dist(CameraLocation, Origin));
				// A successful projection is not visibility. ProjectWorldLocation
				// ToScreen fails only for points BEHIND the camera, so anything
				// merely in front of it - including an actor far off to the side -
				// came back on_screen:true with a rect starting at x=12531 on a
				// 2065-wide viewport. And the raw min/max of eight corners spans
				// wildly when the box straddles the frustum edge, which is how a
				// 10452-pixel-wide rect got reported for something two metres
				// across. On screen now means the projected box actually overlaps
				// the viewport.
				const bool bAnyProjected = ProjectedCorners > 0;
				const bool bOverlapsViewport = bAnyProjected && ViewportW > 0.0 && ViewportH > 0.0
					&& MaxPt.X >= 0.0 && MaxPt.Y >= 0.0 && Min.X <= ViewportW && Min.Y <= ViewportH;
				Row->SetBoolField(TEXT("on_screen"), bOverlapsViewport);

				// No rect for something off screen. Clamping one anyway produced a
				// zero-size box pinned to whichever corner it went past, which
				// reads like a real position and is somewhere a caller might aim.
				if (bOverlapsViewport)
				{
					// Clamped to what a viewer could actually point at, since the
					// whole reason to return a rect is so somebody can aim at it.
					const double ClampedMinX = FMath::Clamp(Min.X, 0.0, ViewportW);
					const double ClampedMinY = FMath::Clamp(Min.Y, 0.0, ViewportH);
					const double ClampedMaxX = FMath::Clamp(MaxPt.X, 0.0, ViewportW);
					const double ClampedMaxY = FMath::Clamp(MaxPt.Y, 0.0, ViewportH);

					TArray<TSharedPtr<FJsonValue>> Rect;
					Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(ClampedMinX)));
					Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(ClampedMinY)));
					Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(ClampedMaxX - ClampedMinX)));
					Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(ClampedMaxY - ClampedMinY)));
					Row->SetArrayField(TEXT("rect"), Rect);

					TSharedRef<FJsonObject> Center = MakeShared<FJsonObject>();
					Center->SetNumberField(TEXT("x"), FMath::RoundToInt((ClampedMinX + ClampedMaxX) * 0.5));
					Center->SetNumberField(TEXT("y"), FMath::RoundToInt((ClampedMinY + ClampedMaxY) * 0.5));
					Row->SetObjectField(TEXT("center"), Center);

					// Both say the rect is smaller than the thing it describes,
					// for different reasons, and a caller measuring an actor's
					// apparent size needs to know which it is holding.
					if (Min.X < 0.0 || Min.Y < 0.0 || MaxPt.X > ViewportW || MaxPt.Y > ViewportH)
					{
						Row->SetBoolField(TEXT("clipped"), true);
					}
					if (ProjectedCorners < 8)
					{
						Row->SetBoolField(TEXT("partially_behind_camera"), true);
					}
				}
				Annotations.Add(MakeShared<FJsonValueObject>(Row));
			}

			FUplinkToolResult Out = FUplinkToolResult::Ok();
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("actors"), Annotations);
			// The frame the rects are measured against. Without it a rect is a
			// set of numbers with no scale - "is x=1988 near the edge?" has no
			// answer until you know how wide the viewport is.
			TSharedRef<FJsonObject> ViewportJson = MakeShared<FJsonObject>();
			ViewportJson->SetNumberField(TEXT("width"), ViewportX);
			ViewportJson->SetNumberField(TEXT("height"), ViewportY);
			Data->SetObjectField(TEXT("viewport"), ViewportJson);

			if (bIncludeImage)
			{
				FIntPoint Size;
				FString Source, Error;
				if (!UplinkObservation::CaptureViewportPng(Out.Png, Size, Source, Error))
				{
					return FUplinkToolResult::Error(Error);
				}
				Data->SetNumberField(TEXT("width"), Size.X);
				Data->SetNumberField(TEXT("height"), Size.Y);
			}
			Out.Data = Data;
			return Out;
		});
}
