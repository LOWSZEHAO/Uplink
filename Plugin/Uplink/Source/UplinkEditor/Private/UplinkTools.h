// Copyright (c) 2026 Low Sze Hao. MIT License.

#pragma once

#include "CoreMinimal.h"

class FUplinkToolRegistry;
class FUplinkLogCapture;
class FUplinkTaskManager;
class FUplinkPieManager;
class FUplinkEventRecorder;

namespace UplinkTools
{
	void RegisterCore(FUplinkToolRegistry& Registry, FUplinkLogCapture& LogCapture, FUplinkTaskManager& Tasks);
	void RegisterWorld(FUplinkToolRegistry& Registry);
	void RegisterObject(FUplinkToolRegistry& Registry);
	void RegisterAssets(FUplinkToolRegistry& Registry);
	void RegisterCapture(FUplinkToolRegistry& Registry);
	void RegisterPie(FUplinkToolRegistry& Registry, FUplinkPieManager& Pie);
	void RegisterControl(FUplinkToolRegistry& Registry);
	void RegisterObserve(FUplinkToolRegistry& Registry, FUplinkEventRecorder& Recorder);
	void RegisterScenario(FUplinkToolRegistry& Registry);
	void RegisterBlueprint(FUplinkToolRegistry& Registry);
}
