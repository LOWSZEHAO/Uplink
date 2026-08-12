// Copyright (c) 2026 Low Sze Hao. MIT License.
// viewport_screenshot — synchronous pixel readback of the active viewport
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

using namespace UplinkToolUtil;

namespace
{
	/** Viewport pixels + PNG, shared by the capture tools. */
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
		TEXT("Capture the active viewport as a PNG image. During PIE this captures the game viewport (what the player sees); otherwise the active editor viewport."),
		TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FViewport* Viewport = nullptr;
			FString Source;

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
			Data->SetStringField(TEXT("source"), Source);
			Out.Data = Data;
			return Out;
		});

	Registry.RegisterQuick(
		TEXT("viewport_annotate"),
		TEXT("Screenshot the running game AND report where each matching actor is on screen - name, class, screen-space rect [x,y,w,h], center, distance - so what's visible is grounded in coordinates, not pixel guessing. Off-screen matches are listed with on_screen:false. PIE only (uses the player's camera). include_image:false skips the PNG for a cheap 'what can I see' query."),
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
				Row->SetBoolField(TEXT("on_screen"), ProjectedCorners > 0);
				if (ProjectedCorners > 0)
				{
					TArray<TSharedPtr<FJsonValue>> Rect;
					Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Min.X)));
					Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Min.Y)));
					Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(MaxPt.X - Min.X)));
					Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(MaxPt.Y - Min.Y)));
					Row->SetArrayField(TEXT("rect"), Rect);
					TSharedRef<FJsonObject> Center = MakeShared<FJsonObject>();
					Center->SetNumberField(TEXT("x"), FMath::RoundToInt((Min.X + MaxPt.X) * 0.5));
					Center->SetNumberField(TEXT("y"), FMath::RoundToInt((Min.Y + MaxPt.Y) * 0.5));
					Row->SetObjectField(TEXT("center"), Center);
				}
				Annotations.Add(MakeShared<FJsonValueObject>(Row));
			}

			FUplinkToolResult Out = FUplinkToolResult::Ok();
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("actors"), Annotations);

			if (bIncludeImage)
			{
				FIntPoint Size;
				FString Source, Error;
				if (!CaptureViewportPng(Out.Png, Size, Source, Error))
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
