// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UplinkTool.h"
#include "UplinkVersion.h"

class IUplinkToolProvider;

/**
 * Which plugin a tool came from.
 *
 * Uplink's own tools all carry the built-in identity; anything a provider
 * registers carries that provider's. Without it, two plugins that both
 * register spawn_actor produce a server where the answer to "whose tool is
 * this" is plugin load order, and nothing says so anywhere.
 */
struct FUplinkToolProvenance
{
	FString Name;
	FString Version;

	/** Registered by Uplink itself rather than by a provider plugin. */
	bool bBuiltIn = false;

	static FUplinkToolProvenance BuiltIn()
	{
		FUplinkToolProvenance Origin;
		Origin.Name = TEXT("Uplink");
		Origin.Version = UPLINK_VERSION;
		Origin.bBuiltIn = true;
		return Origin;
	}
};

/**
 * One tool name registered twice.
 *
 * Kept rather than only logged: a warning scrolls past during editor startup,
 * and the question it answers - why is this tool behaving like somebody
 * else's - gets asked days later.
 */
struct FUplinkToolCollision
{
	FString ToolName;

	/** The registration that was displaced, and is now unreachable. */
	FUplinkToolProvenance Replaced;

	/** The registration that answers calls under this name now. */
	FUplinkToolProvenance Winner;
};

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

	/**
	 * Hand the registry to a provider, attributing every tool it registers
	 * during the call to that provider.
	 *
	 * This is the call site for IUplinkToolProvider. Calling
	 * RegisterUplinkTools() directly still registers the tools, but they are
	 * then stamped as Uplink's own - and a third-party tool that looks
	 * built-in is the exact confusion the provenance is here to remove.
	 */
	void RegisterFrom(IUplinkToolProvider& Provider);

	const FUplinkToolDef* Find(const FString& Name) const;
	const TMap<FString, FUplinkToolDef>& All() const { return Tools; }

	/** Where a tool came from; null for a name that was never registered. */
	const FUplinkToolProvenance* FindProvenance(const FString& Name) const;

	/**
	 * Every name that was registered more than once, in the order the clashes
	 * happened. Empty in the ordinary case, so anything in here is worth
	 * putting in front of a user.
	 */
	const TArray<FUplinkToolCollision>& Collisions() const { return ToolCollisions; }

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

	/**
	 * Kept beside the tools rather than inside FUplinkToolInfo: where a tool
	 * came from is a fact about the registration, not part of the description
	 * its author writes.
	 */
	TMap<FString, FUplinkToolProvenance> ToolProvenance;

	TArray<FUplinkToolCollision> ToolCollisions;

	/**
	 * Stamped onto whatever registers next. RegisterFrom swaps it for the
	 * length of one provider's call and puts it back afterwards, so anything
	 * registered outside such a call is Uplink's own.
	 */
	FUplinkToolProvenance ActiveProvider = FUplinkToolProvenance::BuiltIn();
};
