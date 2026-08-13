// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Asset tools: asset_search, asset_dependencies, asset_referencers.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "FileHelpers.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

using namespace UplinkToolUtil;

namespace
{
	IAssetRegistry& GetAssetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	FUplinkToolResult PackageNameList(const TArray<FName>& Names, const TCHAR* Field)
	{
		TArray<TSharedPtr<FJsonValue>> Json;
		for (const FName& Name : Names)
		{
			Json.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(Field, Json);
		return FUplinkToolResult::Ok(Data);
	}
}

void UplinkTools::RegisterAssets(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("asset_search"),
		TEXT("Search the asset registry by name substring, optionally filtered by class substring and content path prefix (default /Game)."),
		TEXT(R"json({"type":"object","properties":{"query":{"type":"string","description":"Substring of the asset name (empty lists everything under the path)"},"class_contains":{"type":"string","description":"e.g. Blueprint, Material, StaticMesh"},"path_prefix":{"type":"string","default":"/Game"},"max":{"type":"number","default":100}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Query = GetString(Ctx.Params, TEXT("query"));
			const FString ClassFilter = GetString(Ctx.Params, TEXT("class_contains"));
			const FString PathPrefix = GetString(Ctx.Params, TEXT("path_prefix"), TEXT("/Game"));
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 100)), 1, 1000);

			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*PathPrefix));
			Filter.bRecursivePaths = true;

			TArray<FAssetData> Assets;
			GetAssetRegistry().GetAssets(Filter, Assets);

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Total = 0;
			for (const FAssetData& Asset : Assets)
			{
				if (!Query.IsEmpty() && !Asset.AssetName.ToString().Contains(Query))
				{
					continue;
				}
				const FString ClassName = Asset.AssetClassPath.ToString();
				if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter))
				{
					continue;
				}
				++Total;
				if (Rows.Num() >= Max)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Asset.AssetName.ToString());
				Row->SetStringField(TEXT("object_path"), Asset.GetObjectPathString());
				Row->SetStringField(TEXT("class"), ClassName);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("assets"), Rows);
			Data->SetNumberField(TEXT("total_matching"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Rows.Num());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("asset_dependencies"),
		TEXT("List the packages an asset depends on. 'package' is the package path, e.g. /Game/Maps/Arena."),
		TEXT(R"json({"type":"object","properties":{"package":{"type":"string"}},"required":["package"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			TArray<FName> Dependencies;
			GetAssetRegistry().GetDependencies(FName(*GetString(Ctx.Params, TEXT("package"))), Dependencies,
				UE::AssetRegistry::EDependencyCategory::Package);
			return PackageNameList(Dependencies, TEXT("dependencies"));
		});

	Registry.RegisterQuick(
		TEXT("asset_referencers"),
		TEXT("List the packages that reference an asset. 'package' is the package path, e.g. /Game/BP_Door."),
		TEXT(R"json({"type":"object","properties":{"package":{"type":"string"}},"required":["package"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			TArray<FName> Referencers;
			GetAssetRegistry().GetReferencers(FName(*GetString(Ctx.Params, TEXT("package"))), Referencers,
				UE::AssetRegistry::EDependencyCategory::Package);
			return PackageNameList(Referencers, TEXT("referencers"));
		});

	Registry.RegisterQuick(
		TEXT("asset_import"),
		TEXT("Import a file from disk into the project - FBX/OBJ meshes, textures (png/jpg/tga/exr), audio (wav), and anything else the editor's importers handle - fully automated (no dialogs, sensible defaults). 'file' is an absolute path; 'destination' a /Game/ folder. Set save:true to write the .uasset immediately."),
		TEXT(R"json({"type":"object","properties":{"file":{"type":"string","description":"Absolute source file path"},"destination":{"type":"string","description":"e.g. /Game/Imported"},"name":{"type":"string","description":"Optional asset name (default: file name)"},"save":{"type":"boolean","default":false}},"required":["file","destination"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString File = GetString(Ctx.Params, TEXT("file"));
			if (!FPaths::FileExists(File))
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("file not found: %s"), *File));
			}
			const FString Destination = GetString(Ctx.Params, TEXT("destination"));
			if (!Destination.StartsWith(TEXT("/Game")))
			{
				return FUplinkToolResult::Error(TEXT("'destination' must start with /Game"));
			}

			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = File;
			Task->DestinationPath = Destination;
			Task->DestinationName = GetString(Ctx.Params, TEXT("name"));
			Task->bAutomated = true;
			Task->bReplaceExisting = true;
			bool bSave = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("save")), bSave);
			Task->bSave = bSave;

			FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
			AssetTools.Get().ImportAssetTasks({ Task });

			TArray<TSharedPtr<FJsonValue>> Imported;
			for (const UObject* Object : Task->GetObjects())
			{
				if (Object)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("asset"), Object->GetPathName());
					Row->SetStringField(TEXT("class"), Object->GetClass()->GetName());
					Imported.Add(MakeShared<FJsonValueObject>(Row));
				}
			}
			if (Imported.Num() == 0)
			{
				return FUplinkToolResult::Error(TEXT("import produced no assets (unsupported format or importer error - see output_log)"));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("imported"), Imported);
			Data->SetBoolField(TEXT("saved"), bSave);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("save"),
		TEXT("Write edited assets to disk. Authoring tools leave their work in memory and merely mark it dirty, so anything not saved is lost when the editor closes - blueprint graphs especially. With no arguments this saves everything dirty, including the level; pass 'asset' to save one package by path. 'list_only' reports what is unsaved without writing."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string","description":"Package or object path; omit to save everything dirty"},"list_only":{"type":"boolean","default":false},"include_level":{"type":"boolean","default":true}}})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString AssetPath = GetString(Ctx.Params, TEXT("asset"));
			bool bListOnly = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("list_only")), bListOnly);
			bool bIncludeLevel = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("include_level")), bIncludeLevel);

			TArray<UPackage*> Packages;
			if (!AssetPath.IsEmpty())
			{
				// Accept either a package path or a full object path.
				FString PackageName = AssetPath;
				int32 Dot;
				if (PackageName.FindChar(TEXT('.'), Dot))
				{
					PackageName.LeftInline(Dot);
				}
				UPackage* Package = FindPackage(nullptr, *PackageName);
				if (!Package)
				{
					Package = LoadPackage(nullptr, *PackageName, LOAD_None);
				}
				if (!Package)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no package '%s' is loaded. Pass the path the asset was created at, e.g. /Game/Folder/Asset."),
						*PackageName));
				}
				Packages.Add(Package);
			}
			else
			{
				if (bIncludeLevel)
				{
					FEditorFileUtils::GetDirtyWorldPackages(Packages);
				}
				FEditorFileUtils::GetDirtyContentPackages(Packages);
			}

			TArray<TSharedPtr<FJsonValue>> Names;
			for (const UPackage* Package : Packages)
			{
				Names.Add(MakeShared<FJsonValueString>(Package->GetName()));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(bListOnly ? TEXT("unsaved") : TEXT("saved"), Names);
			Data->SetNumberField(TEXT("count"), Packages.Num());

			if (bListOnly || Packages.Num() == 0)
			{
				return FUplinkToolResult::Ok(Data, Packages.Num() == 0
					? TEXT("nothing to save") : FString());
			}

			// bPromptToSave=false: this runs unattended, a dialog would hang the
			// editor waiting for a click nobody is there to give.
			TArray<UPackage*> Failed;
			const FEditorFileUtils::EPromptReturnCode Result = FEditorFileUtils::PromptForCheckoutAndSave(
				Packages, /*bCheckDirty=*/AssetPath.IsEmpty(), /*bPromptToSave=*/false, &Failed);

			if (Failed.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> FailedNames;
				for (const UPackage* Package : Failed)
				{
					FailedNames.Add(MakeShared<FJsonValueString>(Package->GetName()));
				}
				Data->SetArrayField(TEXT("failed"), FailedNames);
			}

			if (Result != FEditorFileUtils::PR_Success)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("save did not complete (%d of %d package(s) failed) - a read-only file or source control checkout is the usual cause"),
					Failed.Num(), Packages.Num()));
			}
			return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("saved %d package(s)"), Packages.Num()));
		});
}
