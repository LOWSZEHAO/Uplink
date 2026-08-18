// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UplinkTool.h"

/**
 * Name -> tool definition map. Registration happens once at module startup on
 * the game thread; lookups happen on the game thread (HTTP handlers tick there).
 * Other plugins add their own tools through IUplinkToolProvider (see
 * UplinkToolProvider.h) rather than touching this class directly.
 */
class UPLINKEDITOR_API FUplinkToolRegistry
{
public:
	/** Register a tool with a custom invocation factory (latent tools). */
	void Register(FUplinkToolInfo Info, TFunction<TSharedRef<IUplinkInvocation>()> Factory);

	/**
	 * Register a quick tool from a lambda; SchemaJson is a JSON Schema object
	 * as a string literal (parsed once here).
	 *
	 * bTransactional=false is for tools that write but do not make an undoable
	 * edit - undo/redo itself, a compile, a test run. Without it, the only way
	 * to keep such a tool out of a transaction was to declare it read-only,
	 * which then goes out to clients as readOnlyHint and invites them to
	 * auto-approve it.
	 */
	void RegisterQuick(
		const FString& Name,
		const FString& Description,
		const FString& SchemaJson,
		bool bReadOnly,
		TFunction<FUplinkToolResult(const FUplinkToolContext&)> Fn,
		bool bTransactional = true);

	const FUplinkToolDef* Find(const FString& Name) const;
	const TMap<FString, FUplinkToolDef>& All() const { return Tools; }

	/** MCP-shaped tool list: [{name, description, inputSchema, annotations}]. */
	TArray<TSharedPtr<FJsonValue>> BuildMcpToolList() const;

	/**
	 * Stamp the risk traits - destructive, arbitrary-execution, PIE-only,
	 * long-running - onto the registered tools from the single table in
	 * UplinkToolTraits.cpp.
	 *
	 * Call once, after every tool has registered. A tool that arrives later
	 * (a provider plugin loading mid-session) keeps the defaults, which claim
	 * nothing; the table only speaks for tools Uplink itself ships.
	 */
	void ApplyTraits();

	/**
	 * Parse a tool's input schema literal.
	 *
	 * Returns null when the text is not valid JSON, and that is the caller's
	 * cue to drop the tool rather than register it anyway: Register() refuses
	 * an Info with no schema. A tool served without one advertises no
	 * parameters at all, so every option it has becomes undiscoverable and
	 * calls quietly do the default thing.
	 */
	static TSharedPtr<FJsonObject> ParseSchema(const FString& SchemaJson);

	/**
	 * Names of tools that were refused registration because their schema would
	 * not parse. Kept so a later report can distinguish a tool that broke from
	 * a name that never existed.
	 */
	const TSet<FString>& SkippedTools() const { return SkippedSchemaTools; }

	/**
	 * Reject parameters the tool does not declare, then check the ones it does
	 * against the schema. A misspelt or wrong-named parameter is otherwise
	 * ignored in silence and the tool runs with its defaults, which reads as
	 * success while doing something else entirely. A wrongly-typed one is
	 * worse: the tool coerces it and reports confidently on work it did not do.
	 *
	 * The type pass covers the JSON Schema this project actually writes - type,
	 * required, enum, minimum/maximum, minLength/maxLength, items, and nested
	 * properties. Anything else in a schema is left alone, because being unable
	 * to check a value is not the same as the value being wrong.
	 *
	 * Returns false and fills OutError when anything is unrecognised.
	 */
	static bool ValidateParams(
		const FUplinkToolInfo& Info, const TSharedPtr<FJsonObject>& Params, FString& OutError);

private:
	TMap<FString, FUplinkToolDef> Tools;
	TSet<FString> SkippedSchemaTools;
};
