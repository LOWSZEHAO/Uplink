// Copyright (c) 2026 Low Sze Hao. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"

/**
 * Uplink's HTTP surface inside the editor: localhost-only REST endpoints the
 * external MCP bridge talks to. Phase 0 exposes /status and /tools; the tool
 * registry, dispatcher, and task queue arrive in Phase 1 (see docs/PLAN.md).
 */
class FUplinkServer
{
public:
	~FUplinkServer();

	bool Start(uint32 InPort);
	void Stop();
	bool IsRunning() const { return bRunning; }
	uint32 GetPort() const { return Port; }

private:
	bool HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleListTools(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle StatusHandle;
	FHttpRouteHandle ToolsHandle;

	bool bRunning = false;
	uint32 Port = 0;
};
