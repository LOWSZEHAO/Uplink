// Copyright (c) 2026 Low Sze Hao. MIT License.
// Tidy Blueprint wires: straight lines when pins are level, 90-degree elbows
// otherwise - no splines. Toggle with the console variable uplink.TidyWires.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class FUplinkWireNodeFactory;

/**
 * Blueprint graph panels ask their schema for a wire-drawing policy before
 * consulting any registered factory, so a plugin cannot restyle wires through
 * FEdGraphUtilities alone. SGraphPanel::SetNodeFactory is the sanctioned
 * override point - this class sweeps open windows for graph panels on a slow
 * ticker and installs a factory that only replaces the connection policy
 * (node and pin widgets fall through to the engine defaults).
 */
class FUplinkGraphWires
{
public:
	void Startup();
	void Shutdown();

private:
	bool Sweep(float DeltaTime);

	FTSTicker::FDelegateHandle TickerHandle;
	TSharedPtr<FUplinkWireNodeFactory> Factory;
};
