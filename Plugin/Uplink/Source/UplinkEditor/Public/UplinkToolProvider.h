// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"

class FUplinkToolRegistry;

/**
 * Extension point: any plugin can add its own MCP tools to Uplink without
 * Uplink knowing about it. Implement this on a module (or any long-lived
 * object), register it as a modular feature, and Uplink calls back with the
 * registry - at its own startup for already-loaded plugins, or immediately on
 * registration for plugins that load later.
 *
 *   class FMyModule : public IModuleInterface, public IUplinkToolProvider
 *   {
 *       virtual void StartupModule() override
 *       {
 *           IModularFeatures::Get().RegisterModularFeature(
 *               IUplinkToolProvider::GetModularFeatureName(), this);
 *       }
 *       virtual void ShutdownModule() override
 *       {
 *           IModularFeatures::Get().UnregisterModularFeature(
 *               IUplinkToolProvider::GetModularFeatureName(), this);
 *       }
 *       virtual void RegisterUplinkTools(FUplinkToolRegistry& Registry) override
 *       {
 *           Registry.RegisterQuick(TEXT("my_tool"), ...);
 *       }
 *   };
 */
class IUplinkToolProvider : public IModularFeature
{
public:
	static FName GetModularFeatureName()
	{
		static const FName FeatureName(TEXT("UplinkToolProvider"));
		return FeatureName;
	}

	virtual ~IUplinkToolProvider() = default;

	virtual void RegisterUplinkTools(FUplinkToolRegistry& Registry) = 0;
};
