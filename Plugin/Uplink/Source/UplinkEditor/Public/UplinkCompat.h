// Copyright (c) 2026 Low Sze Hao. MIT License.
//
// Engine-version compatibility shims. Uplink compiles from ONE codebase against
// UE 5.7 and UE 5.8 — every API divergence between the two lives in this header,
// guarded by UPLINK_UE_AT_LEAST, so tool code stays version-agnostic.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"

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
}
