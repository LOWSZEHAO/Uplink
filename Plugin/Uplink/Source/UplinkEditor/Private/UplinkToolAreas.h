// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

/** One subject grouping of tools - see the table and its rationale in the .cpp. */
struct FUplinkArea
{
	FString Name;
	FString Summary;
	TArray<FString> Tools;
};

namespace UplinkToolAreas
{
	/** Every area, in the order a project is approached rather than alphabetically. */
	TArray<FUplinkArea> All();

	/** The area a tool belongs to, or empty for a name no row claims. */
	FString AreaOf(const FString& ToolName);
}
