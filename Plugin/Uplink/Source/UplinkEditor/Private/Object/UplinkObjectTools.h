// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class FUplinkToolRegistry;

/**
 * The three reflection tools, one file each. They share a target lookup and a
 * property-path walk and almost nothing else, and call_function carries enough
 * rules of its own - argument checking, C++ defaults, the editor execution
 * guard - to be worth reading without the other two around it.
 *
 * UplinkTools::RegisterObject remains the single entry point, so the module
 * startup call and the order the tools register in are unchanged.
 */
namespace UplinkObject
{
	void RegisterGetProperty(FUplinkToolRegistry& Registry);
	void RegisterSetProperty(FUplinkToolRegistry& Registry);
	void RegisterCallFunction(FUplinkToolRegistry& Registry);
}
