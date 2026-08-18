// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

namespace UplinkObject
{
	/**
	 * Resolve a dotted property path such as "MyStruct.Inner.Value", returning the
	 * leaf property and the address of its value. Needed because some engine
	 * structs refuse to serialise as a whole (FPoseSearchBlueprintResult exports
	 * as an empty object), but their individual members read back fine.
	 *
	 * OutOwningObject and OutMemberProperty describe where the value really
	 * lives, which is NOT the target object whenever the path crosses an object
	 * reference: the owner is the object that holds the value, and the member is
	 * the outermost property on it. Both matter to a caller that writes.
	 * Modify() has to record the owner or the edit is not in the transaction and
	 * Ctrl+Z will not bring it back, and PostEditChangeProperty has to name the
	 * member or handlers keyed on it - every USceneComponent transform field -
	 * never run, so the viewport does not move.
	 */
	FProperty* ResolvePropertyPath(
		UObject* Object,
		const FString& Path,
		void*& OutContainer,
		FString& OutError,
		UObject** OutOwningObject = nullptr,
		FProperty** OutMemberProperty = nullptr);
}
