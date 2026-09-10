// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class FUplinkToolRegistry;

/**
 * The three-tool front door.
 *
 * A client that asks for the whole tool list gets every schema this server has
 * and carries them for the session. Measured on this project's own tools that
 * is ~34,000 tokens before a single question is asked - on a 200k window, a
 * sixth of it gone, and paid for again on every metered turn.
 *
 * So by default tools/list answers with three tools instead: the areas and the
 * names in them, a way to fetch schemas for the few tools about to be used, and
 * a way to run one. The flat list is still there behind UPLINK_TOOL_LIST=flat
 * for clients that would rather pay once than round-trip.
 *
 * The names are constants because the dispatch layer has to recognise call_tool
 * before any registry lookup - see UplinkDispatch::Begin.
 */
namespace UplinkFacade
{
	extern const FString ListAreasName;
	extern const FString DescribeToolsName;
	extern const FString CallToolName;

	/** True unless UPLINK_TOOL_LIST=flat was set when the editor started. */
	bool IsEnabled();

	/** The three façade entries, MCP-shaped, for tools/list to answer with. */
	TArray<TSharedPtr<FJsonValue>> BuildToolList(const FUplinkToolRegistry& Registry);

	/** Register list_areas and describe_tools. call_tool is handled in dispatch. */
	void RegisterTools(FUplinkToolRegistry& Registry);
}
