// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class FUplinkTaskManager;

/**
 * Keeps Uplink answering while a modal dialog is open.
 *
 * FSlateApplication::AddModalWindow does not return until the dialog closes.
 * It runs its own loop, and that loop ticks the platform, Slate and the
 * renderer but never FTSTicker - so the HTTP server stops accepting, every
 * in-flight tool stops stepping, and the whole plugin looks dead until someone
 * walks over and clicks the dialog. Every request made meanwhile times out.
 *
 * That is not a corner case. "Create a Blueprint" opens the parent-class
 * picker, "Save As", "Add Feature", the asset-name prompts, the delete
 * confirmations - the editor asks a modal question at the start of most things
 * worth automating, and Uplink could not so much as read the dialog it had
 * just opened.
 *
 * Slate leaves a door open for exactly this: ModalLoopTickEvent broadcasts
 * once per iteration of that loop, commented in the engine as "tick any other
 * systems that need to update during modal dialogs". This subscribes to it and
 * ticks the two things Uplink needs - the HTTP server and the task manager -
 * and deliberately nothing else. Ticking the whole core ticker from inside a
 * modal loop would run every async-loading callback and every other plugin's
 * work in a context the engine chose to pause them in.
 */
namespace UplinkModalPump
{
	/** Subscribe. Safe to call when Slate is not initialised; it does nothing. */
	void Initialize(FUplinkTaskManager& InTasks);

	/** Unsubscribe. Must run before the task manager is destroyed. */
	void Shutdown();

	/** True while a modal dialog is open and this pump is what is keeping Uplink alive. */
	bool IsPumping();
}
