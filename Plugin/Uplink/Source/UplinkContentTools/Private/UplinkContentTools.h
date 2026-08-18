// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class FUplinkToolRegistry;

/**
 * Tools whose editor dependencies the Uplink core does not otherwise need -
 * Niagara, MaterialEditor, AnimGraph, Landscape, LevelSequence/MovieScene and
 * AIModule. Keeping them in their own module is what lets someone who only
 * reads a project load the core without any of that behind it.
 *
 * These are registered through IUplinkToolProvider, not called by the core, so
 * the core has no declaration of them anywhere.
 */
namespace UplinkContentTools
{
	void RegisterNiagara(FUplinkToolRegistry& Registry);
	void RegisterMaterial(FUplinkToolRegistry& Registry);
	void RegisterAnim(FUplinkToolRegistry& Registry);
	void RegisterEnvironment(FUplinkToolRegistry& Registry);
	void RegisterSequencer(FUplinkToolRegistry& Registry);
	void RegisterAI(FUplinkToolRegistry& Registry);
}
