// Copyright (c) 2026 Low Sze Hao. MIT License.

#pragma once

#include "CoreMinimal.h"

class FUplinkToolRegistry;
class FUplinkLogCapture;
class FUplinkTaskManager;
class FUplinkPieManager;
class FUplinkEventRecorder;
class FUplinkInputRecorder;

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
	void RegisterReflection(FUplinkToolRegistry& Registry);
	void RegisterWidget(FUplinkToolRegistry& Registry);
	void RegisterAnim(FUplinkToolRegistry& Registry);
	void RegisterNiagara(FUplinkToolRegistry& Registry);
	void RegisterSlate(FUplinkToolRegistry& Registry);
	void RegisterDev(FUplinkToolRegistry& Registry);
	void RegisterRecord(FUplinkToolRegistry& Registry, FUplinkInputRecorder& Recorder);
	void RegisterEnvironment(FUplinkToolRegistry& Registry);
	void RegisterData(FUplinkToolRegistry& Registry);
	void RegisterTests(FUplinkToolRegistry& Registry);
	void RegisterSequencer(FUplinkToolRegistry& Registry);
	void RegisterPCG(FUplinkToolRegistry& Registry);
	void RegisterGASP(FUplinkToolRegistry& Registry);
}
