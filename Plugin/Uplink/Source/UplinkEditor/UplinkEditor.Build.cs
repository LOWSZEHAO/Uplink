// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

public class UplinkEditor : ModuleRules
{
	public UplinkEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"HTTPServer",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			"ImageWrapper",
			"LevelEditor",
			"InputCore",
			"EnhancedInput",
			"BlueprintGraph",
			"UMG",
			"UMGEditor",
			"Niagara",
			"NiagaraCore",
			"NiagaraEditor",
			"Slate",
			"SlateCore",
			"EditorSubsystem",
			"AIModule",
			"AssetTools",
			"Landscape",
			"Projects",
			"LevelSequence",
			"MovieScene",
			"AnimGraph",
			"MaterialEditor",
			"RHI",
			// Motion controller components live here. This is an engine
			// runtime module, always present - no XR device or plugin needed
			// to reference the component type.
			"HeadMountedDisplay",
			// UGameMapsSettings - the project's default and startup maps.
			"EngineSettings",
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
