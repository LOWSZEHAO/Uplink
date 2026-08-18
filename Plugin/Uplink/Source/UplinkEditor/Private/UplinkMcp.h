// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "Misc/Guid.h"

class FUplinkToolRegistry;
class FUplinkTaskManager;
struct FUplinkTask;

/**
 * Native MCP endpoint: JSON-RPC 2.0 over a single POST /mcp route (streamable
 * HTTP transport, JSON-response mode - SSE is optional per spec and is not
 * used). Lets MCP clients connect directly to the editor with no bridge
 * process:  claude mcp add --transport http uplink http://127.0.0.1:3777/mcp
 *
 * Handle assumes the request already got past FUplinkServer::CheckRequest,
 * which is where the Origin and bearer-token checks live for every route.
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

/**
 * Starting a tool run is the same job on both transports: resolve the name,
 * validate the parameters, work out the two deadlines, submit the task. Kept as
 * two copies it drifted - wait_ms was honoured over REST and silently ignored
 * over MCP, so the same call behaved differently depending on how it arrived.
 *
 * What stays with each transport is framing only: how a refusal is spelt (an
 * HTTP body with success=false, or a JSON-RPC error) and how a finished task is
 * shaped for its client (the image_base64 field, or an MCP image content block).
 */
namespace UplinkDispatch
{
	struct FDispatchOutcome
	{
		/** False when the call was refused before any tool ran. */
		bool bAccepted = false;

		/** Why it was refused; the caller frames it in its own protocol. */
		FString RefusalMessage;

		/** Valid only when accepted. */
		FGuid TaskId;

		/**
		 * How long the transport may hold its response open before answering
		 * "still running" and leaving the caller to poll.
		 */
		double WaitSeconds = 0.0;
	};

	FDispatchOutcome Begin(
		FUplinkToolRegistry& Registry,
		FUplinkTaskManager& Tasks,
		const FString& ToolName,
		const TSharedPtr<FJsonObject>& Params);
}
