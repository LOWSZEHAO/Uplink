// Copyright (c) 2026 Low Sze Hao. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FUplinkServer;

DECLARE_LOG_CATEGORY_EXTERN(LogUplink, Log, All);

class FUplinkEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	TUniquePtr<FUplinkServer> Server;
};
