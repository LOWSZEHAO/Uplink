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
	 */
	void RegisterQuick(
		const FString& Name,
		const FString& Description,
		const FString& SchemaJson,
		bool bReadOnly,
		TFunction<FUplinkToolResult(const FUplinkToolContext&)> Fn);

	const FUplinkToolDef* Find(const FString& Name) const;
	const TMap<FString, FUplinkToolDef>& All() const { return Tools; }

	/** MCP-shaped tool list: [{name, description, inputSchema, annotations}]. */
	TArray<TSharedPtr<FJsonValue>> BuildMcpToolList() const;

	static TSharedPtr<FJsonObject> ParseSchema(const FString& SchemaJson);

private:
	TMap<FString, FUplinkToolDef> Tools;
};
