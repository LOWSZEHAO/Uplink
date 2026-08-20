// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Everything that adds a callable to a Blueprint: implemented interfaces,
// overrides of a parent's function, and function graphs of its own.

#include "Blueprint/UplinkBlueprintCommon.h"
#include "UplinkToolUtil.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Interface.h"

using namespace UplinkToolUtil;

namespace UplinkBlueprint
{
	FUplinkToolResult ApplyInterfaceOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data)
	{
		const FString Op = GetString(OpParams, TEXT("op"));

		const FString InterfaceSpec = GetString(OpParams, TEXT("interface"));
		UClass* InterfaceClass = StaticLoadClass(UInterface::StaticClass(), nullptr, *InterfaceSpec);
		if (!InterfaceClass)
		{
			// Accept the asset path too - /Game/BPI_Thing rather than its _C class.
			if (UBlueprint* InterfaceBP = LoadObject<UBlueprint>(nullptr, *InterfaceSpec))
			{
				InterfaceClass = InterfaceBP->GeneratedClass;
			}
		}
		if (!InterfaceClass || !InterfaceClass->IsChildOf(UInterface::StaticClass()))
		{
			return FUplinkToolResult::Error(FString::Printf(
				TEXT("'%s' is not an interface (use /Script/Module.IThing, /Game/BPI_Thing or /Game/BPI_Thing.BPI_Thing_C)"),
				*InterfaceSpec));
		}

		const bool bAlready = Blueprint->ImplementedInterfaces.ContainsByPredicate(
			[InterfaceClass](const FBPInterfaceDescription& Desc) { return Desc.Interface == InterfaceClass; });

		if (Op == TEXT("remove_interface"))
		{
			// RemoveInterface ensure()s on an interface that is not there.
			if (!bAlready)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("%s does not implement %s"), *Blueprint->GetName(), *InterfaceClass->GetName()));
			}
			bool bPreserve = false;
			OpParams->TryGetBoolField(FStringView(TEXT("preserve_functions")), bPreserve);
			FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfaceClass->GetClassPathName(), bPreserve);
			Data->SetStringField(TEXT("removed"), InterfaceClass->GetPathName());
			Data->SetBoolField(TEXT("preserved_functions"), bPreserve);
		}
		else
		{
			// The engine path returns false into a Slate toast nobody sees here.
			if (bAlready)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("%s already implements %s"), *Blueprint->GetName(), *InterfaceClass->GetName()));
			}
			Blueprint->Modify();
			if (!FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceClass->GetClassPathName()))
			{
				return FUplinkToolResult::Error(TEXT(
					"ImplementNewInterface failed - usually a graph in this blueprint already has an interface function's name"));
			}
			// Only functions with outputs - or const/ForceAsFunction ones - get
			// a stub graph. Event-style ones get nothing at all, and the
			// add_node 'event' kind cannot reach them because an interface is
			// not in the super chain - so say which ones still need
			// override_function.
			TArray<FString> AsGraphs, AsEvents;
			for (TFieldIterator<UFunction> It(InterfaceClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				if (UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(*It)) { AsEvents.Add(It->GetName()); }
				else if (UEdGraphSchema_K2::CanKismetOverrideFunction(*It)) { AsGraphs.Add(It->GetName()); }
			}
			Data->SetStringField(TEXT("interface"), InterfaceClass->GetPathName());
			if (AsGraphs.Num())
			{
				Data->SetStringField(TEXT("function_graphs_created"), FString::Join(AsGraphs, TEXT(", ")));
			}
			if (AsEvents.Num())
			{
				Data->SetStringField(TEXT("event_style_use_override_function"), FString::Join(AsEvents, TEXT(", ")));
			}
		}
		return FUplinkToolResult::Ok();
	}

	FUplinkToolResult ApplyOverrideFunctionOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TMap<FString, FString>& NodeRefs, TSharedRef<FJsonObject> Data)
	{
		const FName FuncName(*GetString(OpParams, TEXT("name")));
		if (!Blueprint->SkeletonGeneratedClass)
		{
			return FUplinkToolResult::Error(TEXT("blueprint has no skeleton class - compile it once first"));
		}
		FBlueprintEditorUtils::ConformImplementedInterfaces(Blueprint);

		UFunction* OverrideFunc = nullptr;
		UClass* const OverrideFuncClass =
			FBlueprintEditorUtils::GetOverrideFunctionClass(Blueprint, FuncName, &OverrideFunc);
		if (!OverrideFunc)
		{
			TArray<FString> Candidates;
			for (TFieldIterator<UFunction> It(Blueprint->SkeletonGeneratedClass, EFieldIteratorFlags::IncludeSuper);
				It && Candidates.Num() < 60; ++It)
			{
				if (UEdGraphSchema_K2::CanKismetOverrideFunction(*It))
				{
					Candidates.AddUnique(It->GetName());
				}
			}
			return FUplinkToolResult::Error(FString::Printf(
				TEXT("'%s' is not overridable here. Overridable: %s"),
				*FuncName.ToString(), *FString::Join(Candidates, TEXT(", "))));
		}

		bool bAsFunction = false;
		OpParams->TryGetBoolField(FStringView(TEXT("as_function")), bAsFunction);
		if (!bAsFunction)
		{
			// Editor parity: if a parent BLUEPRINT declared this as a function
			// graph, the override must be a function graph too, not an event.
			TSet<FName> GraphNames;
			FBlueprintEditorUtils::GetAllGraphNames(Blueprint, GraphNames);
			bAsFunction = GraphNames.Contains(FuncName);
		}
		UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);

		bool bPlacedAsEvent = false;
		if (UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(OverrideFunc) && !bAsFunction && EventGraph)
		{
			bPlacedAsEvent = true;
			UK2Node_Event* EventNode = FBlueprintEditorUtils::FindOverrideForFunction(
				Blueprint, OverrideFuncClass, OverrideFunc->GetFName());
			if (!EventNode)
			{
				// GetOverrideFunctionClass resolves to THIS blueprint's own class
				// once the event is implemented - the skeleton declares the
				// function and GetAuthoritativeClass maps it back - and the
				// engine's lookup demands the node's parent class be a child of
				// that, so a second call would never find the node the first call
				// made, and would duplicate it. The editor's Override menu never
				// hits this, because it hides functions that are already
				// overridden. Match the way the compiler does when it reports a
				// duplicate event: by name.
				TArray<UK2Node_Event*> AllEvents;
				FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Event>(Blueprint, AllEvents);
				for (UK2Node_Event* Candidate : AllEvents)
				{
					if (Candidate && Candidate->bOverrideFunction &&
						Candidate->EventReference.GetMemberName() == OverrideFunc->GetFName())
					{
						EventNode = Candidate;
						break;
					}
				}
			}
			if (EventNode)
			{
				EventNode->Modify();
				EventNode->SetEnabledState(ENodeEnabledState::Enabled);
				Data->SetObjectField(TEXT("node"), NodeToJson(EventNode));
				Data->SetBoolField(TEXT("reused"), true);
			}
			else
			{
				UK2Node_Event* Node = NewObject<UK2Node_Event>(EventGraph);
				// The DECLARING class, not ParentClass - this is what makes an
				// interface event reachable at all.
				Node->EventReference.SetExternalMember(OverrideFunc->GetFName(), OverrideFuncClass);
				Node->bOverrideFunction = true;
				FinalizeNewNode(EventGraph, Node, OpParams);
				Data->SetObjectField(TEXT("node"), NodeToJson(Node));
				EventNode = Node;
			}
			// A top-level op never reaches add_node's ref-registration tail.
			const FString Ref = GetString(OpParams, TEXT("ref"));
			if (!Ref.IsEmpty() && EventNode)
			{
				NodeRefs.Add(Ref, EventNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			}
		}
		else
		{
			if (UEdGraph* ExistingGraph = FindObject<UEdGraph>(Blueprint, *FuncName.ToString()))
			{
				Data->SetStringField(TEXT("graph"), ExistingGraph->GetName());
				Data->SetBoolField(TEXT("reused"), true);
			}
			else
			{
				Blueprint->Modify();
				UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
					Blueprint, FuncName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				// bIsUserCreated=false: an override inherits its signature and
				// must NOT be stamped as a fresh user function.
				FBlueprintEditorUtils::AddFunctionGraph<UClass>(
					Blueprint, NewGraph, /*bIsUserCreated=*/false, OverrideFuncClass);
				Data->SetStringField(TEXT("graph"), NewGraph->GetName());
			}
		}
		Data->SetStringField(TEXT("declaring_class"), OverrideFuncClass->GetPathName());
		Data->SetBoolField(TEXT("as_event"), bPlacedAsEvent);
		return FUplinkToolResult::Ok();
	}

	FUplinkToolResult ApplyFunctionOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data)
	{
		FString Error;
		const FString Op = GetString(OpParams, TEXT("op"));

		if (Op == TEXT("add_function"))
		{
			// Creates a real function graph, not just nodes in the event graph.
			// Needed for anything that must run off the game thread - anim graph
			// node functions in particular, which have to be marked thread-safe.
			const FName FuncName(*GetString(OpParams, TEXT("name")));
			if (FuncName.IsNone())
			{
				return FUplinkToolResult::Error(TEXT("'name' is required for add_function"));
			}
			for (UEdGraph* Existing : Blueprint->FunctionGraphs)
			{
				if (Existing && Existing->GetFName() == FuncName)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("a function named '%s' already exists"), *FuncName.ToString()));
				}
			}

			UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, FuncName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!NewGraph)
			{
				return FUplinkToolResult::Error(TEXT("CreateNewGraph failed"));
			}
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, /*bIsUserCreated=*/true, nullptr);

			UK2Node_FunctionEntry* Entry = nullptr;
			for (UEdGraphNode* Node : NewGraph->Nodes)
			{
				if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(Node))
				{
					Entry = AsEntry;
					break;
				}
			}
			if (!Entry)
			{
				return FUplinkToolResult::Error(TEXT("new function has no entry node"));
			}

			bool bThreadSafe = false;
			OpParams->TryGetBoolField(FStringView(TEXT("thread_safe")), bThreadSafe);
			if (bThreadSafe)
			{
				Entry->MetaData.bThreadSafe = true;
			}
			bool bPure = false;
			OpParams->TryGetBoolField(FStringView(TEXT("pure")), bPure);
			if (bPure)
			{
				Entry->SetExtraFlags(Entry->GetExtraFlags() | FUNC_BlueprintPure);
			}
			const FString Category = GetString(OpParams, TEXT("category"));
			if (!Category.IsEmpty())
			{
				Entry->MetaData.Category = FText::FromString(Category);
			}

			// Typed inputs, e.g. the anim node reference an anim graph function receives.
			TArray<FString> AddedInputs;
			const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
			if (OpParams->TryGetArrayField(FStringView(TEXT("inputs")), Inputs))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Inputs)
				{
					const TSharedPtr<FJsonObject>* Obj = nullptr;
					if (!Value->TryGetObject(Obj) || !Obj->IsValid())
					{
						continue;
					}
					const FString PinName = GetString(*Obj, TEXT("name"));
					FEdGraphPinType PinType;
					if (!MakePinType(GetString(*Obj, TEXT("type"), TEXT("float")), PinType, Error))
					{
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("input '%s': %s"), *PinName, *Error));
					}
					// Prototype-matched signatures (anim node bindings) declare their
					// params const-ref; IsSignatureCompatibleWith rejects a by-value
					// pin against them, so these must be settable per input.
					bool bByRef = false;
					(*Obj)->TryGetBoolField(FStringView(TEXT("by_ref")), bByRef);
					bool bConst = false;
					(*Obj)->TryGetBoolField(FStringView(TEXT("const")), bConst);
					PinType.bIsReference = bByRef;
					PinType.bIsConst = bConst;
					Entry->CreateUserDefinedPin(FName(*PinName), PinType, EGPD_Output);
					AddedInputs.Add(PinName);
				}
			}

			// Typed outputs. The return value lives on a Result node - its INPUT
			// pins are the function's outputs - and a graph does not get one
			// until something asks for a return. FindOrCreateFunctionResultNode
			// is the editor's own path: it builds the node, wires the exec pin
			// to the entry, and does not leave the duplicate exec pin a
			// hand-rolled node acquires when the graph is next reconstructed.
			TArray<FString> AddedOutputs;
			const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
			if (OpParams->TryGetArrayField(FStringView(TEXT("outputs")), Outputs) && Outputs->Num() > 0)
			{
				UK2Node_FunctionResult* ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
				if (!ResultNode)
				{
					return FUplinkToolResult::Error(TEXT("could not create the function's Result node"));
				}
				for (const TSharedPtr<FJsonValue>& Value : *Outputs)
				{
					const TSharedPtr<FJsonObject>* Obj = nullptr;
					if (!Value->TryGetObject(Obj) || !Obj->IsValid())
					{
						continue;
					}
					const FString PinName = GetString(*Obj, TEXT("name"), TEXT("ReturnValue"));
					FEdGraphPinType PinType;
					if (!MakePinType(GetString(*Obj, TEXT("type"), TEXT("float")), PinType, Error))
					{
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("output '%s': %s"), *PinName, *Error));
					}
					ResultNode->CreateUserDefinedPin(FName(*PinName), PinType, EGPD_Input);
					AddedOutputs.Add(PinName);
				}
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

			Data->SetStringField(TEXT("function"), FuncName.ToString());
			Data->SetStringField(TEXT("graph"), NewGraph->GetName());
			Data->SetBoolField(TEXT("thread_safe"), bThreadSafe);
			if (AddedInputs.Num())
			{
				Data->SetStringField(TEXT("inputs"), FString::Join(AddedInputs, TEXT(", ")));
			}
			if (AddedOutputs.Num())
			{
				Data->SetStringField(TEXT("outputs"), FString::Join(AddedOutputs, TEXT(", ")));
			}
		}
		else if (Op == TEXT("remove_function"))
		{
			// Without this, abandoned attempts pile up in the blueprint with
			// no way to clear them except opening the editor by hand.
			const FName FunctionName(*GetString(OpParams, TEXT("name")));
			UEdGraph* Target = nullptr;
			TArray<FString> Existing;
			for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
			{
				if (!FunctionGraph)
				{
					continue;
				}
				Existing.Add(FunctionGraph->GetFName().ToString());
				if (FunctionGraph->GetFName() == FunctionName)
				{
					Target = FunctionGraph;
				}
			}
			if (!Target)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("no function '%s' on %s. It has: %s"),
					*FunctionName.ToString(), *Blueprint->GetName(),
					Existing.Num() ? *FString::Join(Existing, TEXT(", ")) : TEXT("(none)")));
			}
			FBlueprintEditorUtils::RemoveGraph(Blueprint, Target, EGraphRemoveFlags::Recompile);
			Data->SetStringField(TEXT("removedFunction"), FunctionName.ToString());
		}
		return FUplinkToolResult::Ok();
	}
}
