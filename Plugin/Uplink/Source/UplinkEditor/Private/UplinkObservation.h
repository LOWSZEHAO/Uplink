// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Pieces of a situation report that more than one tool needs.
//
// Each of these is the body of a tool that already exists - the viewport
// capture behind viewport_screenshot, the widget walk behind ui_live. They are
// shared rather than copied because observe now assembles the same facts into
// one reply, and a second copy of either would drift from the tool it came
// from without anything noticing.

#pragma once

#include "CoreMinimal.h"

class UUserWidget;
class UWidget;

namespace UplinkObservation
{
	/**
	 * Viewport pixels as PNG: the PIE game viewport during play, else the
	 * active editor viewport.
	 *
	 * Direct ReadPixels rather than FScreenshotRequest, because the
	 * screenshot-captured delegate does not fire for PIE-in-editor-viewport
	 * sessions.
	 */
	bool CaptureViewportPng(TArray64<uint8>& OutPng, FIntPoint& OutSize, FString& OutSource, FString& OutError);

	/**
	 * Every UWidget under one screen, the contents of nested UserWidgets
	 * included.
	 *
	 * UWidgetTree::ForEachWidget walks panels and named slots, and a nested
	 * UserWidget is neither, so it stops dead at every sub-widget boundary. A
	 * menu assembled from sub-widgets - which is how most of them are built -
	 * listed its outer boxes and hid every button inside them. Seen keeps a
	 * widget reachable from two screens to one row and stops a cycle from
	 * recursing forever.
	 *
	 * Visit is handed the UserWidget whose tree the widget actually lives in,
	 * not the outermost screen: that is the one carrying the variables and
	 * functions a caller would go on to read, and for a button inside a row
	 * widget the two are not the same object.
	 */
	void ForEachWidgetInScreen(UUserWidget* Screen, TSet<UWidget*>& Seen,
		TFunctionRef<void(UWidget*, UUserWidget*)> Visit);

	/**
	 * Every live UUserWidget that is actually on screen, in the given world.
	 *
	 * IsInViewport means AddToViewport or AddToPlayerScreen and nothing else,
	 * so these are the screens a player is looking at - not widgets nested
	 * inside them (walked by ForEachWidgetInScreen) and not ones drawn on a
	 * WidgetComponent in the world, which no viewport ever sees.
	 */
	void GatherLiveWidgets(UWorld* World, TArray<UUserWidget*>& Out);
}
