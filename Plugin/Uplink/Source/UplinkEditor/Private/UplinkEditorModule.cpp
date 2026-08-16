// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkEditorModule.h"
#include "UplinkEventRecorder.h"
#include "UplinkInputRecorder.h"
#include "UplinkLogCapture.h"
#include "UplinkPieManager.h"
#include "UplinkServer.h"
#include "UplinkTaskManager.h"
#include "UplinkToolProvider.h"
#include "UplinkToolRegistry.h"
#include "UplinkTools.h"
#include "Features/IModularFeatures.h"
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
	Recorder = MakeUnique<FUplinkEventRecorder>();
	InputRecorder = MakeUnique<FUplinkInputRecorder>();

	UplinkTools::RegisterCore(*Registry, *LogCapture, *Tasks);
	UplinkTools::RegisterWorld(*Registry);
	UplinkTools::RegisterObject(*Registry);
	UplinkTools::RegisterAssets(*Registry);
	UplinkTools::RegisterCapture(*Registry);
	UplinkTools::RegisterPie(*Registry, *Pie);
	UplinkTools::RegisterControl(*Registry);
	UplinkTools::RegisterObserve(*Registry, *Recorder);
	UplinkTools::RegisterScenario(*Registry);
	UplinkTools::RegisterBlueprint(*Registry);
	UplinkTools::RegisterReflection(*Registry);
	UplinkTools::RegisterWidget(*Registry);
	UplinkTools::RegisterAnim(*Registry);
	UplinkTools::RegisterNiagara(*Registry);
	UplinkTools::RegisterSlate(*Registry);
	UplinkTools::RegisterDev(*Registry);
	UplinkTools::RegisterRecord(*Registry, *InputRecorder);
	UplinkTools::RegisterEnvironment(*Registry);
	UplinkTools::RegisterData(*Registry);
	UplinkTools::RegisterTests(*Registry);
	UplinkTools::RegisterSequencer(*Registry);
	UplinkTools::RegisterPCG(*Registry);
	UplinkTools::RegisterGASP(*Registry);
	UplinkTools::RegisterMaterial(*Registry);
	UplinkTools::RegisterAI(*Registry);
	UplinkTools::RegisterAutoplay(*Registry);

	// Tools contributed by other plugins: already-loaded providers now, and
	// late-loading ones as they register their modular feature.
	IModularFeatures& ModularFeatures = IModularFeatures::Get();
	for (IUplinkToolProvider* Provider :
		ModularFeatures.GetModularFeatureImplementations<IUplinkToolProvider>(IUplinkToolProvider::GetModularFeatureName()))
	{
		Provider->RegisterUplinkTools(*Registry);
	}
	ProviderRegisteredHandle = ModularFeatures.OnModularFeatureRegistered().AddLambda(
		[this](const FName& Type, IModularFeature* Feature)
		{
			if (Type == IUplinkToolProvider::GetModularFeatureName() && Registry.IsValid() && Feature)
			{
				static_cast<IUplinkToolProvider*>(Feature)->RegisterUplinkTools(*Registry);
			}
		});

	Server = MakeUnique<FUplinkServer>(*Registry, *Tasks);
	if (!Server->Start(GUplinkDefaultPort))
	{
		UE_LOG(LogUplink, Error, TEXT("Uplink server failed to start on port %u"), GUplinkDefaultPort);
		Server.Reset();
	}
}

void FUplinkEditorModule::ShutdownModule()
{
	if (ProviderRegisteredHandle.IsValid())
	{
		IModularFeatures::Get().OnModularFeatureRegistered().Remove(ProviderRegisteredHandle);
		ProviderRegisteredHandle.Reset();
	}
	Server.Reset();
	InputRecorder.Reset();
	Recorder.Reset();
	Pie.Reset();
	Tasks.Reset();
	Registry.Reset();
	LogCapture.Reset();
}

IMPLEMENT_MODULE(FUplinkEditorModule, UplinkEditor)
