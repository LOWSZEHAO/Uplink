// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// bp_query, and the node/pin JSON shape it and the add_node ops both report.

#include "Blueprint/UplinkBlueprintCommon.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"

using namespace UplinkToolUtil;

namespace UplinkBlueprint
{
	TSharedRef<FJsonObject> NodeToJson(const UEdGraphNode* Node)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Out->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Out->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Out->SetNumberField(TEXT("x"), Node->NodePosX);
		Out->SetNumberField(TEXT("y"), Node->NodePosY);

		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden)
			{
				continue;
			}
			TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
			PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			// Only when it differs, and never instead of the name: 'name' is
			// what connect addresses, and the two part company exactly where it
			// matters most. A Switch on a User Defined Enum names its case pins
			// after the stored entry - NewEnumerator0 - and shows the authored
			// one, so a caller who wrote "Closed" finds nothing called that and
			// no way to tell which pin is theirs.
			if (!Pin->PinFriendlyName.IsEmpty()
				&& !Pin->PinFriendlyName.ToString().Equals(Pin->PinName.ToString()))
			{
				PinJson->SetStringField(TEXT("shown_as"), Pin->PinFriendlyName.ToString());
			}
			PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
			PinJson->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			if (!Pin->DefaultValue.IsEmpty())
			{
				PinJson->SetStringField(TEXT("default"), Pin->DefaultValue);
			}
			if (Pin->LinkedTo.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Links;
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode())
					{
						Links.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s:%s"),
							*Linked->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
							*Linked->PinName.ToString())));
					}
				}
				PinJson->SetArrayField(TEXT("links"), Links);
			}
			Pins.Add(MakeShared<FJsonValueObject>(PinJson));
		}
		Out->SetArrayField(TEXT("pins"), Pins);
		return Out;
	}

	void RegisterQuery(FUplinkToolRegistry& Registry)
	{
		Registry.RegisterQuick(
			TEXT("bp_query"),
			TEXT("Inspect a Blueprint: parent class, variables, and graphs with their nodes, pins and connections (node guids are the handles bp_modify uses)."),
			TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"},"graph":{"type":"string","description":"Only this graph (default: all)"},"max_nodes":{"type":"number","default":100},"offset":{"type":"number","default":0,"description":"Skip this many nodes per graph - page a big graph with offset + next_offset"}},"required":["blueprint"]})json"),
			/*bReadOnly=*/true,
			[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
			{
				FString Error;
				UBlueprint* Blueprint = LoadBlueprint(Ctx, Error);
				if (!Blueprint)
				{
					return FUplinkToolResult::Error(Error);
				}

				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
				Data->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
				Data->SetStringField(TEXT("status"),
					Blueprint->Status == BS_Error ? TEXT("error") :
					Blueprint->Status == BS_UpToDate ? TEXT("up_to_date") : TEXT("dirty"));

				// bp_modify can add an interface but nothing could report one, so
				// the only evidence an implement_interface call had worked was a
				// stub graph appearing among the others - and a graph named after
				// a function looks the same whoever created it. Reported even when
				// empty, because "implements nothing" is an answer and a missing
				// field is not.
				TArray<TSharedPtr<FJsonValue>> Interfaces;
				for (const FBPInterfaceDescription& Implemented : Blueprint->ImplementedInterfaces)
				{
					if (Implemented.Interface)
					{
						Interfaces.Add(MakeShared<FJsonValueString>(Implemented.Interface->GetPathName()));
					}
				}
				Data->SetArrayField(TEXT("interfaces"), Interfaces);

				TArray<TSharedPtr<FJsonValue>> Variables;
				for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Variable.VarName.ToString());
					Row->SetStringField(TEXT("type"), UEdGraphSchema_K2::TypeToText(Variable.VarType).ToString());
					if (!Variable.DefaultValue.IsEmpty())
					{
						Row->SetStringField(TEXT("default"), Variable.DefaultValue);
					}
					Variables.Add(MakeShared<FJsonValueObject>(Row));
				}
				Data->SetArrayField(TEXT("variables"), Variables);

				if (Blueprint->SimpleConstructionScript)
				{
					TArray<TSharedPtr<FJsonValue>> Components;
					TFunction<void(USCS_Node*, const FString&)> AddNodeRow =
						[&Components, &AddNodeRow](USCS_Node* Node, const FString& ParentName)
					{
						if (!Node)
						{
							return;
						}
						TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
						Row->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
						Row->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT(""));
						if (!ParentName.IsEmpty())
						{
							Row->SetStringField(TEXT("parent"), ParentName);
						}
						Components.Add(MakeShared<FJsonValueObject>(Row));
						for (USCS_Node* Child : Node->GetChildNodes())
						{
							AddNodeRow(Child, Node->GetVariableName().ToString());
						}
					};
					for (USCS_Node* Root : Blueprint->SimpleConstructionScript->GetRootNodes())
					{
						AddNodeRow(Root, FString());
					}
					Data->SetArrayField(TEXT("components"), Components);
				}

				const FString GraphFilter = GetString(Ctx.Params, TEXT("graph"));
				const int32 MaxNodes = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max_nodes"), 100)), 1, 500);
				// Paging: a hand-authored event graph can run to thousands of nodes,
				// and a capped read with no way to ask for the rest meant the far end
				// of a big graph was simply unreadable. offset + total + next_offset
				// make "read the whole thing" a loop instead of a dead end.
				const int32 Offset = FMath::Max(0, static_cast<int32>(GetNumber(Ctx.Params, TEXT("offset"), 0)));
				TArray<UEdGraph*> AllGraphs;
				Blueprint->GetAllGraphs(AllGraphs);

				TArray<TSharedPtr<FJsonValue>> Graphs;
				for (UEdGraph* Graph : AllGraphs)
				{
					if (!Graph)
					{
						continue;
					}
					if (!GraphFilter.IsEmpty() && !Graph->GetName().Equals(GraphFilter, ESearchCase::IgnoreCase))
					{
						continue;
					}
					TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
					GraphJson->SetStringField(TEXT("name"), Graph->GetName());
					TArray<TSharedPtr<FJsonValue>> Nodes;
					int32 Seen = 0;
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (!Node)
						{
							continue;
						}
						if (Seen++ < Offset)
						{
							continue;
						}
						if (Nodes.Num() >= MaxNodes)
						{
							break;
						}
						Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
					}
					GraphJson->SetArrayField(TEXT("nodes"), Nodes);
					GraphJson->SetNumberField(TEXT("total_nodes"), Graph->Nodes.Num());
					const bool bMore = Offset + Nodes.Num() < Graph->Nodes.Num();
					GraphJson->SetBoolField(TEXT("truncated"), bMore || Offset > 0);
					if (bMore)
					{
						GraphJson->SetNumberField(TEXT("next_offset"), Offset + Nodes.Num());
					}
					Graphs.Add(MakeShared<FJsonValueObject>(GraphJson));
				}
				Data->SetArrayField(TEXT("graphs"), Graphs);
				return FUplinkToolResult::Ok(Data);
			});
	}
}
