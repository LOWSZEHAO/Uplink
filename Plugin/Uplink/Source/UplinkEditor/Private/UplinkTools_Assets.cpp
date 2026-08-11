// Copyright (c) 2026 Low Sze Hao. MIT License.
// Asset tools: asset_search, asset_dependencies, asset_referencers.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"

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
}
