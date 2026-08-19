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
			"Slate",
			"SlateCore",
			"EditorSubsystem",
			"AssetTools",
			"Projects",
			// UGameMapsSettings - the project's default and startup maps.
			"EngineSettings",
			// navigate_to walks the player with SimpleMoveToLocation. The
			// behaviour-tree tools moved to UplinkContentTools and took the rest
			// of AIModule with them; this one call keeps it needed here.
			"AIModule",
			// navigate_to asks the navigation system whether the pawn is even on
			// a navmesh before blaming one for a stall.
			"NavigationSystem",
			// perf_stats reports how close graphics memory is to its budget,
			// which needs GDynamicRHI. Engine exposes the RHI headers publicly
			// but not the globals, so the link has to be declared here.
			"RHI",
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
