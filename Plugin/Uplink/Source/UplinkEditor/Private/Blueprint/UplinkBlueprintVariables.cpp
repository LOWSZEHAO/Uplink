// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Blueprint member state: variables and their details-panel knobs, and event
// dispatchers - which are a variable plus a signature graph.

#include "Blueprint/UplinkBlueprintCommon.h"
#include "UplinkToolUtil.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"

using namespace UplinkToolUtil;

namespace UplinkBlueprint
{
	FUplinkToolResult ApplyVariableOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data)
	{
		FString Error;
		const FString Op = GetString(OpParams, TEXT("op"));

		if (Op == TEXT("add_variable") || Op == TEXT("set_variable"))
		{
			const FName VarName(*GetString(OpParams, TEXT("name")));

			if (Op == TEXT("add_variable"))
			{
				FEdGraphPinType PinType;
				if (!MakePinType(GetString(OpParams, TEXT("type"), TEXT("float")), PinType, Error))
				{
					return FUplinkToolResult::Error(Error);
				}
				if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType, GetString(OpParams, TEXT("default"))))
				{
					return FUplinkToolResult::Error(TEXT("AddMemberVariable failed (name collision?)"));
				}
			}
			else
			{
				if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) == INDEX_NONE)
				{
					TArray<FString> Names;
					for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
					{
						Names.Add(Variable.VarName.ToString());
					}
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no variable '%s' on this blueprint. Variables: %s"),
						*VarName.ToString(), *FString::Join(Names, TEXT(", "))));
				}
				// set_variable changes the details-panel knobs below; 'type' and
				// 'default' are read only by add_variable, which passes them to
				// AddMemberVariable. Accepting them here dropped them in silence
				// and answered success, so a call meant to retune a variable
				// reported having done it and changed nothing.
				if (OpParams->HasField(TEXT("type")) || OpParams->HasField(TEXT("default")))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("set_variable cannot change a variable's type or default value - it sets the ")
						TEXT("details-panel flags (instance_editable, expose_on_spawn, category, tooltip). ")
						TEXT("'%s' already exists; remove it and add_variable it again to change its shape."),
						*VarName.ToString()));
				}
			}

			// The knobs on the variable details panel, addressable on creation or
			// afterwards. Without these, every variable an agent made was private,
			// uncategorised and unspawnable - fine for a scratch counter, wrong
			// for anything a designer is meant to touch.
			bool bFlag = false;
			if (OpParams->TryGetBoolField(FStringView(TEXT("instance_editable")), bFlag))
			{
				// The engine name is inverted history: "blueprint only" means NOT
				// editable on placed instances.
				FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, !bFlag);
			}
			if (OpParams->TryGetBoolField(FStringView(TEXT("expose_on_spawn")), bFlag))
			{
				if (bFlag)
				{
					FBlueprintEditorUtils::SetBlueprintVariableMetaData(
						Blueprint, VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
				}
				else
				{
					FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(
						Blueprint, VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn);
				}
			}
			if (OpParams->TryGetBoolField(FStringView(TEXT("read_only")), bFlag))
			{
				FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, VarName, bFlag);
			}
			const FString Category = GetString(OpParams, TEXT("category"));
			if (!Category.IsEmpty())
			{
				FBlueprintEditorUtils::SetBlueprintVariableCategory(
					Blueprint, VarName, nullptr, FText::FromString(Category), /*bDontRecompile=*/true);
			}
			const FString Tooltip = GetString(OpParams, TEXT("tooltip"));
			if (!Tooltip.IsEmpty())
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(
					Blueprint, VarName, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);
			}
			if (OpParams->TryGetBoolField(FStringView(TEXT("replicated")), bFlag))
			{
				const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
				if (VarIndex != INDEX_NONE)
				{
					if (bFlag)
					{
						Blueprint->NewVariables[VarIndex].PropertyFlags |= CPF_Net;
					}
					else
					{
						Blueprint->NewVariables[VarIndex].PropertyFlags &= ~CPF_Net;
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				}
			}

			Data->SetStringField(TEXT("variable"), VarName.ToString());
		}
		else if (Op == TEXT("remove_variable"))
		{
			// Removing a name that was never there is not success - it usually
			// means the name was typed wrong, and reporting OK sends the caller
			// looking for the problem somewhere else.
			const FName VarName(*GetString(OpParams, TEXT("name")));
			if (FBlueprintEditorUtils::FindMemberVariableGuidByName(Blueprint, VarName) == FGuid())
			{
				TArray<FString> Existing;
				for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
				{
					Existing.Add(Variable.VarName.ToString());
				}
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("no variable '%s' on %s. It has: %s"),
					*VarName.ToString(), *Blueprint->GetName(),
					Existing.Num() ? *FString::Join(Existing, TEXT(", ")) : TEXT("(none)")));
			}
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);
			Data->SetStringField(TEXT("removed"), VarName.ToString());
		}
		return FUplinkToolResult::Ok();
	}

	FUplinkToolResult ApplyAddDispatcherOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data)
	{
		FString Error;

		// An event dispatcher is two halves the compiler joins BY NAME: a
		// member variable of multicast-delegate type, and a signature graph
		// carrying the identical FName. Mirrors FBlueprintEditor::OnAddNewDelegate.
		const FName DispatcherName(*GetString(OpParams, TEXT("name")));
		if (DispatcherName.IsNone())
		{
			return FUplinkToolResult::Error(TEXT("add_dispatcher needs 'name'"));
		}
		if (FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, DispatcherName))
		{
			return FUplinkToolResult::Error(FString::Printf(
				TEXT("'%s' is already a dispatcher on this blueprint"), *DispatcherName.ToString()));
		}
		if (OpParams->HasField(FStringView(TEXT("outputs"))))
		{
			// Silently dropping these would be the worse failure: the caller
			// gets a dispatcher that looks right and returns nothing.
			return FUplinkToolResult::Error(TEXT("a dispatcher signature cannot have return values - it is a broadcast. Use 'inputs'."));
		}

		FEdGraphPinType DelegateType;
		DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, DispatcherName, DelegateType))
		{
			return FUplinkToolResult::Error(FString::Printf(
				TEXT("could not add '%s' (name already taken?)"), *DispatcherName.ToString()));
		}

		UEdGraph* SignatureGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, DispatcherName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!SignatureGraph)
		{
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, DispatcherName);
			return FUplinkToolResult::Error(TEXT("could not create the dispatcher's signature graph"));
		}
		SignatureGraph->bEditable = false;

		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		K2Schema->CreateDefaultNodesForGraph(*SignatureGraph);
		// Null class: entry node only. A dispatcher has no result node.
		K2Schema->CreateFunctionGraphTerminators(*SignatureGraph, static_cast<UClass*>(nullptr));
		K2Schema->AddExtraFunctionFlags(SignatureGraph,
			FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
		K2Schema->MarkFunctionEntryAsEditable(SignatureGraph, true);
		Blueprint->DelegateSignatureGraphs.Add(SignatureGraph);

		TArray<FString> AddedParams;
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (OpParams->TryGetArrayField(FStringView(TEXT("inputs")), Inputs))
		{
			UK2Node_FunctionEntry* Entry = nullptr;
			for (UEdGraphNode* GraphNode : SignatureGraph->Nodes)
			{
				if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(GraphNode))
				{
					Entry = AsEntry;
					break;
				}
			}
			if (Entry)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Inputs)
				{
					const TSharedPtr<FJsonObject>* Obj = nullptr;
					if (!Value->TryGetObject(Obj) || !Obj->IsValid())
					{
						continue;
					}
					const FString ParamName = GetString(*Obj, TEXT("name"));
					FEdGraphPinType ParamType;
					if (!MakePinType(GetString(*Obj, TEXT("type"), TEXT("float")), ParamType, Error))
					{
						return FUplinkToolResult::Error(FString::Printf(TEXT("input '%s': %s"), *ParamName, *Error));
					}
					Entry->CreateUserDefinedPin(FName(*ParamName), ParamType, EGPD_Output);
					AddedParams.Add(ParamName);
				}
			}
		}

		// Compile the skeleton NOW, not at the end of the batch. Bind and
		// call nodes read the generated signature when their pins are
		// allocated; with a stale skeleton they come out param-less and
		// every later connect in the same batch fails on a missing pin.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Data->SetStringField(TEXT("dispatcher"), DispatcherName.ToString());
		if (AddedParams.Num())
		{
			Data->SetStringField(TEXT("inputs"), FString::Join(AddedParams, TEXT(", ")));
		}
		return FUplinkToolResult::Ok();
	}
}
