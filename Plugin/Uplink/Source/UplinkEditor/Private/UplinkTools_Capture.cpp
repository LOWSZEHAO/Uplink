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
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "UnrealClient.h"

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
}
