// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// PCG tools: author Procedural Content Generation graphs and run them on actors.
//
// Everything here goes through reflection on purpose. PCG ships disabled by
// default in 5.7 (enabled by default only in 5.8), so linking the PCG module
// would stop UplinkEditor from loading in any project that has the plugin off.
// Reflection keeps the dependency at zero and lets the tools report a clean
// "enable PCG and restart" instead of taking the whole server down with them.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"
#include "UplinkCompat.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "JsonObjectConverter.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

using namespace UplinkToolUtil;

namespace
{
	/** Resolve a PCG class, or explain that the plugin is off. */
	UClass* FindPCGClass(const TCHAR* ClassPath, FString& OutError)
	{
		UClass* Class = FindObject<UClass>(nullptr, ClassPath);
		if (!Class)
		{
			OutError = FString::Printf(
				TEXT("PCG is not available in this editor (could not resolve %s). ")
				TEXT("Enable it with plugin_enable {\"name\":\"PCG\"} and restart the editor - ")
				TEXT("PCG is off by default in UE 5.7."), ClassPath);
		}
		return Class;
	}

	/**
	 * Invoke a UFUNCTION with JSON arguments and export the whole parameter
	 * frame back out (out-params plus 'returnValue'), mirroring call_function.
	 */
	bool CallUFunc(UObject* Target, const TCHAR* FuncName, const TSharedRef<FJsonObject>& Args,
		TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		if (!Target)
		{
			OutError = TEXT("null target");
			return false;
		}
		UFunction* Function = Target->FindFunction(FName(FuncName));
		if (!Function)
		{
			OutError = FString::Printf(TEXT("'%s' not found on %s (PCG API changed?)"),
				FuncName, *Target->GetClass()->GetName());
			return false;
		}

		FStructOnScope Frame(Function);
		FText FailReason;
		if (!FJsonObjectConverter::JsonObjectToUStruct(
			Args, Function, Frame.GetStructMemory(), CPF_Parm, CPF_ReturnParm, /*bStrictMode=*/false, &FailReason))
		{
			OutError = FString::Printf(TEXT("could not map arguments onto %s: %s"), FuncName, *FailReason.ToString());
			return false;
		}

		Target->ProcessEvent(Function, Frame.GetStructMemory());

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		FJsonObjectConverter::UStructToJsonObject(Function, Frame.GetStructMemory(), Out, CPF_Parm, 0);
		OutResult = Out;
		return true;
	}

	/** Case-insensitive field read; the converter emits camelCase keys. */
	FString ResultString(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
	{
		if (!Result.IsValid())
		{
			return FString();
		}
		for (const auto& Pair : Result->Values)
		{
			if (UplinkCompat::JsonKeyToString(Pair.Key).Equals(Field, ESearchCase::IgnoreCase))
			{
				FString Value;
				return Pair.Value->TryGetString(Value) ? Value : FString();
			}
		}
		return FString();
	}

	/** Object references come back as Class'/Path/Name.Name' - unwrap and resolve. */
	UObject* ResolveExportedObject(const FString& Exported)
	{
		if (Exported.IsEmpty() || Exported == TEXT("None"))
		{
			return nullptr;
		}
		FString Path = Exported;
		int32 Open, Close;
		if (Path.FindChar(TEXT('\''), Open) && Path.FindLastChar(TEXT('\''), Close) && Close > Open)
		{
			Path = Path.Mid(Open + 1, Close - Open - 1);
		}
		return StaticFindObject(UObject::StaticClass(), nullptr, *Path);
	}

	/**
	 * Resolve a graph asset by path. StaticFindObject alone only sees objects
	 * that are already in memory, so every graph tool failed against a freshly
	 * started editor until the load fallback was added.
	 */
	UObject* ResolveGraph(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Found = StaticFindObject(UObject::StaticClass(), nullptr, *Path))
		{
			return Found;
		}
		return StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
	}

	/** Collect the graph's nodes without relying on a UPROPERTY name. */
	void GatherNodes(UObject* Graph, UClass* NodeClass, TArray<UObject*>& OutNodes)
	{
		ForEachObjectWithOuter(Graph, [NodeClass, &OutNodes](UObject* Child)
		{
			if (Child->IsA(NodeClass))
			{
				OutNodes.Add(Child);
			}
		}, /*bIncludeNestedObjects=*/false);
	}

	/** Match a node by object name first, then by its node title. */
	UObject* FindNode(UObject* Graph, UClass* NodeClass, const FString& Ref)
	{
		TArray<UObject*> Nodes;
		GatherNodes(Graph, NodeClass, Nodes);
		for (UObject* Node : Nodes)
		{
			if (Node->GetName().Equals(Ref, ESearchCase::IgnoreCase))
			{
				return Node;
			}
		}
		// Fall back to the authored title / settings class name.
		for (UObject* Node : Nodes)
		{
			if (FNameProperty* TitleProp = CastField<FNameProperty>(Node->GetClass()->FindPropertyByName(TEXT("NodeTitle"))))
			{
				const FName Title = TitleProp->GetPropertyValue_InContainer(Node);
				if (!Title.IsNone() && Title.ToString().Equals(Ref, ESearchCase::IgnoreCase))
				{
					return Node;
				}
			}
		}
		return nullptr;
	}

	/** Pin labels for a node, read defensively from its pin arrays. */
	void GatherPinLabels(UObject* Node, const TCHAR* ArrayName, TArray<TSharedPtr<FJsonValue>>& Out)
	{
		FArrayProperty* ArrayProp = CastField<FArrayProperty>(Node->GetClass()->FindPropertyByName(ArrayName));
		if (!ArrayProp)
		{
			return;
		}
		FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(ArrayProp->Inner);
		if (!Inner)
		{
			return;
		}
		FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Node));
		for (int32 i = 0; i < Helper.Num(); ++i)
		{
			UObject* Pin = Inner->GetObjectPropertyValue(Helper.GetRawPtr(i));
			if (!Pin)
			{
				continue;
			}
			FString Label = Pin->GetName();
			// UPCGPin stores its label inside an FPCGPinProperties struct.
			if (FStructProperty* PropsProp = CastField<FStructProperty>(Pin->GetClass()->FindPropertyByName(TEXT("Properties"))))
			{
				void* PropsPtr = PropsProp->ContainerPtrToValuePtr<void>(Pin);
				if (FNameProperty* LabelProp = CastField<FNameProperty>(PropsProp->Struct->FindPropertyByName(TEXT("Label"))))
				{
					const FName PinLabel = LabelProp->GetPropertyValue_InContainer(PropsPtr);
					if (!PinLabel.IsNone())
					{
						Label = PinLabel.ToString();
					}
				}
			}
			Out.Add(MakeShared<FJsonValueString>(Label));
		}
	}

	/**
	 * Accept a full class path, or a friendly name like "SurfaceSampler" /
	 * "StaticMeshSpawner" that gets expanded to UPCGxxxSettings.
	 */
	UClass* ResolveSettingsClass(const FString& Requested, FString& OutError)
	{
		if (Requested.StartsWith(TEXT("/")))
		{
			UClass* Direct = FindObject<UClass>(nullptr, *Requested);
			if (!Direct)
			{
				OutError = FString::Printf(TEXT("settings class not found: %s"), *Requested);
			}
			return Direct;
		}

		UClass* SettingsBase = FindObject<UClass>(nullptr, TEXT("/Script/PCG.PCGSettings"));
		if (!SettingsBase)
		{
			OutError = TEXT("PCG is not available in this editor (enable the PCG plugin and restart)");
			return nullptr;
		}

		const FString Bare = Requested.Replace(TEXT(" "), TEXT(""));
		TArray<FString> Candidates;
		Candidates.Add(FString::Printf(TEXT("PCG%sSettings"), *Bare));
		Candidates.Add(FString::Printf(TEXT("%sSettings"), *Bare));
		Candidates.Add(Bare);

		TArray<FString> Available;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class->IsChildOf(SettingsBase) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}
			for (const FString& Candidate : Candidates)
			{
				if (Class->GetName().Equals(Candidate, ESearchCase::IgnoreCase))
				{
					return Class;
				}
			}
			if (Available.Num() < 30 && Class->GetName().Contains(Bare))
			{
				Available.Add(Class->GetName());
			}
		}

		OutError = FString::Printf(TEXT("no PCG settings class matching '%s'.%s"), *Requested,
			Available.Num() ? *FString::Printf(TEXT(" Close matches: %s"), *FString::Join(Available, TEXT(", "))) : TEXT(""));
		return nullptr;
	}
}

void UplinkTools::RegisterPCG(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("pcg_create"),
		TEXT("Create an empty PCG graph asset. Author it with pcg_add_node / pcg_connect, then run it on an actor with pcg_generate. Requires the PCG plugin (off by default in 5.7 - plugin_enable it and restart)."),
		TEXT(R"json({"type":"object","properties":{"path":{"type":"string","description":"Package folder, e.g. /Game/PCG"},"name":{"type":"string"},"save":{"type":"boolean","default":true}},"required":["path","name"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UClass* GraphClass = FindPCGClass(TEXT("/Script/PCG.PCGGraph"), Error);
			if (!GraphClass)
			{
				return FUplinkToolResult::Error(Error);
			}

			const FString Folder = GetString(Ctx.Params, TEXT("path"));
			const FString Name = GetString(Ctx.Params, TEXT("name"));
			const FString PackageName = FString::Printf(TEXT("%s/%s"), *Folder.TrimChar('/'), *Name).Replace(TEXT("//"), TEXT("/"));
			const FString FullPackage = PackageName.StartsWith(TEXT("/")) ? PackageName : TEXT("/") + PackageName;

			if (StaticFindObject(UObject::StaticClass(), nullptr, *FString::Printf(TEXT("%s.%s"), *FullPackage, *Name)))
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("asset already exists: %s.%s"), *FullPackage, *Name));
			}

			UPackage* Package = CreatePackage(*FullPackage);
			if (!Package)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("could not create package %s"), *FullPackage));
			}
			UObject* Graph = NewObject<UObject>(Package, GraphClass, FName(*Name), RF_Public | RF_Standalone);
			if (!Graph)
			{
				return FUplinkToolResult::Error(TEXT("PCGGraph creation failed"));
			}
			FAssetRegistryModule::AssetCreated(Graph);
			Package->MarkPackageDirty();

			bool bSave = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("save")), bSave);
			bool bSaved = false;
			if (bSave)
			{
				const FString FileName = FPackageName::LongPackageNameToFilename(FullPackage, FPackageName::GetAssetPackageExtension());
				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
				bSaved = UPackage::SavePackage(Package, Graph, *FileName, SaveArgs);
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("graph"), Graph->GetPathName());
			Data->SetBoolField(TEXT("saved"), bSaved);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("pcg_add_node"),
		TEXT("Add a node to a PCG graph. 'settings_class' takes a friendly name (SurfaceSampler, StaticMeshSpawner, TransformPoints, DensityFilter) or a full /Script/PCG.PCGxxxSettings path; unknown names come back with close matches. 'properties' are applied to the node's settings object."),
		TEXT(R"json({"type":"object","properties":{"graph":{"type":"string"},"settings_class":{"type":"string"},"properties":{"type":"object","description":"Settings property name -> JSON value"},"title":{"type":"string","description":"Optional node title"}},"required":["graph","settings_class"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UObject* Graph = ResolveGraph(GetString(Ctx.Params, TEXT("graph")));
			if (!Graph)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("graph not found: %s (pass the full object path, e.g. /Game/PCG/G.G)"),
					*GetString(Ctx.Params, TEXT("graph"))));
			}

			UClass* SettingsClass = ResolveSettingsClass(GetString(Ctx.Params, TEXT("settings_class")), Error);
			if (!SettingsClass)
			{
				return FUplinkToolResult::Error(Error);
			}

			TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("InSettingsClass"), SettingsClass->GetPathName());
			TSharedPtr<FJsonObject> Result;
			if (!CallUFunc(Graph, TEXT("AddNodeOfType"), Args, Result, Error))
			{
				return FUplinkToolResult::Error(Error);
			}

			UObject* Node = ResolveExportedObject(ResultString(Result, TEXT("ReturnValue")));
			UObject* Settings = ResolveExportedObject(ResultString(Result, TEXT("DefaultNodeSettings")));
			if (!Node)
			{
				return FUplinkToolResult::Error(TEXT("AddNodeOfType returned no node"));
			}

			// Apply caller-supplied settings properties.
			TArray<FString> Applied, Rejected;
			const TSharedPtr<FJsonObject>* Props = nullptr;
			if (Settings && Ctx.Params->TryGetObjectField(FStringView(TEXT("properties")), Props) && Props->IsValid())
			{
				for (const auto& Pair : (*Props)->Values)
				{
					const FString PropName = UplinkCompat::JsonKeyToString(Pair.Key);
					FProperty* Property = Settings->GetClass()->FindPropertyByName(FName(*PropName));
					if (!Property)
					{
						Rejected.Add(PropName);
						continue;
					}
					void* Addr = Property->ContainerPtrToValuePtr<void>(Settings);
					if (FJsonObjectConverter::JsonValueToUProperty(Pair.Value, Property, Addr, 0, 0))
					{
						Applied.Add(PropName);
					}
					else
					{
						Rejected.Add(PropName);
					}
				}
				Settings->Modify();
				Settings->PostEditChange();
			}

			const FString Title = GetString(Ctx.Params, TEXT("title"));
			if (!Title.IsEmpty())
			{
				if (FNameProperty* TitleProp = CastField<FNameProperty>(Node->GetClass()->FindPropertyByName(TEXT("NodeTitle"))))
				{
					TitleProp->SetPropertyValue_InContainer(Node, FName(*Title));
				}
			}

			Graph->MarkPackageDirty();

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("node"), Node->GetName());
			Data->SetStringField(TEXT("node_path"), Node->GetPathName());
			Data->SetStringField(TEXT("settings_class"), SettingsClass->GetName());
			if (Settings)
			{
				Data->SetStringField(TEXT("settings_path"), Settings->GetPathName());
			}
			TArray<TSharedPtr<FJsonValue>> InPins, OutPins;
			GatherPinLabels(Node, TEXT("InputPins"), InPins);
			GatherPinLabels(Node, TEXT("OutputPins"), OutPins);
			Data->SetArrayField(TEXT("input_pins"), InPins);
			Data->SetArrayField(TEXT("output_pins"), OutPins);
			if (Applied.Num())
			{
				Data->SetStringField(TEXT("applied"), FString::Join(Applied, TEXT(", ")));
			}
			if (Rejected.Num())
			{
				Data->SetStringField(TEXT("rejected"), FString::Join(Rejected, TEXT(", ")));
			}
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("pcg_connect"),
		TEXT("Wire two PCG nodes together, or unwire them with disconnect:true. Nodes are referenced by the name pcg_add_node returned (or their title); pin labels default to the usual In/Out. Rewiring a pin usually means disconnecting the old edge first."),
		TEXT(R"json({"type":"object","properties":{"graph":{"type":"string"},"from_node":{"type":"string"},"from_pin":{"type":"string","default":"Out"},"to_node":{"type":"string"},"to_pin":{"type":"string","default":"In"},"disconnect":{"type":"boolean","default":false}},"required":["graph","from_node","to_node"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UClass* NodeClass = FindPCGClass(TEXT("/Script/PCG.PCGNode"), Error);
			if (!NodeClass)
			{
				return FUplinkToolResult::Error(Error);
			}
			UObject* Graph = ResolveGraph(GetString(Ctx.Params, TEXT("graph")));
			if (!Graph)
			{
				return FUplinkToolResult::Error(TEXT("graph not found"));
			}

			const FString FromRef = GetString(Ctx.Params, TEXT("from_node"));
			const FString ToRef = GetString(Ctx.Params, TEXT("to_node"));
			UObject* From = FindNode(Graph, NodeClass, FromRef);
			UObject* To = FindNode(Graph, NodeClass, ToRef);
			if (!From || !To)
			{
				TArray<UObject*> Nodes;
				GatherNodes(Graph, NodeClass, Nodes);
				TArray<FString> Names;
				for (UObject* N : Nodes)
				{
					Names.Add(N->GetName());
				}
				return FUplinkToolResult::Error(FString::Printf(TEXT("node not found: %s. Graph has: %s"),
					!From ? *FromRef : *ToRef, *FString::Join(Names, TEXT(", "))));
			}

			FString FromPin = GetString(Ctx.Params, TEXT("from_pin"));
			FString ToPin = GetString(Ctx.Params, TEXT("to_pin"));
			if (FromPin.IsEmpty()) { FromPin = TEXT("Out"); }
			if (ToPin.IsEmpty()) { ToPin = TEXT("In"); }

			bool bDisconnect = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("disconnect")), bDisconnect);

			// RemoveEdge names its pin params differently from AddEdge.
			TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("From"), From->GetPathName());
			Args->SetStringField(bDisconnect ? TEXT("FromLabel") : TEXT("FromPinLabel"), FromPin);
			Args->SetStringField(TEXT("To"), To->GetPathName());
			Args->SetStringField(bDisconnect ? TEXT("ToLabel") : TEXT("ToPinLabel"), ToPin);
			TSharedPtr<FJsonObject> Result;
			if (!CallUFunc(Graph, bDisconnect ? TEXT("RemoveEdge") : TEXT("AddEdge"), Args, Result, Error))
			{
				return FUplinkToolResult::Error(Error);
			}

			// AddEdge returns the "To" node; RemoveEdge returns a bool.
			const FString Returned = ResultString(Result, TEXT("ReturnValue"));
			const bool bChanged = bDisconnect
				? Returned.ToBool() || Returned.Equals(TEXT("true"), ESearchCase::IgnoreCase)
				: (!Returned.IsEmpty() && Returned != TEXT("None"));
			Graph->MarkPackageDirty();

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(bDisconnect ? TEXT("disconnected") : TEXT("connected"), bChanged);
			Data->SetStringField(TEXT("from"), FString::Printf(TEXT("%s.%s"), *From->GetName(), *FromPin));
			Data->SetStringField(TEXT("to"), FString::Printf(TEXT("%s.%s"), *To->GetName(), *ToPin));
			return bChanged
				? FUplinkToolResult::Ok(Data)
				: FUplinkToolResult::Error(FString::Printf(
					TEXT("%s refused %s.%s -> %s.%s (check the pin labels with pcg_query)"),
					bDisconnect ? TEXT("RemoveEdge") : TEXT("AddEdge"),
					*From->GetName(), *FromPin, *To->GetName(), *ToPin));
		});

	Registry.RegisterQuick(
		TEXT("pcg_query"),
		TEXT("Inspect a PCG graph: every node with its settings class, title and pin labels - use it to find the exact pin names before pcg_connect."),
		TEXT(R"json({"type":"object","properties":{"graph":{"type":"string"}},"required":["graph"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UClass* NodeClass = FindPCGClass(TEXT("/Script/PCG.PCGNode"), Error);
			if (!NodeClass)
			{
				return FUplinkToolResult::Error(Error);
			}
			UObject* Graph = ResolveGraph(GetString(Ctx.Params, TEXT("graph")));
			if (!Graph)
			{
				return FUplinkToolResult::Error(TEXT("graph not found"));
			}

			TArray<UObject*> Nodes;
			GatherNodes(Graph, NodeClass, Nodes);

			TArray<TSharedPtr<FJsonValue>> Rows;
			for (UObject* Node : Nodes)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("node"), Node->GetName());
				if (FNameProperty* TitleProp = CastField<FNameProperty>(Node->GetClass()->FindPropertyByName(TEXT("NodeTitle"))))
				{
					const FName Title = TitleProp->GetPropertyValue_InContainer(Node);
					if (!Title.IsNone())
					{
						Row->SetStringField(TEXT("title"), Title.ToString());
					}
				}
				TSharedPtr<FJsonObject> SettingsResult;
				FString CallError;
				if (CallUFunc(Node, TEXT("GetSettings"), MakeShared<FJsonObject>(), SettingsResult, CallError))
				{
					if (UObject* Settings = ResolveExportedObject(ResultString(SettingsResult, TEXT("ReturnValue"))))
					{
						Row->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
						Row->SetStringField(TEXT("settings_path"), Settings->GetPathName());
					}
				}
				TArray<TSharedPtr<FJsonValue>> InPins, OutPins;
				GatherPinLabels(Node, TEXT("InputPins"), InPins);
				GatherPinLabels(Node, TEXT("OutputPins"), OutPins);
				Row->SetArrayField(TEXT("input_pins"), InPins);
				Row->SetArrayField(TEXT("output_pins"), OutPins);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("graph"), Graph->GetPathName());
			Data->SetArrayField(TEXT("nodes"), Rows);
			Data->SetNumberField(TEXT("count"), Rows.Num());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("pcg_generate"),
		TEXT("Run a PCG graph on an actor: adds a PCG component if the actor has none, assigns the graph, and generates. Pair with spawn_actor (/Script/PCG.PCGVolume) to get a bounded volume to populate. cleanup:true clears previously generated content instead."),
		TEXT(R"json({"type":"object","properties":{"actor":{"type":"string"},"graph":{"type":"string"},"cleanup":{"type":"boolean","default":false},"force":{"type":"boolean","default":true},"world":{"type":"string","enum":["editor","pie"]}},"required":["actor"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UClass* ComponentClass = FindPCGClass(TEXT("/Script/PCG.PCGComponent"), Error);
			if (!ComponentClass)
			{
				return FUplinkToolResult::Error(Error);
			}

			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return FUplinkToolResult::Error(WorldError);
			}
			AActor* Actor = FindActor(World, GetString(Ctx.Params, TEXT("actor")));
			if (!Actor)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("actor not found: %s"), *GetString(Ctx.Params, TEXT("actor"))));
			}

			UActorComponent* Component = Actor->FindComponentByClass(ComponentClass);
			bool bCreated = false;
			if (!Component)
			{
				Component = NewObject<UActorComponent>(Actor, ComponentClass, TEXT("UplinkPCGComponent"));
				if (!Component)
				{
					return FUplinkToolResult::Error(TEXT("could not create a PCG component on the actor"));
				}
				Actor->AddInstanceComponent(Component);
				Component->RegisterComponent();
				bCreated = true;
			}

			bool bCleanup = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("cleanup")), bCleanup);
			bool bForce = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("force")), bForce);

			const FString GraphPath = GetString(Ctx.Params, TEXT("graph"));
			if (!GraphPath.IsEmpty())
			{
				UObject* Graph = ResolveGraph(GraphPath);
				if (!Graph)
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("graph not found: %s"), *GraphPath));
				}
				TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
				Args->SetStringField(TEXT("InGraph"), Graph->GetPathName());
				TSharedPtr<FJsonObject> Result;
				if (!CallUFunc(Component, TEXT("SetGraph"), Args, Result, Error))
				{
					return FUplinkToolResult::Error(Error);
				}
			}

			TSharedRef<FJsonObject> RunArgs = MakeShared<FJsonObject>();
			TSharedPtr<FJsonObject> RunResult;
			if (bCleanup)
			{
				RunArgs->SetBoolField(TEXT("bRemoveComponents"), false);
				if (!CallUFunc(Component, TEXT("Cleanup"), RunArgs, RunResult, Error))
				{
					return FUplinkToolResult::Error(Error);
				}
			}
			else
			{
				// GenerateLocal is the plain BlueprintCallable entry point;
				// Generate() is a NetMulticast and needlessly goes through the
				// networking path for an editor-side build.
				RunArgs->SetBoolField(TEXT("bForce"), bForce);
				if (!CallUFunc(Component, TEXT("GenerateLocal"), RunArgs, RunResult, Error))
				{
					return FUplinkToolResult::Error(Error);
				}
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("actor"), Actor->GetName());
			Data->SetBoolField(TEXT("component_created"), bCreated);
			Data->SetStringField(TEXT("action"), bCleanup ? TEXT("cleanup") : TEXT("generate"));
			return FUplinkToolResult::Ok(Data, TEXT("PCG generation is asynchronous - give it a moment, then check level_actors for the spawned content."));
		});
}
