// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#include "Rendering/SlateRenderer.h"
#include "Widgets/SViewport.h"

/**
 * A safe wrapper around FSlateApplication::TakeScreenshot.
 *
 * The engine version is a use-after-free waiting to happen. TakeScreenshot does
 * not own or copy anything: it stores a raw TArray<FColor>* in the renderer's
 * long-lived ScreenshotState and returns, and the pixels are written later, on
 * the render thread, by an RDG readback pass. That pass is only queued if the
 * target widget's window is drawn during the same call, and the pointer is only
 * cleared if it was (SlateRHIRenderer.cpp, bScreenshotProcessed). When the window
 * is not drawn - a modal is up, the editor is behind another app, a tab is
 * hidden, a PIE viewport is going away - TakeScreenshot returns false with the
 * array still empty and the pointer still armed. The next time that window is
 * drawn, the readback fires into whatever that address is now. Pointed at a
 * stack local, that is an access violation on the render thread inside D3D12
 * ReadSurfaceData, several seconds and several tool calls after the screenshot
 * that caused it.
 *
 * So: the destination is a buffer that outlives every call, and the renderer is
 * disarmed afterwards whether the capture worked or not. Both the API and the
 * engine behaviour are identical on 5.7 and 5.8, so this needs no version guard.
 */
namespace UplinkSlateScreenshot
{
	inline TArray<FColor>& Buffer()
	{
		static TArray<FColor> Pixels;
		return Pixels;
	}

	/** Re-point Slate's screenshot state at no window, so nothing queued can fire. */
	inline void Disarm()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			// A null window resolves to a null viewport, and the renderer only
			// captures when a viewport it is drawing matches, so this disarms it.
			Renderer->PrepareToTakeScreenshot(FIntRect(), &Buffer(), nullptr);
		}
	}

	/**
	 * Capture a widget through Slate, which composites the UMG layer. False means
	 * Slate produced nothing - callers fall back to raw viewport pixels, or report
	 * the widget as uncapturable.
	 */
	inline bool Take(const TSharedRef<SWidget>& Target, TArray<FColor>& OutPixels, FIntPoint& OutSize)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}

		// TakeScreenshot resolves the widget's path with a check(), so a widget
		// that is not currently rendered asserts and takes the editor with it.
		// The unchecked lookup is the same question without the assert.
		FWidgetPath Unused;
		if (!FSlateApplication::Get().GeneratePathToWidgetUnchecked(Target, Unused))
		{
			return false;
		}

		TArray<FColor>& Pixels = Buffer();
		Pixels.Reset();

		FIntVector Size = FIntVector::ZeroValue;
		const bool bTaken = FSlateApplication::Get().TakeScreenshot(Target, Pixels, Size);

		// Whether it worked or not: leave nothing armed behind us.
		Disarm();

		// ">=", not "==": D3D12 hands back Width * SampleCount rows for an MSAA
		// target, so an exact-count test rejects a perfectly good capture.
		if (!bTaken || Size.X <= 0 || Size.Y <= 0 || Pixels.Num() < Size.X * Size.Y)
		{
			return false;
		}

		OutSize = FIntPoint(Size.X, Size.Y);
		OutPixels = TArray<FColor>(Pixels.GetData(), Size.X * Size.Y);
		for (FColor& Pixel : OutPixels)
		{
			Pixel.A = 255;
		}
		return true;
	}

	/** The running game's viewport, UI included. False outside play. */
	inline bool TakeGameViewport(TArray<FColor>& OutPixels, FIntPoint& OutSize)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}
		const TSharedPtr<SViewport> GameViewport = FSlateApplication::Get().GetGameViewport();
		return GameViewport.IsValid() && Take(GameViewport.ToSharedRef(), OutPixels, OutSize);
	}
}
