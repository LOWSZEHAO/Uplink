// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
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

	// FPointerEvent's pressed-button set is a lifetime rule, not a shim, so it
	// is recorded rather than wrapped: 5.8 holds it by value, 5.7 holds a
	// const TSet<FKey>* to whatever the constructor was handed (Events.h:1028).
	// The signatures are identical, so a temporary compiles clean on both and
	// only 5.7 reads freed memory - and only once the event reaches a widget
	// that asks IsMouseButtonDown(), which is most of the editor's lists and
	// trees. Whatever set is passed must outlive every copy of the event: use
	// FTouchKeySet::StandardSet / ::EmptySet for plain left-click, or a named
	// local that outlives the routing call for any other button.

	// UE 5.8 replaced the bIncludeNestedObjects bool on the ForEachObjectWith*
	// and GetObjectsWith* family with an EGetObjectsFlags enum, and deprecated
	// the bool overload. The enum does not exist on 5.7, so the argument itself
	// has to differ; these two name the intent rather than the spelling.
#if UPLINK_UE_AT_LEAST(5, 8)
	inline constexpr EGetObjectsFlags DirectChildrenOnly = EGetObjectsFlags::None;
	inline constexpr EGetObjectsFlags IncludeNestedChildren = EGetObjectsFlags::IncludeNestedObjects;
#else
	inline constexpr bool DirectChildrenOnly = false;
	inline constexpr bool IncludeNestedChildren = true;
#endif
}
