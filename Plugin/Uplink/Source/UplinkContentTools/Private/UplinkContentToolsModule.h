// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "UplinkToolProvider.h"
#include "UplinkVersion.h"

/**
 * The first real consumer of IUplinkToolProvider: a second module that adds
 * tools to Uplink without the core knowing this module exists.
 *
 * The split is about what gets loaded. Niagara, MaterialEditor, AnimGraph,
 * Landscape, MovieScene and AIModule are here because only these tools need
 * them; someone who wants to read a project through Uplink now loads none of
 * it, and the core's dependency list is the smaller for it.
 */
class FUplinkContentToolsModule : public IModuleInterface, public IUplinkToolProvider
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	virtual FString GetUplinkProviderName() const override { return TEXT("UplinkContentTools"); }

	// Shipped in the same plugin as the core, so it moves with the same version.
	virtual FString GetUplinkProviderVersion() const override { return UPLINK_VERSION; }

	virtual void RegisterUplinkTools(FUplinkToolRegistry& Registry) override;
};
