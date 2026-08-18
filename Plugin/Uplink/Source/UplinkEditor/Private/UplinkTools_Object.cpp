// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Reflection tools: get_property, set_property, call_function - one file each,
// under Object/, sharing the property-path walk in Object/UplinkObjectPath.h
// and the JSON conversion layer in UplinkValueConverter.h.

#include "UplinkTools.h"

#include "Object/UplinkObjectTools.h"

void UplinkTools::RegisterObject(FUplinkToolRegistry& Registry)
{
	UplinkObject::RegisterGetProperty(Registry);
	UplinkObject::RegisterSetProperty(Registry);
	UplinkObject::RegisterCallFunction(Registry);
}
