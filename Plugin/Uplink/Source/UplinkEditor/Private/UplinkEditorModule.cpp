// Copyright (c) 2026 Low Sze Hao. MIT License.

#include "UplinkEditorModule.h"
#include "UplinkLogCapture.h"
#include "UplinkPieManager.h"
#include "UplinkServer.h"
#include "UplinkTaskManager.h"
#include "UplinkToolRegistry.h"
#include "UplinkTools.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogUplink);

static constexpr uint32 GUplinkDefaultPort = 3777;

void FUplinkEditorModule::StartupModule()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	LogCapture = MakeUnique<FUplinkLogCapture>();
	Registry = MakeUnique<FUplinkToolRegistry>();
	Tasks = MakeUnique<FUplinkTaskManager>();
	Pie = MakeUnique<FUplinkPieManager>(LogCapture.Get());

	UplinkTools::RegisterCore(*Registry, *LogCapture, *Tasks);
	UplinkTools::RegisterWorld(*Registry);
	UplinkTools::RegisterObject(*Registry);
	UplinkTools::RegisterAssets(*Registry);
	UplinkTools::RegisterCapture(*Registry);
	UplinkTools::RegisterPie(*Registry, *Pie);
	UplinkTools::RegisterControl(*Registry);

	Server = MakeUnique<FUplinkServer>(*Registry, *Tasks);
	if (!Server->Start(GUplinkDefaultPort))
	{
		UE_LOG(LogUplink, Error, TEXT("Uplink server failed to start on port %u"), GUplinkDefaultPort);
		Server.Reset();
	}
}

void FUplinkEditorModule::ShutdownModule()
{
	Server.Reset();
	Pie.Reset();
	Tasks.Reset();
	Registry.Reset();
	LogCapture.Reset();
}

IMPLEMENT_MODULE(FUplinkEditorModule, UplinkEditor)
