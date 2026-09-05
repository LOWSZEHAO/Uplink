// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Ops that address nodes already in a graph: resolving a node handle, wiring
// pins together, breaking links, pin defaults, and the arrange op.

#include "Blueprint/UplinkBlueprintCommon.h"
#include "UplinkToolUtil.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

using namespace UplinkToolUtil;

namespace UplinkBlueprint
{
	static UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidString, FString& OutError)
	{
		FGuid Guid;
		if (!FGuid::Parse(GuidString, Guid))
		{
			OutError = FString::Printf(TEXT("'%s' is not a node guid (get guids from bp_query)"), *GuidString);
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == Guid)
			{
				return Node;
			}
		}
		OutError = FString::Printf(TEXT("node %s not found in graph %s"), *GuidString, *Graph->GetName());
		return nullptr;
	}

	static UEdGraphPin* FindPinLoose(UEdGraphNode* Node, const FString& PinName, FString& OutError)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		TArray<FString> Names;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin)
			{
				Names.Add(Pin->PinName.ToString());
			}
		}
		OutError = FString::Printf(TEXT("pin '%s' not found. Pins: %s (exec pins are 'execute' and 'then')"),
			*PinName, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	/** Node handle: a guid from bp_query, or '@ref' naming an earlier batch op. */
	static UEdGraphNode* ResolveNodeHandle(UEdGraph* Graph, const FString& Handle,
		const TMap<FString, FString>& NodeRefs, FString& OutError)
	{
		FString Resolved = Handle;
		if (Handle.StartsWith(TEXT("@")))
		{
			const FString* Guid = NodeRefs.Find(Handle.RightChop(1));
			if (!Guid)
			{
				TArray<FString> Known;
				NodeRefs.GetKeys(Known);
				OutError = FString::Printf(TEXT("unknown node ref '%s'. Refs defined so far: %s"),
					*Handle, Known.Num() ? *FString::Join(Known, TEXT(", ")) : TEXT("(none)"));
				return nullptr;
			}
			Resolved = *Guid;
		}
		return FindNodeByGuid(Graph, Resolved, OutError);
	}

	FUplinkToolResult ApplyArrangeOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data)
	{
		FString Error;

		UEdGraph* Graph = FindGraph(Blueprint, GetString(OpParams, TEXT("graph")), Error);
		if (!Graph)
		{
			return FUplinkToolResult::Error(Error);
		}
		Data->SetNumberField(TEXT("moved"), ArrangeGraph(Graph));
		return FUplinkToolResult::Ok();
	}

	FUplinkToolResult ApplyConnectOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		const TMap<FString, FString>& NodeRefs)
	{
		FString Error;

		UEdGraph* Graph = FindGraph(Blueprint, GetString(OpParams, TEXT("graph")), Error);
		if (!Graph)
		{
			return FUplinkToolResult::Error(Error);
		}
		UEdGraphNode* FromNode = ResolveNodeHandle(Graph, GetString(OpParams, TEXT("from_node")), NodeRefs, Error);
		UEdGraphNode* ToNode = FromNode ? ResolveNodeHandle(Graph, GetString(OpParams, TEXT("to_node")), NodeRefs, Error) : nullptr;
		if (!FromNode || !ToNode)
		{
			return FUplinkToolResult::Error(Error);
		}
		UEdGraphPin* FromPin = FindPinLoose(FromNode, GetString(OpParams, TEXT("from_pin")), Error);
		UEdGraphPin* ToPin = FromPin ? FindPinLoose(ToNode, GetString(OpParams, TEXT("to_pin")), Error) : nullptr;
		if (!FromPin || !ToPin)
		{
			return FUplinkToolResult::Error(Error);
		}
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema->TryCreateConnection(FromPin, ToPin))
		{
			const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
			return FUplinkToolResult::Error(FString::Printf(TEXT("connection rejected: %s"), *Response.Message.ToString()));
		}
		return FUplinkToolResult::Ok();
	}

	FUplinkToolResult ApplyPinOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		const TMap<FString, FString>& NodeRefs)
	{
		FString Error;
		const FString Op = GetString(OpParams, TEXT("op"));

		UEdGraph* Graph = FindGraph(Blueprint, GetString(OpParams, TEXT("graph")), Error);
		if (!Graph)
		{
			return FUplinkToolResult::Error(Error);
		}
		UEdGraphNode* Node = ResolveNodeHandle(Graph, GetString(OpParams, TEXT("node")), NodeRefs, Error);
		if (!Node)
		{
			return FUplinkToolResult::Error(Error);
		}

		if (Op == TEXT("delete_node"))
		{
			FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
		}
		else
		{
			UEdGraphPin* Pin = FindPinLoose(Node, GetString(OpParams, TEXT("pin")), Error);
			if (!Pin)
			{
				return FUplinkToolResult::Error(Error);
			}
			if (Op == TEXT("break_links"))
			{
				Graph->GetSchema()->BreakPinLinks(*Pin, /*bSendsNodeNotification=*/true);
			}
			else // set_pin_default
			{
				FString Value = GetString(OpParams, TEXT("value"));
				const FName Category = Pin->PinType.PinCategory;

				// An enum pin stores the enumerator's own name, which for a
				// User Defined Enum is the NewEnumerator<n> it was born with -
				// never the authored name the node draws and the only one a
				// person has. Translating here is what lets "Halted" through;
				// without it the schema declines it and the pin keeps whatever
				// it had. A name that is already the stored one, and every
				// non-enum pin, passes through untouched.
				if (const UEnum* PinEnum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					if (PinEnum->GetIndexByNameString(Value) == INDEX_NONE)
					{
						for (int32 Index = 0; Index < PinEnum->NumEnums() - 1; ++Index)
						{
							if (PinEnum->GetDisplayNameTextByIndex(Index).ToString().Equals(Value, ESearchCase::IgnoreCase))
							{
								Value = PinEnum->GetNameStringByIndex(Index);
								break;
							}
						}
					}
				}
				const bool bObjectLike = Category == UEdGraphSchema_K2::PC_Object
					|| Category == UEdGraphSchema_K2::PC_Class
					|| Category == UEdGraphSchema_K2::PC_Interface;
				if (bObjectLike)
				{
					UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Value);
					if (!Object)
					{
						return FUplinkToolResult::Error(FString::Printf(TEXT("object not found: %s"), *Value));
					}
					Graph->GetSchema()->TrySetDefaultObject(*Pin, Object);
				}
				else
				{
					Graph->GetSchema()->TrySetDefaultValue(*Pin, Value);
				}
				// The schema silently declines a value the pin's type will
				// not take, so confirm it landed rather than reporting a
				// success the graph does not agree with. The check has to
				// read the slot the schema WRITES for this pin type: text
				// goes to DefaultTextValue and object-likes to
				// DefaultObject, and comparing DefaultValue for those
				// reported failure for values that had in fact landed - in
				// a batch, killing every op after them.
				FString Landed;
				bool bTookIt;
				if (bObjectLike)
				{
					bTookIt = Pin->DefaultObject != nullptr;
					Landed = Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString();
				}
				else if (Category == UEdGraphSchema_K2::PC_Text)
				{
					Landed = Pin->DefaultTextValue.ToString();
					bTookIt = Landed == Value || (Value.IsEmpty() && Landed.IsEmpty());
				}
				else
				{
					Landed = Pin->DefaultValue;
					bTookIt = Landed == Value || (Value.IsEmpty() && Landed.IsEmpty());
				}
				if (!bTookIt)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("pin '%s' would not take '%s' (it is a %s pin; it still reads '%s')"),
						*Pin->PinName.ToString(), *Value,
						*Category.ToString(), *Landed));
				}
			}
		}
		return FUplinkToolResult::Ok();
	}
}
