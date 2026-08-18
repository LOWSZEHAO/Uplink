// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

// The tool files whose editor dependencies the core does not otherwise need.
// Splitting them out is what lets a project that only reads itself through
// Uplink load the core with none of Niagara, MaterialEditor, AnimGraph,
// Landscape, LevelSequence/MovieScene or AIModule behind it.
//
// Each entry below names the one file that needs it. ImageWrapper is the
// exception: the core keeps its own copy for screenshots, so removing it here
// is not a reason to remove it there.
public class UplinkContentTools : ModuleRules
{
	public UplinkContentTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// UplinkToolUtil.h reaches GEditor, FEditorFileUtils and the
			// subsystem base classes, so the shared helpers cost these two.
			"UnrealEd",
			"EditorSubsystem",
			"Json",
			"JsonUtilities",
			// The registry, the tool types and IUplinkToolProvider.
			"UplinkEditor",

			// UplinkTools_Niagara.cpp
			"Niagara",
			"NiagaraCore",
			"NiagaraEditor",
			// UplinkTools_Material.cpp - MaterialEditingLibrary, and
			// GMaxRHIShaderPlatform to read a material's compiled resource.
			"MaterialEditor",
			"RHI",
			// UplinkTools_Anim.cpp - the state machine graph node types.
			"AnimGraph",
			// UplinkTools_Environment.cpp - ALandscape::Import, and the
			// heightmap decode in front of it.
			"Landscape",
			"ImageWrapper",
			// UplinkTools_Sequencer.cpp
			"LevelSequence",
			"MovieScene",
			// UplinkTools_AI.cpp
			"AIModule",
		});
	}
}
