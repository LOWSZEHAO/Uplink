// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkContentToolsModule.h"
#include "UplinkContentTools.h"
#include "Features/IModularFeatures.h"

void FUplinkContentToolsModule::StartupModule()
{
	// This module loads at Default and the core at PostEngineInit, so the
	// feature is already on the list when the core sweeps for providers. The
	// core would pick us up either way - it also listens for providers that
	// arrive later - but only tools registered before it stamps the trait
	// table get their traits, and the table names landscape_create and
	// niagara_compile. Arriving late would strip both and trip the ensure
	// that guards against a trait row losing its tool.
	//
	// Loading first also decides the shutdown order, and that one is not
	// cosmetic. Modules unload in reverse load order, so the core tears its
	// registry down while this DLL is still resident. The registry holds a
	// TFunction per tool whose code lives here; freeing this module first
	// would leave the core destroying callables that point into an unloaded
	// DLL. Do not "tidy" this to PostEngineInit.
	IModularFeatures::Get().RegisterModularFeature(
		IUplinkToolProvider::GetModularFeatureName(), this);
}

void FUplinkContentToolsModule::ShutdownModule()
{
	IModularFeatures::Get().UnregisterModularFeature(
		IUplinkToolProvider::GetModularFeatureName(), this);
}

void FUplinkContentToolsModule::RegisterUplinkTools(FUplinkToolRegistry& Registry)
{
	UplinkContentTools::RegisterNiagara(Registry);
	UplinkContentTools::RegisterMaterial(Registry);
	UplinkContentTools::RegisterAnim(Registry);
	UplinkContentTools::RegisterEnvironment(Registry);
	UplinkContentTools::RegisterSequencer(Registry);
	UplinkContentTools::RegisterAI(Registry);
}

IMPLEMENT_MODULE(FUplinkContentToolsModule, UplinkContentTools)
