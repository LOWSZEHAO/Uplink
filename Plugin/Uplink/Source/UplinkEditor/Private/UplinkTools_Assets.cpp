// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Asset tools: asset_search, asset_dependencies, asset_referencers,
// asset_create, asset_import, save.

#include "UplinkTools.h"
#include "UplinkCompat.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/Blueprint.h"
#include "Factories/Factory.h"
#include "FileHelpers.h"
#include "JsonObjectConverter.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

using namespace UplinkToolUtil;

namespace
{
	IAssetRegistry& GetAssetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	/** Accept "WidgetBlueprint", "UWidgetBlueprint" or "/Script/UMGEditor.WidgetBlueprint". */
	UClass* ResolveClassSpec(const FString& Spec)
	{
		if (Spec.IsEmpty())
		{
			return nullptr;
		}
		if (Spec.StartsWith(TEXT("/")))
		{
			return StaticLoadClass(UObject::StaticClass(), nullptr, *Spec);
		}
		// NativeFirst: a bare name is a global search, and a Blueprint in the
		// project can share a name with the engine class the caller meant.
		if (UClass* Found = FindFirstObject<UClass>(*Spec, EFindFirstObjectOptions::NativeFirst))
		{
			return Found;
		}
		// Tolerate the C++ prefix, because people type UMaterial and AActor.
		if (Spec.Len() > 1 && (Spec[0] == TEXT('U') || Spec[0] == TEXT('A')))
		{
			return FindFirstObject<UClass>(*Spec.RightChop(1), EFindFirstObjectOptions::NativeFirst);
		}
		return nullptr;
	}

	/**
	 * The same test IAssetTools::CreateAsset runs before it commits, done here so
	 * a mismatch is a returned error rather than an ensure, a modal dialog, or -
	 * on 5.7's widget factory - a check() that takes the editor with it.
	 */
	bool FactoryCanMake(UFactory* Factory, UClass* AssetClass, FString& OutError)
	{
		if (!EnumHasAnyFlags(Factory->GetSupportedWorkflows(), EFactoryCreateWorkflow::Default))
		{
			OutError = FString::Printf(
				TEXT("%s does not support being run directly (it is an import- or wizard-only factory)"),
				*Factory->GetClass()->GetName());
			return false;
		}
		UClass* Supported = Factory->GetSupportedClass();
		const bool bOk = Supported ? AssetClass->IsChildOf(Supported) : Factory->DoesSupportClass(AssetClass);
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("%s makes %s, not %s"),
				*Factory->GetClass()->GetName(),
				Supported ? *Supported->GetName() : TEXT("something else"),
				*AssetClass->GetName());
			return false;
		}
		return true;
	}

	/**
	 * The factory that makes this kind of asset, chosen the way the content
	 * browser's Add menu would. More than one factory can claim a class - UBlueprint
	 * alone is claimed by BlueprintFactory, BlueprintFunctionLibraryFactory,
	 * BlueprintInterfaceFactory and BlueprintMacroFactory - so the one named after
	 * the class wins and the rest are reported, because a silently surprising pick
	 * produces an asset that looks right and is the wrong kind of thing.
	 */
	UClass* PickFactoryClass(UClass* AssetClass, TArray<FString>& OutAlternatives)
	{
		TArray<UClass*> Exact;
		TArray<UClass*> Derived;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class->IsChildOf(UFactory::StaticClass())
				|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}
			UFactory* Cdo = Class->GetDefaultObject<UFactory>();
			if (!Cdo || !Cdo->CanCreateNew())
			{
				continue;
			}
			UClass* Supported = Cdo->GetSupportedClass();
			if (Supported == AssetClass)
			{
				Exact.Add(Class);
			}
			else if (Supported && AssetClass->IsChildOf(Supported))
			{
				Derived.Add(Class);
			}
		}

		TArray<UClass*>& Candidates = Exact.Num() > 0 ? Exact : Derived;
		if (Candidates.Num() == 0)
		{
			return nullptr;
		}

		// Sorted first so the fallback pick is the same every run.
		Candidates.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });

		const FString Preferred = AssetClass->GetName() + TEXT("Factory");
		UClass* Best = nullptr;
		for (UClass* Class : Candidates)
		{
			if (Class->GetName() == Preferred || Class->GetName() == Preferred + TEXT("New"))
			{
				Best = Class;
				break;
			}
		}
		if (!Best)
		{
			Best = Candidates[0];
		}
		for (const UClass* Class : Candidates)
		{
			if (Class != Best)
			{
				OutAlternatives.Add(Class->GetPathName());
			}
		}
		return Best;
	}

	int32 ReadMax(const FUplinkToolContext& Ctx)
	{
		return FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 200.0)), 1, 5000);
	}

	FUplinkToolResult PackageNameList(const TArray<FName>& Names, const TCHAR* Field, int32 Max)
	{
		// A map or a heavily-shared material can pull in thousands of packages,
		// so cap the list and say when it was cut rather than returning a wall
		// of text that buries the answer.
		TArray<TSharedPtr<FJsonValue>> Json;
		for (const FName& Name : Names)
		{
			if (Json.Num() >= Max)
			{
				break;
			}
			Json.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(Field, Json);
		Data->SetNumberField(TEXT("total"), Names.Num());
		Data->SetBoolField(TEXT("truncated"), Names.Num() > Json.Num());
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
		TEXT("List the packages an asset depends on. 'package' is the package path, e.g. /Game/Maps/Arena. The reply carries 'total' and 'truncated', so a capped list is never mistaken for the whole set."),
		TEXT(R"json({"type":"object","properties":{"package":{"type":"string"},"max":{"type":"number","default":200}},"required":["package"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			TArray<FName> Dependencies;
			GetAssetRegistry().GetDependencies(FName(*GetString(Ctx.Params, TEXT("package"))), Dependencies,
				UE::AssetRegistry::EDependencyCategory::Package);
			return PackageNameList(Dependencies, TEXT("dependencies"), ReadMax(Ctx));
		});

	Registry.RegisterQuick(
		TEXT("asset_referencers"),
		TEXT("List the packages that reference an asset. 'package' is the package path, e.g. /Game/BP_Door. The reply carries 'total' and 'truncated', so a capped list is never mistaken for the whole set."),
		TEXT(R"json({"type":"object","properties":{"package":{"type":"string"},"max":{"type":"number","default":200}},"required":["package"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			TArray<FName> Referencers;
			GetAssetRegistry().GetReferencers(FName(*GetString(Ctx.Params, TEXT("package"))), Referencers,
				UE::AssetRegistry::EDependencyCategory::Package);
			return PackageNameList(Referencers, TEXT("referencers"), ReadMax(Ctx));
		});

	Registry.RegisterQuick(
		TEXT("asset_create"),
		TEXT("Create a new empty asset - the kinds bp_create cannot make: Widget Blueprints, Materials, Material Instances, Data Tables, Curves, anything the content browser's Add button offers. 'class' is the asset class, either a short name ('WidgetBlueprint', 'Material', 'MaterialInstanceConstant') or a full /Script/Module.Class path; the factory is picked the way the Add menu would and reported back, with any runners-up, so a surprising pick is visible rather than silent. 'parent_class' sets the base class for Blueprint-shaped factories. 'properties' configures the factory itself - {\"InitialParent\":\"/Game/M_Glass.M_Glass\"} for a material instance, {\"Struct\":\"/Game/Data/S_MyRow.S_MyRow\"} (a struct deriving from FTableRowBase) for a data table. The asset is created dirty in memory, so pass save:true or it is gone on the next editor restart."),
		TEXT(R"json({"type":"object","properties":{"path":{"type":"string","description":"Full asset path including the name, e.g. /Game/UI/WBP_Panel"},"class":{"type":"string","description":"Asset class: short name ('WidgetBlueprint', 'Material') or /Script/Module.Class"},"parent_class":{"type":"string","description":"Base class for Blueprint/WidgetBlueprint/AnimBlueprint factories, e.g. /Script/UMG.UserWidget"},"factory":{"type":"string","description":"Factory class to use, when the automatic pick is wrong"},"properties":{"type":"object","description":"Settings applied to the factory before it runs (name -> value)"},"save":{"type":"boolean","default":false}},"required":["path","class"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Path = GetString(Ctx.Params, TEXT("path"));
			if (!FPackageName::IsValidLongPackageName(Path))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' is not a valid asset path. It must be a content path including the asset name, e.g. /Game/UI/WBP_Panel."),
					*Path));
			}
			// On disk and in memory are different answers, and saying which one
			// it hit matters: an asset created earlier this session and never
			// saved leaves nothing on disk, so "an asset already exists" next to
			// an empty Content folder reads as a bug in the tool.
			const bool bOnDisk = FPackageName::DoesPackageExist(Path);
			const bool bInMemory = StaticFindObject(UObject::StaticClass(), nullptr, *Path) != nullptr;
			if (bOnDisk || bInMemory)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("an asset already exists at %s (%s). Delete it first, or pick another path."),
					*Path,
					bOnDisk ? TEXT("on disk") : TEXT("created earlier this session and not yet saved")));
			}

			const FString ClassSpec = GetString(Ctx.Params, TEXT("class"));
			UClass* AssetClass = ResolveClassSpec(ClassSpec);
			if (!AssetClass)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("class not found: '%s'. Use a short class name like WidgetBlueprint or Material, or a full path like /Script/Engine.Material."),
					*ClassSpec));
			}

			TArray<FString> Alternatives;
			UClass* FactoryClass = nullptr;
			const FString FactorySpec = GetString(Ctx.Params, TEXT("factory"));
			if (!FactorySpec.IsEmpty())
			{
				FactoryClass = ResolveClassSpec(FactorySpec);
				if (!FactoryClass || !FactoryClass->IsChildOf(UFactory::StaticClass()))
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("'%s' is not a UFactory class"), *FactorySpec));
				}
			}
			else
			{
				FactoryClass = PickFactoryClass(AssetClass, Alternatives);
				if (!FactoryClass)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("nothing can create a %s from scratch. Import-only types - textures, meshes, audio - come in through asset_import; otherwise name a 'factory' explicitly."),
						*AssetClass->GetName()));
				}
			}

			UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
			if (!Factory)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("could not instantiate %s"), *FactoryClass->GetName()));
			}

			const FString ParentSpec = GetString(Ctx.Params, TEXT("parent_class"));
			if (!ParentSpec.IsEmpty())
			{
				UClass* ParentClass = ResolveClassSpec(ParentSpec);
				if (!ParentClass)
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("parent_class not found: %s"), *ParentSpec));
				}
				FClassProperty* ParentProperty = CastField<FClassProperty>(
					Factory->GetClass()->FindPropertyByName(FName(TEXT("ParentClass"))));
				if (!ParentProperty)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("%s has no ParentClass, so 'parent_class' means nothing to it"), *FactoryClass->GetName()));
				}
				ParentProperty->SetObjectPropertyValue(
					ParentProperty->ContainerPtrToValuePtr<void>(Factory), ParentClass);
			}

			// Applied one at a time rather than in a single JsonObjectToUStruct
			// call: a name the factory does not have would otherwise be a silent
			// no-op, and an asset path that resolves to nothing would be written
			// as null and reported as success - which is how a material instance
			// ends up parentless and the log stays empty.
			const TSharedPtr<FJsonObject>* FactoryProperties = nullptr;
			if (Ctx.Params->TryGetObjectField(FStringView(TEXT("properties")), FactoryProperties)
				&& FactoryProperties->IsValid())
			{
				for (const auto& Pair : (*FactoryProperties)->Values)
				{
					const FString Name = UplinkCompat::JsonKeyToString(Pair.Key);
					FProperty* Property = Factory->GetClass()->FindPropertyByName(FName(*Name));
					if (!Property)
					{
						TArray<FString> Settable;
						for (TFieldIterator<FProperty> It(Factory->GetClass()); It; ++It)
						{
							if (It->HasAnyPropertyFlags(CPF_Edit))
							{
								Settable.Add(It->GetName());
							}
						}
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("%s has no property '%s'. It accepts: %s"),
							*FactoryClass->GetName(), *Name,
							Settable.Num() ? *FString::Join(Settable, TEXT(", ")) : TEXT("(nothing settable)")));
					}

					void* ValueAddr = Property->ContainerPtrToValuePtr<void>(Factory);
					if (!FJsonObjectConverter::JsonValueToUProperty(Pair.Value, Property, ValueAddr))
					{
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("could not set %s.%s from the value given"), *FactoryClass->GetName(), *Name));
					}
					FString ObjectError;
					if (!NamedObjectResolved(Property, Pair.Value, ValueAddr, ObjectError))
					{
						return FUplinkToolResult::Error(ObjectError);
					}
				}
			}

			FString FactoryError;
			if (!FactoryCanMake(Factory, AssetClass, FactoryError))
			{
				return FUplinkToolResult::Error(FactoryError);
			}

			const FString AssetName = FPackageName::GetShortName(Path);
			const FString PackagePath = FPackageName::GetLongPackagePath(Path);

			// A factory that hits a problem opens a modal dialog, and a modal
			// dialog in an unattended tool is an editor that never answers again.
			// This makes FMessageDialog return its default instead of blocking -
			// the same guard the editor's own scripting layer uses.
			TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);

			FAssetToolsModule& AssetToolsModule =
				FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
			UObject* Created = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, AssetClass, Factory);
			if (!Created)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("%s refused to create a %s at %s. Some factories need configuring first and say nothing ")
					TEXT("when they are not - a DataTable needs properties:{\"Struct\":\"...\"} naming a struct that ")
					TEXT("derives from FTableRowBase, a material instance needs properties:{\"InitialParent\":\"...\"}. ")
					TEXT("Anything else it reported is in output_log."),
					*FactoryClass->GetName(), *AssetClass->GetName(), *Path));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("asset"), Created->GetPathName());
			Data->SetStringField(TEXT("class"), Created->GetClass()->GetPathName());
			Data->SetStringField(TEXT("factory"), FactoryClass->GetName());
			if (Alternatives.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Others;
				for (const FString& Name : Alternatives)
				{
					Others.Add(MakeShared<FJsonValueString>(Name));
				}
				Data->SetArrayField(TEXT("other_factories"), Others);
			}

			if (UBlueprint* AsBlueprint = Cast<UBlueprint>(Created))
			{
				// Always, not just when GeneratedClass is null. A Widget Blueprint
				// comes back with a generated class that was compiled BEFORE its
				// root widget was added, so it looks finished and is not. The
				// content browser hides this by opening the asset editor, which
				// recompiles; nothing here opens an editor.
				FKismetEditorUtilities::CompileBlueprint(AsBlueprint);
				Data->SetStringField(TEXT("generated_class"),
					AsBlueprint->GeneratedClass ? AsBlueprint->GeneratedClass->GetPathName() : TEXT(""));
			}

			bool bSave = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("save")), bSave);
			if (bSave)
			{
				TArray<UPackage*> Packages;
				Packages.Add(Created->GetOutermost());
				bSave = SavePackagesUnattended(Packages, /*bCheckDirty=*/false);
			}
			Data->SetBoolField(TEXT("saved"), bSave);

			return FUplinkToolResult::Ok(Data);
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

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("count"), Packages.Num());

			if (bListOnly || Packages.Num() == 0)
			{
				TArray<TSharedPtr<FJsonValue>> Names;
				for (const UPackage* Package : Packages)
				{
					Names.Add(MakeShared<FJsonValueString>(Package->GetName()));
				}
				Data->SetArrayField(bListOnly ? TEXT("unsaved") : TEXT("saved"), Names);
				return FUplinkToolResult::Ok(Data, Packages.Num() == 0
					? TEXT("nothing to save") : FString());
			}

			// Runs unattended: a dialog would hang the editor waiting for a click
			// nobody is there to give, and take every other tool down with it.
			TArray<UPackage*> Failed;
			const bool bAllSaved = SavePackagesUnattended(Packages, /*bCheckDirty=*/AssetPath.IsEmpty(), &Failed);

			// 'saved' is built AFTER the attempt and excludes the failures, so a
			// package can no longer appear in both lists at once.
			TArray<TSharedPtr<FJsonValue>> SavedNames;
			TArray<TSharedPtr<FJsonValue>> FailedNames;
			for (UPackage* Package : Packages)
			{
				const TSharedRef<FJsonValueString> Name = MakeShared<FJsonValueString>(Package->GetName());
				(Failed.Contains(Package) ? FailedNames : SavedNames).Add(Name);
			}
			Data->SetArrayField(TEXT("saved"), SavedNames);
			if (FailedNames.Num() > 0)
			{
				Data->SetArrayField(TEXT("failed"), FailedNames);
			}

			if (!bAllSaved)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("save did not complete (%d of %d package(s) failed) - a read-only file or source control checkout is the usual cause"),
					Failed.Num(), Packages.Num()));
			}
			return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("saved %d package(s)"), SavedNames.Num()));
		});
}
