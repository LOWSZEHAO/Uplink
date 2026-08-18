// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
//
// The JSON <-> reflection boundary, in one place.
//
// Every tool that reads a property, writes one, or calls a function crosses
// this boundary, and the crossing carries a handful of edge cases that each
// cost a debugging session to find: an instanced object reference that expands
// into a cycle, a path that resolves to nothing and is stored as null, a C++
// default argument that a zeroed parameter frame does not reproduce. Written
// inline wherever it happened to be needed, each of those gets fixed at the
// call site that hit it and stays broken in the rest. Conversion belongs here,
// and the tools still calling FJsonObjectConverter directly are candidates to
// move over.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UplinkValue
{
	/**
	 * Read a property as JSON, but report an object reference as its path
	 * instead of serialising the object's contents.
	 *
	 * The converter expands an instanced object property inline, walking into
	 * everything it owns. A UMG widget owns its children and each child points
	 * back at its parent, so that walk never terminates: reading one child
	 * widget overflowed the stack and took the whole editor down. A path is
	 * also the more useful answer, since it can be passed straight back in as
	 * the target of another call.
	 */
	TSharedPtr<FJsonValue> PropertyToJson(FProperty* Property, const void* ValueAddr);

	/**
	 * Write a property from a JSON value.
	 *
	 * Conversion and the reference check are one operation because separating
	 * them is what produces the silent failure: the engine's import accepts a
	 * path that names nothing, stores null and reports success, so a caller
	 * that only converts reports a write that did not happen.
	 */
	bool JsonToProperty(
		const TSharedPtr<FJsonValue>& Value,
		FProperty* Property,
		void* ValueAddr,
		FString& OutError);

	/**
	 * Check, after a conversion, that a hard object reference the caller named
	 * by path came back non-null. Soft references are exempt on purpose -
	 * pointing at something unloaded is what they are for.
	 *
	 * The check itself still lives in UplinkToolUtil, where the tools that have
	 * not adopted this layer yet call it directly. A caller here should not
	 * have to know that, so the operation is named once, here.
	 */
	bool ResolveObjectReference(
		const FProperty* Property,
		const TSharedPtr<FJsonValue>& Supplied,
		const void* ValueAddr,
		FString& OutError);

	/**
	 * Fill a function's parameter frame from an argument map: refuse names that
	 * match no parameter, seed the declared C++ defaults of the ones left out,
	 * convert the rest, and refuse an object argument whose path resolved to
	 * nothing.
	 *
	 * Frame is a zeroed frame the caller owns for the length of the call - an
	 * FStructOnScope over the same UFunction. FunctionLabel is the caller's
	 * spelling of the name and appears in the error text: FindFunction ignores
	 * case, so echoing back what was actually typed is what makes a typo
	 * visible. A null Args means no arguments were given at all.
	 */
	bool JsonToFunctionParams(
		UFunction* Function,
		const TSharedPtr<FJsonObject>& Args,
		void* Frame,
		const FString& FunctionLabel,
		FString& OutError);

	/**
	 * Run the function on Target with the frame, under the guard that makes an
	 * actor sitting in the editor world actually execute it.
	 */
	void InvokeFunction(UObject* Target, UFunction* Function, void* Frame);

	/**
	 * Export a called frame as JSON: the out-parameters plus the literal
	 * 'ReturnValue'.
	 */
	TSharedRef<FJsonObject> FunctionResultToJson(const UFunction* Function, const void* Frame);
}
