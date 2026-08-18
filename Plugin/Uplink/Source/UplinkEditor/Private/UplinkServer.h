// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"

class FUplinkToolRegistry;
class FUplinkTaskManager;

/**
 * Uplink's HTTP surface inside the editor, localhost-only:
 *   GET  /status        server + editor state
 *   GET  /tools         MCP-shaped tool list
 *   POST /tool/{name}   run a tool (REST; used by the optional bridge)
 *   POST /mcp           native MCP JSON-RPC endpoint (no bridge needed)
 * All handlers run on the game thread via FHttpServerModule's tick; long tool
 * runs defer their HTTP response through the task manager instead of blocking.
 *
 * These four routes are the module's whole entrance, so the request gate lives
 * here and nowhere else: CheckRequest runs first in every handler, including the
 * one that forwards to the MCP endpoint.
 */
class FUplinkServer
{
public:
	FUplinkServer(FUplinkToolRegistry& InRegistry, FUplinkTaskManager& InTasks);
	~FUplinkServer();

	bool Start(uint32 InPort);
	void Stop();
	bool IsRunning() const { return bRunning; }
	uint32 GetPort() const { return Port; }

private:
	/**
	 * Loopback Origin, then the optional bearer token. Answers the request and
	 * returns false when it is refused, so a handler's first line is a guard.
	 */
	bool CheckRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;

	bool HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleListTools(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleRunTool(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	FUplinkToolRegistry& Registry;
	FUplinkTaskManager& Tasks;

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> Routes;

	/**
	 * UPLINK_AUTH_TOKEN as it stood when the server started, empty when the user
	 * set nothing. Read once rather than per request so the required token cannot
	 * change under a caller mid-session, and so the mode logged at startup is
	 * still the mode in force an hour later.
	 */
	FString AuthToken;

	bool bRunning = false;
	uint32 Port = 0;
};
