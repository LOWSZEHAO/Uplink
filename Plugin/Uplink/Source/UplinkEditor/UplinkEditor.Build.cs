// Copyright (c) 2026 Low Sze Hao. MIT License.

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
		});
	}
}
