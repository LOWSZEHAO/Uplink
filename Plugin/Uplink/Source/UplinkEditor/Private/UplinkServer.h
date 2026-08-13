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
	bool HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleListTools(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleRunTool(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	FUplinkToolRegistry& Registry;
	FUplinkTaskManager& Tasks;

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> Routes;

	bool bRunning = false;
	uint32 Port = 0;
};
