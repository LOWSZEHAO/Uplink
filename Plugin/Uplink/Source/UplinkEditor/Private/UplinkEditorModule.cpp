// Copyright (c) 2026 Low Sze Hao. MIT License.

#include "UplinkEditorModule.h"
#include "UplinkServer.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogUplink);

static constexpr uint32 GUplinkDefaultPort = 3777;

void FUplinkEditorModule::StartupModule()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	Server = MakeUnique<FUplinkServer>();
	if (!Server->Start(GUplinkDefaultPort))
	{
		UE_LOG(LogUplink, Error, TEXT("Uplink server failed to start on port %u"), GUplinkDefaultPort);
		Server.Reset();
	}
}

void FUplinkEditorModule::ShutdownModule()
{
	if (Server.IsValid())
	{
		Server->Stop();
		Server.Reset();
	}
}

IMPLEMENT_MODULE(FUplinkEditorModule, UplinkEditor)
