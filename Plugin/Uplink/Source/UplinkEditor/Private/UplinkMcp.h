// Copyright (c) 2026 Low Sze Hao. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"

class FUplinkToolRegistry;
class FUplinkTaskManager;
struct FUplinkTask;

/**
 * Native MCP endpoint: JSON-RPC 2.0 over a single POST /mcp route (streamable
 * HTTP transport, JSON-response mode — SSE is optional per spec and is not
 * used). Lets MCP clients connect directly to the editor with no bridge
 * process:  claude mcp add --transport http uplink http://127.0.0.1:3777/mcp
 */
namespace UplinkMcp
{
	bool Handle(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete,
		FUplinkToolRegistry& Registry,
		FUplinkTaskManager& Tasks);

	/** Shared result envelope: {success, status, message, task_id, data?}. */
	TSharedRef<FJsonObject> BuildTaskJson(const FUplinkTask& Task, bool bStillRunning);
}
