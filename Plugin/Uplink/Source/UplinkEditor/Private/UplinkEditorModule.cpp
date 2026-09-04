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
	UplinkTools::RegisterSlate(*Registry);
	UplinkTools::RegisterDev(*Registry);
	UplinkTools::RegisterRecord(*Registry, *InputRecorder);
	UplinkTools::RegisterData(*Registry);
	UplinkTools::RegisterTests(*Registry);
	UplinkTools::RegisterStruct(*Registry);
	UplinkTools::RegisterPCG(*Registry);
	UplinkTools::RegisterGASP(*Registry);
	UplinkTools::RegisterAutoplay(*Registry);
	UplinkTools::RegisterRepair(*Registry);
	UplinkTools::RegisterReferences(*Registry);
	UplinkTools::RegisterSemantic(*Registry, *Recorder);

	// Tools contributed by other modules: already-loaded providers now, and
	// late-loading ones as they register their modular feature. UplinkContentTools
	// is one of these - it ships in this plugin but is not a special case here.
	//
	// RegisterFrom rather than RegisterUplinkTools, so each tool is stamped with
	// the provider that brought it; called directly, they would all read as
	// Uplink's own.
	IModularFeatures& ModularFeatures = IModularFeatures::Get();
	for (IUplinkToolProvider* Provider :
		ModularFeatures.GetModularFeatureImplementations<IUplinkToolProvider>(IUplinkToolProvider::GetModularFeatureName()))
	{
		Registry->RegisterFrom(*Provider);
	}
	ProviderRegisteredHandle = ModularFeatures.OnModularFeatureRegistered().AddLambda(
		[this](const FName& Type, IModularFeature* Feature)
		{
			if (Type == IUplinkToolProvider::GetModularFeatureName() && Registry.IsValid() && Feature)
			{
				Registry->RegisterFrom(*static_cast<IUplinkToolProvider*>(Feature));
			}
		});

	// After every provider has had its turn, so a plugin's own tools are
	// annotated too and a trait naming a tool nobody registered is reported
	// once, here, rather than looking like a missing tool later.
	Registry->ApplyTraits();

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
