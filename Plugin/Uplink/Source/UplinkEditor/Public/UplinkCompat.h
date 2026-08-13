// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
//
// Engine-version compatibility shims. Uplink compiles from ONE codebase against
// UE 5.7 and UE 5.8 — every API divergence between the two lives in this header,
// guarded by UPLINK_UE_AT_LEAST, so tool code stays version-agnostic.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UnrealClient.h"

#define UPLINK_UE_AT_LEAST(Major, Minor) \
	(ENGINE_MAJOR_VERSION > (Major) || (ENGINE_MAJOR_VERSION == (Major) && ENGINE_MINOR_VERSION >= (Minor)))

namespace UplinkCompat
{
	// UE 5.8 changed FJsonObject::Values map keys from FString to UE::FSharedString.
	// Both dereference to const TCHAR*, so one template covers both engines; route
	// every Values iteration through this helper instead of touching Pair.Key directly.
	template <typename KeyType>
	FORCEINLINE FString JsonKeyToString(const KeyType& Key)
	{
		return FString(*Key);
	}

	// UE 5.8 added the bInRestrictToGameViewport parameter to
	// FScreenshotRequest::RequestScreenshot. On 5.7 the no-UI request already
	// captures the game viewport when one exists (PIE), so dropping the flag
	// there preserves the intended behavior.
	FORCEINLINE void RequestViewportScreenshot(bool bRestrictToGameViewport)
	{
#if UPLINK_UE_AT_LEAST(5, 8)
		FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/false, bRestrictToGameViewport);
#else
		FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/false);
#endif
	}
}
