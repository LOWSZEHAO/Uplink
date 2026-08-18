// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Making a Blueprint asset (bp_create) and filling in its construction
// script (bp_add_component).

#include "Blueprint/UplinkBlueprintCommon.h"
#include "UplinkCompat.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "JsonObjectConverter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

using namespace UplinkToolUtil;

namespace UplinkBlueprint
{
	void RegisterCreate(FUplinkToolRegistry& Registry)
	{
		Registry.RegisterQuick(
			TEXT("bp_create"),
			TEXT("Create a new Blueprint asset. It exists in memory and is marked dirty - call 'save' to write it to disk, or an editor restart discards it. 'parent_class' is a full class path, e.g. /Script/Engine.Actor or /Script/Engine.Pawn, and defaults to Actor."),
			TEXT(R"json({"type":"object","properties":{"path":{"type":"string","description":"Asset path, e.g. /Game/Tests/BP_Probe"},"parent_class":{"type":"string","default":"/Script/Engine.Actor"}},"required":["path"]})json"),
			/*bReadOnly=*/false,
			[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
			{
				const FString Path = GetString(Ctx.Params, TEXT("path"));
				if (!Path.StartsWith(TEXT("/Game/")))
				{
					return FUplinkToolResult::Error(TEXT("'path' must start with /Game/"));
				}
				if (LoadObject<UBlueprint>(nullptr, *Path))
				{
					return FUplinkToolResult::Error(TEXT("an asset already exists at that path"));
				}

				UClass* Parent = StaticLoadClass(UObject::StaticClass(), nullptr,
					*GetString(Ctx.Params, TEXT("parent_class"), TEXT("/Script/Engine.Actor")));
				if (!Parent)
				{
					return FUplinkToolResult::Error(TEXT("parent_class not found"));
				}

				// This factory makes plain UBlueprints. A UserWidget parent would
				// "work" - and produce an asset with no widget tree, no designer tab
				// and no way to gain either, which reads as everything else being
				// broken. Refuse, and name the tool that does it properly.
				if (Parent->IsChildOf(UUserWidget::StaticClass()))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' is a UserWidget - bp_create would make a plain Blueprint with no widget tree. ")
						TEXT("Use asset_create {class:\"WidgetBlueprint\", parent_class:\"%s\"} instead, then widget_add."),
						*Parent->GetName(), *GetString(Ctx.Params, TEXT("parent_class"))));
				}

				const FString AssetName = FPackageName::GetShortName(Path);
				UPackage* Package = CreatePackage(*Path);
				UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
					Parent, Package, FName(*AssetName), BPTYPE_Normal,
					UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
				if (!Blueprint)
				{
					return FUplinkToolResult::Error(TEXT("CreateBlueprint failed"));
				}
				FAssetRegistryModule::AssetCreated(Blueprint);
				Package->MarkPackageDirty();

				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
				Data->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
				return FUplinkToolResult::Ok(Data);
			});
	}

	void RegisterAddComponent(FUplinkToolRegistry& Registry)
	{
		Registry.RegisterQuick(
			TEXT("bp_add_component"),
			TEXT("Add a component to a Blueprint's construction script (like the editor's Add Component button). 'class': component class - short name ('StaticMeshComponent', 'BoxComponent') or full path. 'parent': attach under this existing component (default: the scene root for scene components). Conveniences on the template: 'location'/'rotation'/'scale' (relative), 'static_mesh' (asset path), 'collision_profile' (e.g. 'OverlapOnlyPawn', 'BlockAll'), plus 'properties' as a generic name->JSON map. The component becomes a Blueprint variable of the same name (usable by bp_modify's component_bound_event after a compile - set compile:true)."),
			TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"},"class":{"type":"string"},"name":{"type":"string"},"parent":{"type":"string"},"location":{"type":"object"},"rotation":{"type":"object"},"scale":{"type":"object"},"static_mesh":{"type":"string"},"collision_profile":{"type":"string"},"properties":{"type":"object"},"compile":{"type":"boolean","default":false}},"required":["blueprint","class","name"]})json"),
			/*bReadOnly=*/false,
			[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
			{
				FString Error;
				UBlueprint* Blueprint = LoadBlueprint(Ctx, Error);
				if (!Blueprint)
				{
					return FUplinkToolResult::Error(Error);
				}
				if (!Blueprint->SimpleConstructionScript)
				{
					return FUplinkToolResult::Error(TEXT("blueprint has no construction script (not an actor blueprint?)"));
				}
				USimpleConstructionScript* Scs = Blueprint->SimpleConstructionScript;

				// Resolve the component class: full path, or short name under /Script/Engine.
				FString ClassSpec = GetString(Ctx.Params, TEXT("class"));
				UClass* Class = nullptr;
				if (ClassSpec.StartsWith(TEXT("/")))
				{
					Class = StaticLoadClass(UActorComponent::StaticClass(), nullptr, *ClassSpec);
				}
				else
				{
					Class = StaticLoadClass(UActorComponent::StaticClass(), nullptr,
						*FString::Printf(TEXT("/Script/Engine.%s"), *ClassSpec));
				}
				if (!Class)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("component class not found: %s (use a short engine name like 'StaticMeshComponent' or a full path like /Script/UMG.WidgetComponent)"), *ClassSpec));
				}
				if (Class->HasAnyClassFlags(CLASS_Abstract))
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("%s is abstract"), *Class->GetName()));
				}

				const FString Name = GetString(Ctx.Params, TEXT("name"));
				if (Name.IsEmpty() || Scs->FindSCSNode(FName(*Name)))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("component name '%s' is empty or already used in this blueprint"), *Name));
				}

				const bool bScene = Class->IsChildOf(USceneComponent::StaticClass());
				const FString ParentName = GetString(Ctx.Params, TEXT("parent"));
				USCS_Node* ParentNode = nullptr;
				if (!ParentName.IsEmpty())
				{
					if (!bScene)
					{
						return FUplinkToolResult::Error(TEXT("'parent' only applies to scene components"));
					}
					ParentNode = Scs->FindSCSNode(FName(*ParentName));
					if (!ParentNode)
					{
						TArray<FString> Names;
						for (const USCS_Node* Existing : Scs->GetAllNodes())
						{
							if (Existing)
							{
								Names.Add(Existing->GetVariableName().ToString());
							}
						}
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("parent component '%s' not found. Components: %s"),
							*ParentName, *FString::Join(Names, TEXT(", "))));
					}
				}

				Blueprint->Modify();
				USCS_Node* Node = Scs->CreateNode(Class, FName(*Name));
				if (!Node)
				{
					return FUplinkToolResult::Error(TEXT("CreateNode failed"));
				}
				if (ParentNode)
				{
					ParentNode->AddChildNode(Node);
				}
				else if (bScene && Scs->GetDefaultSceneRootNode())
				{
					Scs->GetDefaultSceneRootNode()->AddChildNode(Node);
				}
				else
				{
					Scs->AddNode(Node);
				}

				// Apply template setup. Errors past this point report but keep the node.
				TArray<FString> Applied;
				TArray<FString> Failed;
				UActorComponent* Template = Node->ComponentTemplate;

				FVector Location, Scale;
				FRotator Rotation;
				if (USceneComponent* SceneTemplate = Cast<USceneComponent>(Template))
				{
					if (TryGetVector(Ctx.Params, TEXT("location"), Location))
					{
						SceneTemplate->SetRelativeLocation_Direct(Location);
						Applied.Add(TEXT("location"));
					}
					if (TryGetRotator(Ctx.Params, TEXT("rotation"), Rotation))
					{
						SceneTemplate->SetRelativeRotation_Direct(Rotation);
						Applied.Add(TEXT("rotation"));
					}
					if (TryGetVector(Ctx.Params, TEXT("scale"), Scale))
					{
						SceneTemplate->SetRelativeScale3D_Direct(Scale);
						Applied.Add(TEXT("scale"));
					}
				}

				const FString MeshPath = GetString(Ctx.Params, TEXT("static_mesh"));
				if (!MeshPath.IsEmpty())
				{
					UStaticMeshComponent* MeshTemplate = Cast<UStaticMeshComponent>(Template);
					UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
					if (MeshTemplate && Mesh)
					{
						MeshTemplate->SetStaticMesh(Mesh);
						Applied.Add(TEXT("static_mesh"));
					}
					else
					{
						Failed.Add(FString::Printf(TEXT("static_mesh: %s"),
							Mesh ? TEXT("component is not a StaticMeshComponent") : TEXT("mesh asset not found")));
					}
				}

				const FString Profile = GetString(Ctx.Params, TEXT("collision_profile"));
				if (!Profile.IsEmpty())
				{
					if (UPrimitiveComponent* PrimitiveTemplate = Cast<UPrimitiveComponent>(Template))
					{
						PrimitiveTemplate->SetCollisionProfileName(FName(*Profile));
						Applied.Add(TEXT("collision_profile"));
					}
					else
					{
						Failed.Add(TEXT("collision_profile: component is not a PrimitiveComponent"));
					}
				}

				const TSharedPtr<FJsonObject>* Properties = nullptr;
				if (Ctx.Params->TryGetObjectField(FStringView(TEXT("properties")), Properties) && Properties->IsValid())
				{
					for (const auto& Pair : (*Properties)->Values)
					{
						const FString PropertyName = UplinkCompat::JsonKeyToString(Pair.Key);
						FProperty* Property = FindFProperty<FProperty>(Template->GetClass(), *PropertyName);
						if (!Property)
						{
							Failed.Add(FString::Printf(TEXT("%s: no such property on %s"), *PropertyName, *Class->GetName()));
							continue;
						}
						if (FJsonObjectConverter::JsonValueToUProperty(
							Pair.Value, Property, Property->ContainerPtrToValuePtr<void>(Template)))
						{
							Applied.Add(PropertyName);
						}
						else
						{
							Failed.Add(FString::Printf(TEXT("%s: JSON conversion failed"), *PropertyName));
						}
					}
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
				Data->SetStringField(TEXT("component"), Name);
				Data->SetStringField(TEXT("class"), Class->GetName());
				Data->SetStringField(TEXT("parent"), ParentNode
					? ParentNode->GetVariableName().ToString()
					: (bScene && Scs->GetDefaultSceneRootNode() && Node != Scs->GetDefaultSceneRootNode()
						? Scs->GetDefaultSceneRootNode()->GetVariableName().ToString() : TEXT("")));
				if (Applied.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> Values;
					for (const FString& Entry : Applied) { Values.Add(MakeShared<FJsonValueString>(Entry)); }
					Data->SetArrayField(TEXT("applied"), Values);
				}
				if (Failed.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> Values;
					for (const FString& Entry : Failed) { Values.Add(MakeShared<FJsonValueString>(Entry)); }
					Data->SetArrayField(TEXT("failed"), Values);
				}

				bool bCompile = false;
				Ctx.Params->TryGetBoolField(FStringView(TEXT("compile")), bCompile);
				if (bCompile)
				{
					FUplinkToolResult CompileResult = CompileAndReport(Blueprint);
					if (CompileResult.Data.IsValid())
					{
						Data->SetObjectField(TEXT("compile"), CompileResult.Data);
					}
					FUplinkToolResult Out = FUplinkToolResult::Ok(Data, CompileResult.Message);
					Out.bError = CompileResult.bError;
					return Out;
				}
				return FUplinkToolResult::Ok(Data, TEXT("component added (not compiled)"));
			});
	}
}
