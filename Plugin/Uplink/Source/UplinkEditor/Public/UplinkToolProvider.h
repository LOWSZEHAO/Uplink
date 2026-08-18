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
 * Tool names are one flat namespace shared with Uplink's own tools and with
 * every other provider. Registering a name that already exists replaces it, so
 * pick names a second plugin is unlikely to reach for; the registry logs the
 * clash and keeps a record of it, but it cannot serve both.
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
 *       virtual FString GetUplinkProviderName() const override { return TEXT("MyPlugin"); }
 *       virtual FString GetUplinkProviderVersion() const override { return TEXT("1.0.0"); }
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

	/**
	 * Who is registering. The name goes out with this provider's tools in the
	 * tool list, which is the only way a user can tell a tool that arrived with
	 * some plugin from one Uplink ships; the version is recorded with each tool
	 * and named in the log when two providers collide over the same tool name.
	 *
	 * Both are defaulted, so a provider written before provenance existed still
	 * compiles. It is then recorded as an unknown provider - less than a name,
	 * but still not passed off as a built-in.
	 */
	virtual FString GetUplinkProviderName() const { return FString(); }

	virtual FString GetUplinkProviderVersion() const { return FString(); }
};
