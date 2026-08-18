// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Blueprint tools: bp_create, bp_query, bp_modify, bp_add_component,
// bp_compile. Each is implemented in Blueprint/, one file per subject.

#include "UplinkTools.h"
#include "Blueprint/UplinkBlueprintCommon.h"

void UplinkTools::RegisterBlueprint(FUplinkToolRegistry& Registry)
{
	// Registration order is the order a client sees these tools listed in.
	UplinkBlueprint::RegisterCreate(Registry);
	UplinkBlueprint::RegisterQuery(Registry);
	UplinkBlueprint::RegisterModify(Registry);
	UplinkBlueprint::RegisterAddComponent(Registry);
	UplinkBlueprint::RegisterCompile(Registry);
}
