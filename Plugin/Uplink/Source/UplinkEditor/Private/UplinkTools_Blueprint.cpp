// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Blueprint tools: bp_create, bp_query, bp_modify, bp_compile.

#include "UplinkTools.h"
#include "UplinkCompat.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "FileHelpers.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "JsonObjectConverter.h"
#include "Blueprint/UserWidget.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Select.h"
#include "K2Node_Self.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"

using namespace UplinkToolUtil;

namespace
{
	UBlueprint* LoadBlueprint(const FUplinkToolContext& Ctx, FString& OutError)
	{
		const FString Path = GetString(Ctx.Params, TEXT("blueprint"));
		if (Path.IsEmpty())
		{
			OutError = TEXT("'blueprint' (asset path, e.g. /Game/BP_Thing) is required");
			return nullptr;
		}
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path);
		if (!Blueprint)
		{
			OutError = FString::Printf(TEXT("blueprint not found: %s"), *Path);
		}
		return Blueprint;
	}

	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName, FString& OutError)
	{
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		if (GraphName.IsEmpty())
		{
			if (Blueprint->UbergraphPages.Num() > 0)
			{
				return Blueprint->UbergraphPages[0];
			}
			OutError = TEXT("blueprint has no event graph; pass 'graph'");
			return nullptr;
		}
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
		TArray<FString> Names;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph)
			{
				Names.Add(Graph->GetName());
			}
		}
		OutError = FString::Printf(TEXT("graph '%s' not found. Available: %s"), *GraphName, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidString, FString& OutError)
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

	UEdGraphPin* FindPinLoose(UEdGraphNode* Node, const FString& PinName, FString& OutError)
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

	bool MakePinType(const FString& TypeString, FEdGraphPinType& Out, FString& OutError);

	bool MakePinType(const FString& TypeString, FEdGraphPinType& Out, FString& OutError)
	{
		Out = FEdGraphPinType();

		FString Inner = TypeString;
		if (Inner.StartsWith(TEXT("array:")))
		{
			Out.ContainerType = EPinContainerType::Array;
			Inner = Inner.RightChop(6);
		}
		else if (Inner.StartsWith(TEXT("set:")))
		{
			Out.ContainerType = EPinContainerType::Set;
			Inner = Inner.RightChop(4);
		}
		else if (Inner.StartsWith(TEXT("map:")))
		{
			// map:<key>:<value> - the value type rides in the terminal category.
			Out.ContainerType = EPinContainerType::Map;
			const FString Spec = Inner.RightChop(4);
			FString KeyPart, ValuePart;
			if (!Spec.Split(TEXT(":"), &KeyPart, &ValuePart)
				|| KeyPart.IsEmpty() || ValuePart.IsEmpty())
			{
				OutError = TEXT("map needs both halves: map:<key>:<value>, e.g. map:name:float");
				return false;
			}
			// Object/struct specs contain their own colon (object:/Script/...),
			// so re-join everything after the first split point for the key when
			// the key itself is a prefixed form.
			if ((KeyPart == TEXT("object") || KeyPart == TEXT("class") || KeyPart == TEXT("struct")
				|| KeyPart == TEXT("enum") || KeyPart == TEXT("soft_object") || KeyPart == TEXT("soft_class")))
			{
				// key is prefixed: find the value after the key's own payload.
				// Simplest honest rule: prefixed keys are not supported - keys in
				// Blueprint maps are almost always name/string/int anyway.
				OutError = TEXT("map keys must be simple types (bool,int,int64,float,string,name,text,byte). Prefixed key types are not supported.");
				return false;
			}
			FEdGraphPinType ValueType;
			if (!MakePinType(ValuePart, ValueType, OutError))
			{
				return false;
			}
			if (ValueType.ContainerType != EPinContainerType::None)
			{
				OutError = TEXT("a map value cannot itself be a container");
				return false;
			}
			Out.PinValueType = FEdGraphTerminalType::FromPinType(ValueType);
			Inner = KeyPart;
		}

		if (Inner == TEXT("bool")) { Out.PinCategory = UEdGraphSchema_K2::PC_Boolean; }
		else if (Inner == TEXT("int")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int; }
		else if (Inner == TEXT("int64")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int64; }
		else if (Inner == TEXT("float") || Inner == TEXT("double"))
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Real;
			Out.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		else if (Inner == TEXT("string")) { Out.PinCategory = UEdGraphSchema_K2::PC_String; }
		else if (Inner == TEXT("name")) { Out.PinCategory = UEdGraphSchema_K2::PC_Name; }
		else if (Inner == TEXT("text")) { Out.PinCategory = UEdGraphSchema_K2::PC_Text; }
		else if (Inner == TEXT("byte")) { Out.PinCategory = UEdGraphSchema_K2::PC_Byte; }
		else if (Inner == TEXT("vector"))
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		}
		else if (Inner == TEXT("rotator"))
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		}
		else if (Inner == TEXT("transform"))
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		}
		else if (Inner.StartsWith(TEXT("object:")) || Inner.StartsWith(TEXT("class:")))
		{
			const bool bClassPin = Inner.StartsWith(TEXT("class:"));
			const FString ClassPath = Inner.RightChop(bClassPin ? 6 : 7);
			UClass* Class = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
			if (!Class)
			{
				OutError = FString::Printf(TEXT("class not found: %s"), *ClassPath);
				return false;
			}
			Out.PinCategory = bClassPin ? UEdGraphSchema_K2::PC_Class : UEdGraphSchema_K2::PC_Object;
			Out.PinSubCategoryObject = Class;
		}
		else if (Inner.StartsWith(TEXT("struct:")))
		{
			const FString StructPath = Inner.RightChop(7);
			UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
			if (!Struct)
			{
				OutError = FString::Printf(TEXT("struct not found: %s"), *StructPath);
				return false;
			}
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = Struct;
		}
		else if (Inner.StartsWith(TEXT("enum:")))
		{
			const FString EnumPath = Inner.RightChop(5);
			UEnum* Enum = LoadObject<UEnum>(nullptr, *EnumPath);
			if (!Enum)
			{
				OutError = FString::Printf(TEXT("enum not found: %s (e.g. /Script/Engine.ECollisionChannel or /Game/E_State.E_State)"), *EnumPath);
				return false;
			}
			// Byte-backed, the way the editor types an enum variable.
			Out.PinCategory = UEdGraphSchema_K2::PC_Byte;
			Out.PinSubCategoryObject = Enum;
		}
		else if (Inner.StartsWith(TEXT("soft_object:")) || Inner.StartsWith(TEXT("soft_class:")))
		{
			const bool bClassPin = Inner.StartsWith(TEXT("soft_class:"));
			const FString ClassPath = Inner.RightChop(bClassPin ? 11 : 12);
			UClass* Class = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
			if (!Class)
			{
				OutError = FString::Printf(TEXT("class not found: %s"), *ClassPath);
				return false;
			}
			Out.PinCategory = bClassPin ? UEdGraphSchema_K2::PC_SoftClass : UEdGraphSchema_K2::PC_SoftObject;
			Out.PinSubCategoryObject = Class;
		}
		else
		{
			OutError = FString::Printf(TEXT("unknown type '%s' (bool,int,int64,float,string,name,text,byte,vector,rotator,transform,object:<class>,class:<class>,soft_object:<class>,soft_class:<class>,struct:<path>,enum:<path>,array:<inner>,set:<inner>,map:<key>:<value>)"), *TypeString);
			return false;
		}
		return true;
	}

	FVector2D EstimateNodeSize(const UEdGraphNode* Node)
	{
		int32 In = 0;
		int32 Out = 0;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->bHidden)
			{
				(Pin->Direction == EGPD_Input ? In : Out)++;
			}
		}
		const float Width = (Node->NodeWidth > 0) ? Node->NodeWidth : 280.0f;
		const float Height = (Node->NodeHeight > 0) ? Node->NodeHeight : (52.0f + 24.0f * FMath::Max(In, Out));
		return FVector2D(Width, Height);
	}

	/**
	 * Authoring style rule: nodes never overlap. Explicit positions are nudged
	 * down out of occupied space; without a position the node goes on a fresh
	 * row below everything, aligned with the leftmost column.
	 */
	void PlaceNodeWithoutOverlap(UEdGraph* Graph, UEdGraphNode* Node, bool bHasExplicitPos)
	{
		if (!bHasExplicitPos)
		{
			float MinX = 0.0f;
			float MaxBottom = 0.0f;
			bool bAny = false;
			for (const UEdGraphNode* Other : Graph->Nodes)
			{
				if (!Other || Other == Node)
				{
					continue;
				}
				MinX = bAny ? FMath::Min(MinX, static_cast<float>(Other->NodePosX)) : Other->NodePosX;
				MaxBottom = FMath::Max(MaxBottom, Other->NodePosY + static_cast<float>(EstimateNodeSize(Other).Y));
				bAny = true;
			}
			if (bAny)
			{
				Node->NodePosX = static_cast<int32>(MinX);
				Node->NodePosY = static_cast<int32>(MaxBottom + 60.0f);
			}
		}

		const FVector2D MySize = EstimateNodeSize(Node);
		const float Margin = 20.0f;
		for (int32 Guard = 0; Guard < 200; ++Guard)
		{
			bool bMoved = false;
			for (const UEdGraphNode* Other : Graph->Nodes)
			{
				if (!Other || Other == Node)
				{
					continue;
				}
				const FVector2D OtherSize = EstimateNodeSize(Other);
				const bool bOverlapX = Node->NodePosX < Other->NodePosX + OtherSize.X + Margin
					&& Other->NodePosX < Node->NodePosX + MySize.X + Margin;
				const bool bOverlapY = Node->NodePosY < Other->NodePosY + OtherSize.Y + Margin
					&& Other->NodePosY < Node->NodePosY + MySize.Y + Margin;
				if (bOverlapX && bOverlapY)
				{
					Node->NodePosY = static_cast<int32>(Other->NodePosY + OtherSize.Y + Margin + 8.0f);
					bMoved = true;
				}
			}
			if (!bMoved)
			{
				break;
			}
		}
	}

	void FinalizeNewNode(UEdGraph* Graph, UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Op)
	{
		Graph->Modify();
		Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		Node->NodePosX = static_cast<int32>(GetNumber(Op, TEXT("x"), 0));
		Node->NodePosY = static_cast<int32>(GetNumber(Op, TEXT("y"), 0));
		const bool bHasExplicitPos = Op->HasField(FStringView(TEXT("x"))) || Op->HasField(FStringView(TEXT("y")));
		PlaceNodeWithoutOverlap(Graph, Node, bHasExplicitPos);
	}

	/** Node handle: a guid from bp_query, or '@ref' naming an earlier batch op. */
	UEdGraphNode* ResolveNodeHandle(UEdGraph* Graph, const FString& Handle,
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

	/** Estimated Y of a pin's row, for headless layout decisions. */
	float EstimatePinY(const UEdGraphNode* Node, const UEdGraphPin* Pin)
	{
		int32 Row = 0;
		for (const UEdGraphPin* Other : Node->Pins)
		{
			if (Other == Pin)
			{
				break;
			}
			if (Other && !Other->bHidden && Other->Direction == Pin->Direction)
			{
				++Row;
			}
		}
		return Node->NodePosY + 38.0f + 24.0f * Row;
	}

	/** Remove every reroute knot, reconnecting what flowed through it. */
	void RemoveKnots(UEdGraph* Graph)
	{
		TArray<UK2Node_Knot*> Knots;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node))
			{
				Knots.Add(Knot);
			}
		}
		for (UK2Node_Knot* Knot : Knots)
		{
			UEdGraphPin* In = Knot->GetInputPin();
			UEdGraphPin* Out = Knot->GetOutputPin();
			if (In && Out)
			{
				TArray<UEdGraphPin*> Sources = In->LinkedTo;
				TArray<UEdGraphPin*> Targets = Out->LinkedTo;
				for (UEdGraphPin* Source : Sources)
				{
					for (UEdGraphPin* Target : Targets)
					{
						Source->MakeLinkTo(Target);
					}
				}
			}
			Knot->BreakAllNodeLinks();
			Graph->RemoveNode(Knot);
		}
	}

	/**
	 * The stock way to tidy a turning wire: a reroute knot. The wire leaves the
	 * source pin dead level, hits the knot just before the target column, and
	 * the engine's own spline makes the drop into the pin.
	 */
	int32 InsertKnotsAtTurns(UEdGraph* Graph)
	{
		struct FTurn
		{
			UEdGraphPin* Source = nullptr;
			UEdGraphPin* Target = nullptr;
			float KnotX = 0.0f;
			float KnotY = 0.0f;
		};
		TArray<FTurn> Turns;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || Node->IsA<UK2Node_Knot>())
			{
				continue;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					UEdGraphNode* TargetNode = Linked ? Linked->GetOwningNode() : nullptr;
					if (!TargetNode || TargetNode->IsA<UK2Node_Knot>())
					{
						continue;
					}
					const float SourceY = EstimatePinY(Node, Pin);
					const float TargetY = EstimatePinY(TargetNode, Linked);
					const float SourceRight = Node->NodePosX + static_cast<float>(EstimateNodeSize(Node).X);
					const float Gap = TargetNode->NodePosX - SourceRight;
					if (FMath::Abs(SourceY - TargetY) > 40.0f && Gap > 120.0f)
					{
						FTurn Turn;
						Turn.Source = Pin;
						Turn.Target = Linked;
						Turn.KnotX = TargetNode->NodePosX - 56.0f;
						Turn.KnotY = SourceY - 8.0f;
						Turns.Add(Turn);
					}
				}
			}
		}

		for (const FTurn& Turn : Turns)
		{
			UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(Graph);
			Graph->AddNode(Knot, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			Knot->CreateNewGuid();
			Knot->PostPlacedNewNode();
			Knot->AllocateDefaultPins();
			Knot->NodePosX = static_cast<int32>(Turn.KnotX);
			Knot->NodePosY = static_cast<int32>(Turn.KnotY);

			Turn.Source->BreakLinkTo(Turn.Target);
			if (!Graph->GetSchema()->TryCreateConnection(Turn.Source, Knot->GetInputPin())
				|| !Graph->GetSchema()->TryCreateConnection(Knot->GetOutputPin(), Turn.Target))
			{
				// Knot rejected (wildcard propagation failed) - restore the direct wire.
				Knot->BreakAllNodeLinks();
				Graph->RemoveNode(Knot);
				Turn.Source->MakeLinkTo(Turn.Target);
			}
		}
		return Turns.Num();
	}

	/**
	 * Authoring style rule: tidy graphs. Left-to-right dependency columns; each
	 * exec chain becomes one horizontal lane (level pins = straight wires);
	 * pure data nodes sit below the lane they feed, one column left of their
	 * first consumer; wires that change height get a reroute knot at the turn.
	 */
	int32 ArrangeGraph(UEdGraph* Graph)
	{
		RemoveKnots(Graph);

		TArray<UEdGraphNode*> Nodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Nodes.Add(Node);
			}
		}
		if (Nodes.Num() == 0)
		{
			return 0;
		}

		auto IsExecPin = [](const UEdGraphPin* Pin)
		{
			return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
		};
		auto HasExecPins = [&IsExecPin](const UEdGraphNode* Node)
		{
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (IsExecPin(Pin))
				{
					return true;
				}
			}
			return false;
		};

		TMap<UEdGraphNode*, TArray<UEdGraphNode*>> Preds;
		TMap<UEdGraphNode*, TArray<UEdGraphNode*>> Succs;
		for (UEdGraphNode* Node : Nodes)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input)
				{
					continue;
				}
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode())
					{
						Preds.FindOrAdd(Node).AddUnique(Linked->GetOwningNode());
						Succs.FindOrAdd(Linked->GetOwningNode()).AddUnique(Node);
					}
				}
			}
		}

		// Longest-path ranks give the left-to-right column order.
		TMap<UEdGraphNode*, int32> Rank;
		for (UEdGraphNode* Node : Nodes)
		{
			Rank.Add(Node, 0);
		}
		for (int32 Pass = 0; Pass < Nodes.Num() + 2; ++Pass)
		{
			bool bChanged = false;
			for (UEdGraphNode* Node : Nodes)
			{
				if (const TArray<UEdGraphNode*>* NodePreds = Preds.Find(Node))
				{
					for (UEdGraphNode* Pred : *NodePreds)
					{
						if (Rank[Node] < Rank[Pred] + 1)
						{
							Rank[Node] = Rank[Pred] + 1;
							bChanged = true;
						}
					}
				}
			}
			if (!bChanged)
			{
				break;
			}
		}
		// Pure data nodes compress rightward, next to their first consumer.
		for (UEdGraphNode* Node : Nodes)
		{
			if (HasExecPins(Node))
			{
				continue;
			}
			if (const TArray<UEdGraphNode*>* Consumers = Succs.Find(Node))
			{
				int32 MinConsumer = TNumericLimits<int32>::Max();
				for (UEdGraphNode* Consumer : *Consumers)
				{
					MinConsumer = FMath::Min(MinConsumer, Rank[Consumer]);
				}
				if (MinConsumer != TNumericLimits<int32>::Max() && MinConsumer > 0)
				{
					Rank[Node] = MinConsumer - 1;
				}
			}
		}

		int32 MaxRank = 0;
		for (const auto& Pair : Rank)
		{
			MaxRank = FMath::Max(MaxRank, Pair.Value);
		}
		TArray<float> ColWidth;
		ColWidth.SetNumZeroed(MaxRank + 1);
		for (UEdGraphNode* Node : Nodes)
		{
			ColWidth[Rank[Node]] = FMath::Max(ColWidth[Rank[Node]], static_cast<float>(EstimateNodeSize(Node).X));
		}
		TArray<float> ColX;
		ColX.SetNumZeroed(MaxRank + 1);
		for (int32 Col = 1; Col <= MaxRank; ++Col)
		{
			ColX[Col] = ColX[Col - 1] + ColWidth[Col - 1] + 130.0f;
		}

		// Exec chains become horizontal lanes: roots first, in visual order.
		TArray<UEdGraphNode*> Roots;
		for (UEdGraphNode* Node : Nodes)
		{
			if (!HasExecPins(Node))
			{
				continue;
			}
			bool bHasExecIn = false;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (IsExecPin(Pin) && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0)
				{
					bHasExecIn = true;
					break;
				}
			}
			if (!bHasExecIn)
			{
				Roots.Add(Node);
			}
		}
		Roots.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodePosY < B.NodePosY; });

		TSet<UEdGraphNode*> Placed;
		auto WalkChain = [&IsExecPin, &Placed](UEdGraphNode* Start)
		{
			TArray<UEdGraphNode*> Chain;
			UEdGraphNode* Current = Start;
			while (Current && !Placed.Contains(Current))
			{
				Chain.Add(Current);
				Placed.Add(Current);
				UEdGraphNode* Next = nullptr;
				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (IsExecPin(Pin) && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
					{
						Next = Pin->LinkedTo[0]->GetOwningNode();
						break;
					}
				}
				Current = Next;
			}
			return Chain;
		};

		TArray<TArray<UEdGraphNode*>> Lanes;
		for (UEdGraphNode* Root : Roots)
		{
			if (!Placed.Contains(Root))
			{
				Lanes.Add(WalkChain(Root));
			}
		}
		for (UEdGraphNode* Node : Nodes)
		{
			if (HasExecPins(Node) && !Placed.Contains(Node))
			{
				Lanes.Add(WalkChain(Node));
			}
		}

		Graph->Modify();
		int32 Moved = 0;
		float LaneY = 0.0f;
		for (const TArray<UEdGraphNode*>& Chain : Lanes)
		{
			float LaneMaxHeight = 0.0f;
			for (UEdGraphNode* Node : Chain)
			{
				Node->Modify();
				Node->NodePosX = static_cast<int32>(ColX[Rank[Node]]);
				Node->NodePosY = static_cast<int32>(LaneY);
				LaneMaxHeight = FMath::Max(LaneMaxHeight, static_cast<float>(EstimateNodeSize(Node).Y));
				++Moved;
			}

			// Pure feeders of this chain (transitively) stack below the lane.
			const float DataY = LaneY + LaneMaxHeight + 60.0f;
			float MaxBottom = DataY;
			TMap<int32, float> ColNextY;
			TArray<UEdGraphNode*> Frontier = Chain;
			for (int32 Index = 0; Index < Frontier.Num(); ++Index)
			{
				if (const TArray<UEdGraphNode*>* NodePreds = Preds.Find(Frontier[Index]))
				{
					for (UEdGraphNode* Pred : *NodePreds)
					{
						if (HasExecPins(Pred) || Placed.Contains(Pred))
						{
							continue;
						}
						Placed.Add(Pred);
						Frontier.Add(Pred);
						Pred->Modify();
						Pred->NodePosX = static_cast<int32>(ColX[Rank[Pred]]);
						float& NextY = ColNextY.FindOrAdd(Rank[Pred]);
						NextY = FMath::Max(NextY, DataY);
						Pred->NodePosY = static_cast<int32>(NextY);
						NextY += EstimateNodeSize(Pred).Y + 40.0f;
						MaxBottom = FMath::Max(MaxBottom, NextY);
						++Moved;
					}
				}
			}
			LaneY = MaxBottom + 100.0f;
		}

		// Anything left (unconnected pure nodes) goes on its own rows at the end.
		for (UEdGraphNode* Node : Nodes)
		{
			if (!Placed.Contains(Node))
			{
				Node->Modify();
				Node->NodePosX = static_cast<int32>(ColX[Rank[Node]]);
				Node->NodePosY = static_cast<int32>(LaneY);
				LaneY += EstimateNodeSize(Node).Y + 40.0f;
				++Moved;
			}
		}

		InsertKnotsAtTurns(Graph);
		return Moved;
	}

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

	FUplinkToolResult CompileAndReport(UBlueprint* Blueprint)
	{
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

		TArray<TSharedPtr<FJsonValue>> Messages;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("severity"),
				Message->GetSeverity() == EMessageSeverity::Error ? TEXT("error") :
				Message->GetSeverity() == EMessageSeverity::Warning ? TEXT("warning") : TEXT("info"));
			Row->SetStringField(TEXT("text"), Message->ToText().ToString());
			Messages.Add(MakeShared<FJsonValueObject>(Row));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Data->SetNumberField(TEXT("errors"), Results.NumErrors);
		Data->SetNumberField(TEXT("warnings"), Results.NumWarnings);
		Data->SetArrayField(TEXT("messages"), Messages);

		FUplinkToolResult Out = FUplinkToolResult::Ok(Data,
			Results.NumErrors == 0 ? TEXT("compiled") : TEXT("compile FAILED"));
		Out.bError = Results.NumErrors > 0;
		return Out;
	}
	/**
	 * One graph edit. Op fields match bp_modify's single-op form; NodeRefs maps
	 * batch 'ref' names to node guids so later ops can say '@ref'.
	 */
	FUplinkToolResult ApplyGraphOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TMap<FString, FString>& NodeRefs, TSharedRef<FJsonObject> Data)
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
			else if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) == INDEX_NONE)
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
		else if (Op == TEXT("implement_interface") || Op == TEXT("remove_interface"))
		{
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
				// Only functions with outputs get a stub graph. Event-style ones
				// get nothing at all, and the add_node 'event' kind cannot reach
				// them because an interface is not in the super chain - so say
				// which ones still need override_function.
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
		}
		else if (Op == TEXT("override_function"))
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
					// GetOverrideFunctionClass resolves to the SKELETON class once this
					// event is implemented, and the engine's lookup demands the node's
					// parent class be a child of that - so a second call would never
					// find the node the first call made, and would duplicate it. The
					// editor's Override menu never hits this, because it hides functions
					// that are already overridden. Match the way the compiler does when
					// it reports a duplicate event: by name.
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
		}
		else if (Op == TEXT("add_dispatcher"))
		{
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
		else if (Op == TEXT("add_function"))
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

			// Find the entry node so we can flag thread-safety and add parameters.
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
		else if (Op == TEXT("set_node_property"))
		{
			// Set a property ON A GRAPH NODE (addressed by guid), not on an asset.
			// Supports dotted paths so struct members are reachable - anim graph
			// nodes keep their real settings inside a 'Node' struct, e.g.
			// "Node.OnMotionMatchingStateUpdated.FunctionName".
			UEdGraph* Graph = FindGraph(Blueprint, GetString(OpParams, TEXT("graph")), Error);
			if (!Graph)
			{
				return FUplinkToolResult::Error(Error);
			}
			const FString NodeRef = GetString(OpParams, TEXT("node"));
			// Compare parsed guids: bp_query emits DigitsWithHyphens while
			// FGuid::ToString() defaults to Digits, so a string compare misses.
			FGuid WantedGuid;
			const bool bParsedGuid = FGuid::Parse(NodeRef, WantedGuid);
			UEdGraphNode* Target = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (bParsedGuid ? (Node->NodeGuid == WantedGuid)
					: (Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) == NodeRef))
				{
					Target = Node;
					break;
				}
			}
			if (!Target)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("node '%s' not found in graph '%s' (use bp_query to list node guids)"), *NodeRef, *Graph->GetName()));
			}

			const FString PropPath = GetString(OpParams, TEXT("property"));
			TArray<FString> Segments;
			PropPath.ParseIntoArray(Segments, TEXT("."), true);
			if (Segments.Num() == 0)
			{
				return FUplinkToolResult::Error(TEXT("'property' is required"));
			}

			// Walk the path: every segment but the last must be a struct.
			void* Container = Target;
			UStruct* Owner = Target->GetClass();
			FProperty* Leaf = nullptr;
			for (int32 i = 0; i < Segments.Num(); ++i)
			{
				FProperty* Found = Owner->FindPropertyByName(FName(*Segments[i]));
				if (!Found)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("property '%s' not found on %s (path '%s')"),
						*Segments[i], *Owner->GetName(), *PropPath));
				}
				if (i == Segments.Num() - 1)
				{
					Leaf = Found;
					Container = Found->ContainerPtrToValuePtr<void>(Container);
					break;
				}
				FStructProperty* AsStruct = CastField<FStructProperty>(Found);
				if (!AsStruct)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' is not a struct, so '%s' cannot be reached"), *Segments[i], *PropPath));
				}
				Container = AsStruct->ContainerPtrToValuePtr<void>(Container);
				Owner = AsStruct->Struct;
			}

			const TSharedPtr<FJsonValue> Value = OpParams->TryGetField(FStringView(TEXT("value")));
			if (!Value.IsValid())
			{
				return FUplinkToolResult::Error(TEXT("'value' is required"));
			}
			Target->Modify();
			if (!FJsonObjectConverter::JsonValueToUProperty(Value, Leaf, Container))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("could not convert value for %s (%s)"), *PropPath, *Leaf->GetCPPType()));
			}

			// Let the node rebuild its pins/state from the new value.
			bool bReconstruct = true;
			OpParams->TryGetBoolField(FStringView(TEXT("reconstruct")), bReconstruct);
			if (bReconstruct)
			{
				Target->ReconstructNode();
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

			Data->SetStringField(TEXT("node"), Target->GetClass()->GetName());
			Data->SetStringField(TEXT("property"), PropPath);
			Data->SetField(TEXT("value"), FJsonObjectConverter::UPropertyToJsonValue(Leaf, Container));
		}
		else if (Op == TEXT("add_node"))
		{
			UEdGraph* Graph = FindGraph(Blueprint, GetString(OpParams, TEXT("graph")), Error);
			if (!Graph)
			{
				return FUplinkToolResult::Error(Error);
			}
			const FString Kind = GetString(OpParams, TEXT("kind"));
			UEdGraphNode* NewNode = nullptr;
			UEdGraphNode* ResultNode = nullptr;

			if (Kind == TEXT("call_function"))
			{
				const FString ClassPath = GetString(OpParams, TEXT("class"), TEXT("self"));
				UClass* TargetClass = nullptr;
				if (ClassPath == TEXT("self"))
				{
					TargetClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->ParentClass.Get();
				}
				else
				{
					TargetClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
				}
				if (!TargetClass)
				{
					return FUplinkToolResult::Error(TEXT("class not found for call_function"));
				}
				UFunction* Function = TargetClass->FindFunctionByName(FName(*GetString(OpParams, TEXT("function"))));
				if (!Function)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("function '%s' not found on %s"), *GetString(OpParams, TEXT("function")), *TargetClass->GetName()));
				}
				// The node class matters, not just the function. Array_Add and
				// friends declare a typed TArray parameter, and only
				// UK2Node_CallArrayFunction turns that into a wildcard that
				// propagates the connected type - on a plain call node the pin is
				// hard int32, the schema rejects everything else, and nothing
				// says why. Same story for the DataTable row pin. This mirrors
				// how the palette picks (BlueprintFunctionNodeSpawner).
				TSubclassOf<UK2Node_CallFunction> NodeClass = UK2Node_CallFunction::StaticClass();
				if (Function->HasMetaData(FBlueprintMetadata::MD_DataTablePin))
				{
					NodeClass = UK2Node_CallDataTableFunction::StaticClass();
				}
				else if (Function->HasMetaData(FBlueprintMetadata::MD_ArrayParam))
				{
					NodeClass = UK2Node_CallArrayFunction::StaticClass();
				}
				UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph, NodeClass);
				Node->SetFromFunction(Function);
				NewNode = Node;
			}
			else if (Kind == TEXT("custom_event"))
			{
				UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
				Node->CustomFunctionName = FName(*GetString(OpParams, TEXT("name"), TEXT("CustomEvent")));
				NewNode = Node;
			}
			else if (Kind == TEXT("event"))
			{
				// New actor blueprints carry disabled ghost placeholders for the
				// common events; reuse (and enable) a matching one instead of
				// stacking a duplicate on top of it.
				const FName EventName(*GetString(OpParams, TEXT("name")));
				// Without this, a missing or misspelt 'name' built a nameless event
				// node that compiles to nothing and reads as broken in the editor.
				if (EventName.IsNone())
				{
					return FUplinkToolResult::Error(
						TEXT("add_node kind 'event' needs 'name' (the function being overridden, ")
						TEXT("e.g. ReceiveBeginPlay). For a new event of your own, use kind 'custom_event'."));
				}
				{
					UClass* SearchClass = Blueprint->SkeletonGeneratedClass
						? Blueprint->SkeletonGeneratedClass.Get()
						: Blueprint->ParentClass.Get();
					if (SearchClass && !SearchClass->FindFunctionByName(EventName))
					{
						TArray<FString> Candidates;
						for (TFieldIterator<UFunction> It(SearchClass, EFieldIteratorFlags::IncludeSuper);
							It && Candidates.Num() < 40; ++It)
						{
							if (UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(*It))
							{
								Candidates.AddUnique(It->GetName());
							}
						}
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("no event '%s' on %s. Available: %s"),
							*EventName.ToString(), *SearchClass->GetName(), *FString::Join(Candidates, TEXT(", "))));
					}
				}
				UK2Node_Event* Existing = nullptr;
				for (UEdGraphNode* GraphNode : Graph->Nodes)
				{
					UK2Node_Event* Event = Cast<UK2Node_Event>(GraphNode);
					if (Event && Event->bOverrideFunction && Event->EventReference.GetMemberName() == EventName)
					{
						Existing = Event;
						break;
					}
				}
				if (Existing)
				{
					Existing->Modify();
					Existing->SetEnabledState(ENodeEnabledState::Enabled);
					Data->SetObjectField(TEXT("node"), NodeToJson(Existing));
					Data->SetBoolField(TEXT("reused"), true);
					ResultNode = Existing;
				}
				else
				{
					UK2Node_Event* Node = NewObject<UK2Node_Event>(Graph);
					Node->EventReference.SetExternalMember(EventName, Blueprint->ParentClass);
					Node->bOverrideFunction = true;
					NewNode = Node;
				}
			}
			else if (Kind == TEXT("component_bound_event"))
			{
				// Bind a component's (or widget's) delegate as an event node -
				// the graph equivalent of clicking + on OnClicked/OnSystemFinished.
				UClass* OwnerClass = Blueprint->SkeletonGeneratedClass
					? Blueprint->SkeletonGeneratedClass.Get()
					: (Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->ParentClass.Get());
				FObjectProperty* ComponentProperty =
					FindFProperty<FObjectProperty>(OwnerClass, *GetString(OpParams, TEXT("component")));
				if (!ComponentProperty)
				{
					TArray<FString> Names;
					for (TFieldIterator<FObjectProperty> It(OwnerClass); It && Names.Num() < 40; ++It)
					{
						Names.Add(It->GetName());
					}
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("component property '%s' not found (widgets must have Is Variable set). Object properties: %s"),
						*GetString(OpParams, TEXT("component")), *FString::Join(Names, TEXT(", "))));
				}
				FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(
					ComponentProperty->PropertyClass, *GetString(OpParams, TEXT("event")));
				if (!DelegateProperty)
				{
					TArray<FString> Names;
					for (TFieldIterator<FMulticastDelegateProperty> It(ComponentProperty->PropertyClass); It; ++It)
					{
						Names.Add(It->GetName());
					}
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("event '%s' not found on %s. Delegates: %s"),
						*GetString(OpParams, TEXT("event")), *ComponentProperty->PropertyClass->GetName(),
						*FString::Join(Names, TEXT(", "))));
				}
				// One binding per component+event, like the editor's + button. A
				// second node compiles clean and fires the handler twice at
				// runtime, which is a miserable thing to debug from inside a game.
				if (const UK2Node_ComponentBoundEvent* Existing = FKismetEditorUtilities::FindBoundEventForComponent(
					Blueprint, DelegateProperty->GetFName(), ComponentProperty->GetFName()))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' on '%s' is already bound (node %s). Wire onto the existing node instead of stacking a second binding."),
						*DelegateProperty->GetName(), *ComponentProperty->GetName(),
						*Existing->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
				}
				UK2Node_ComponentBoundEvent* Node = NewObject<UK2Node_ComponentBoundEvent>(Graph);
				Node->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
				NewNode = Node;
			}
			else if (Kind == TEXT("variable_get") || Kind == TEXT("variable_set"))
			{
				const FName VarName(*GetString(OpParams, TEXT("name")));
				if (Kind == TEXT("variable_get"))
				{
					UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Graph);
					Node->VariableReference.SetSelfMember(VarName);
					NewNode = Node;
				}
				else
				{
					UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Graph);
					Node->VariableReference.SetSelfMember(VarName);
					NewNode = Node;
				}
			}
			else if (Kind == TEXT("call_parent"))
			{
				const FName FuncName(*GetString(OpParams, TEXT("function")));
				UClass* SelfClass = Blueprint->SkeletonGeneratedClass
					? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->ParentClass.Get();
				UFunction* FnInSelf = SelfClass ? SelfClass->FindFunctionByName(FuncName) : nullptr;
				if (!FnInSelf)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("function '%s' not found on %s or its parents"),
						*FuncName.ToString(), SelfClass ? *SelfClass->GetName() : TEXT("null")));
				}
				// If this blueprint owns the override, step up to the super's
				// version; if the function only exists on a parent, that IS the
				// parent implementation already.
				UFunction* ParentFn = (FnInSelf->GetOwnerClass() == SelfClass)
					? UEdGraphSchema_K2::GetCallableParentFunction(FnInSelf)
					: FnInSelf;
				if (!ParentFn)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' has no parent implementation to call"), *FuncName.ToString()));
				}
				UK2Node_CallParentFunction* Node = NewObject<UK2Node_CallParentFunction>(Graph);
				Node->SetFromFunction(ParentFn);
				NewNode = Node;
			}
			// --- event dispatchers --------------------------------------------
			else if (Kind == TEXT("bind_dispatcher") || Kind == TEXT("unbind_dispatcher")
				|| Kind == TEXT("call_dispatcher") || Kind == TEXT("clear_dispatcher"))
			{
				const FString DispatcherName = GetString(OpParams, TEXT("name"));
				// A dispatcher on another object needs its class; on this
				// blueprint, self-context stores no class at all, which is what
				// lets the node survive a rename or a duplicate of the asset.
				const FString TargetSpec = GetString(OpParams, TEXT("target"));
				UClass* OwnerClass = nullptr;
				bool bSelfContext = true;
				if (TargetSpec.IsEmpty())
				{
					OwnerClass = Blueprint->SkeletonGeneratedClass
						? Blueprint->SkeletonGeneratedClass.Get()
						: (Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->ParentClass.Get());
				}
				else
				{
					bSelfContext = false;
					OwnerClass = StaticLoadClass(UObject::StaticClass(), nullptr, *TargetSpec);
					if (!OwnerClass)
					{
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("target class not found: '%s'. A Blueprint's class needs the _C suffix, e.g. /Game/BP_Door.BP_Door_C"),
							*TargetSpec));
					}
				}

				FMulticastDelegateProperty* DelegateProperty =
					FindFProperty<FMulticastDelegateProperty>(OwnerClass, *DispatcherName);
				if (!DelegateProperty)
				{
					TArray<FString> Names;
					for (TFieldIterator<FMulticastDelegateProperty> It(OwnerClass); It; ++It)
					{
						Names.Add(It->GetName());
					}
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no dispatcher '%s' on %s. Dispatchers: %s"),
						*DispatcherName, *OwnerClass->GetName(),
						Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
				}

				UK2Node_BaseMCDelegate* Node = nullptr;
				if (Kind == TEXT("bind_dispatcher")) { Node = NewObject<UK2Node_AddDelegate>(Graph); }
				else if (Kind == TEXT("unbind_dispatcher")) { Node = NewObject<UK2Node_RemoveDelegate>(Graph); }
				else if (Kind == TEXT("call_dispatcher")) { Node = NewObject<UK2Node_CallDelegate>(Graph); }
				else { Node = NewObject<UK2Node_ClearDelegate>(Graph); }
				Node->SetFromProperty(DelegateProperty, bSelfContext, DelegateProperty->GetOwnerClass());
				NewNode = Node;
			}
			// --- flow control -------------------------------------------------
			else if (Kind == TEXT("branch"))
			{
				NewNode = NewObject<UK2Node_IfThenElse>(Graph);
			}
			else if (Kind == TEXT("sequence"))
			{
				NewNode = NewObject<UK2Node_ExecutionSequence>(Graph);
			}
			else if (Kind == TEXT("cast"))
			{
				UClass* TargetType = StaticLoadClass(UObject::StaticClass(), nullptr, *GetString(OpParams, TEXT("class")));
				if (!TargetType)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("cast needs 'class' to name the type to cast TO, e.g. /Script/Engine.PlayerController or /Game/BP_Door.BP_Door_C (got '%s')"),
						*GetString(OpParams, TEXT("class"))));
				}
				UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(Graph);
				Node->TargetType = TargetType;
				bool bPure = false;
				OpParams->TryGetBoolField(FStringView(TEXT("pure")), bPure);
				Node->SetPurity(bPure);
				NewNode = Node;
			}
			else if (Kind == TEXT("switch_enum"))
			{
				UEnum* Enum = LoadObject<UEnum>(nullptr, *GetString(OpParams, TEXT("enum")));
				if (!Enum)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("switch_enum needs 'enum', e.g. /Script/Engine.ECollisionChannel or /Game/E_State.E_State (got '%s')"),
						*GetString(OpParams, TEXT("enum"))));
				}
				UK2Node_SwitchEnum* Node = NewObject<UK2Node_SwitchEnum>(Graph);
				Node->SetEnum(Enum);
				NewNode = Node;
			}
			else if (Kind == TEXT("switch_int"))
			{
				NewNode = NewObject<UK2Node_SwitchInteger>(Graph);
			}
			else if (Kind == TEXT("switch_string"))
			{
				NewNode = NewObject<UK2Node_SwitchString>(Graph);
			}
			else if (Kind == TEXT("macro"))
			{
				// Loops and the rest of the standard library are macro instances,
				// not node classes - ForEachLoop, ForLoop, WhileLoop, DoOnce, Gate,
				// FlipFlop and friends all live in one engine Blueprint.
				const FString Library = GetString(OpParams, TEXT("library"),
					TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
				UBlueprint* MacroLibrary = LoadObject<UBlueprint>(nullptr, *Library);
				if (!MacroLibrary)
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("macro library not found: %s"), *Library));
				}
				const FString MacroName = GetString(OpParams, TEXT("name"));
				UEdGraph* MacroGraph = nullptr;
				TArray<FString> Available;
				for (UEdGraph* Candidate : MacroLibrary->MacroGraphs)
				{
					if (!Candidate)
					{
						continue;
					}
					Available.Add(Candidate->GetName());
					if (Candidate->GetName().Equals(MacroName, ESearchCase::IgnoreCase))
					{
						MacroGraph = Candidate;
					}
				}
				if (!MacroGraph)
				{
					Available.Sort();
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("macro '%s' not found in %s. Available: %s"),
						*MacroName, *Library, *FString::Join(Available, TEXT(", "))));
				}
				UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
				Node->SetMacroGraph(MacroGraph);
				NewNode = Node;
			}
			// --- data ---------------------------------------------------------
			else if (Kind == TEXT("make_struct") || Kind == TEXT("break_struct"))
			{
				UScriptStruct* StructType = LoadObject<UScriptStruct>(nullptr, *GetString(OpParams, TEXT("struct")));
				if (!StructType)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("%s needs 'struct', e.g. /Script/CoreUObject.Vector or /Game/S_Row.S_Row (got '%s')"),
						*Kind, *GetString(OpParams, TEXT("struct"))));
				}
				if (Kind == TEXT("make_struct"))
				{
					UK2Node_MakeStruct* Node = NewObject<UK2Node_MakeStruct>(Graph);
					Node->StructType = StructType;
					NewNode = Node;
				}
				else
				{
					UK2Node_BreakStruct* Node = NewObject<UK2Node_BreakStruct>(Graph);
					Node->StructType = StructType;
					NewNode = Node;
				}
			}
			else if (Kind == TEXT("make_array"))
			{
				NewNode = NewObject<UK2Node_MakeArray>(Graph);
			}
			else if (Kind == TEXT("select"))
			{
				NewNode = NewObject<UK2Node_Select>(Graph);
			}
			else if (Kind == TEXT("self"))
			{
				NewNode = NewObject<UK2Node_Self>(Graph);
			}
			else if (Kind == TEXT("function_result"))
			{
				// The return node of a function graph. Only meaningful there, and
				// a graph may only have one, so hand back the existing one rather
				// than making a second that silently wins or loses.
				for (UEdGraphNode* Existing : Graph->Nodes)
				{
					if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Existing))
					{
						ResultNode = Result;
						Data->SetObjectField(TEXT("node"), NodeToJson(Result));
						break;
					}
				}
				if (!ResultNode)
				{
					NewNode = NewObject<UK2Node_FunctionResult>(Graph);
				}
			}
			else
			{
				return FUplinkToolResult::Error(TEXT(
					"unknown 'kind'. Events and calls: call_function, custom_event, event, component_bound_event. "
					"Data: variable_get, variable_set, make_struct, break_struct, make_array, select, self. "
					"Flow: branch, sequence, cast, switch_enum, switch_int, switch_string, macro (ForEachLoop, ForLoop, WhileLoop, DoOnce, Gate, FlipFlop...), function_result. "
					"Dispatchers: bind_dispatcher, unbind_dispatcher, call_dispatcher, clear_dispatcher {name, target?}. Parent: call_parent {function}."));
			}

			if (NewNode)
			{
				FinalizeNewNode(Graph, NewNode, OpParams);
				Data->SetObjectField(TEXT("node"), NodeToJson(NewNode));
				ResultNode = NewNode;
			}
			const FString Ref = GetString(OpParams, TEXT("ref"));
			if (!Ref.IsEmpty() && ResultNode)
			{
				NodeRefs.Add(Ref, ResultNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			}
		}
		else if (Op == TEXT("arrange"))
		{
			UEdGraph* Graph = FindGraph(Blueprint, GetString(OpParams, TEXT("graph")), Error);
			if (!Graph)
			{
				return FUplinkToolResult::Error(Error);
			}
			Data->SetNumberField(TEXT("moved"), ArrangeGraph(Graph));
		}
		else if (Op == TEXT("connect"))
		{
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
		}
		else if (Op == TEXT("break_links") || Op == TEXT("delete_node") || Op == TEXT("set_pin_default"))
		{
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
					const FString Value = GetString(OpParams, TEXT("value"));
					const FName Category = Pin->PinType.PinCategory;
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
		}
		else
		{
			return FUplinkToolResult::Error(TEXT("unknown op (add_variable, set_variable, remove_variable, add_dispatcher, implement_interface, remove_interface, override_function, add_function, remove_function, add_node, arrange, connect, break_links, delete_node, set_pin_default, set_node_property)"));
		}
		return FUplinkToolResult::Ok();
	}
}

void UplinkTools::RegisterBlueprint(FUplinkToolRegistry& Registry)
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

	Registry.RegisterQuick(
		TEXT("bp_modify"),
		TEXT("Edit a Blueprint - one op, or a whole batch in a single call via 'ops': [{op:..., ...}, ...]. In a batch, give add_node ops a 'ref' name and later ops can address that node as '@ref' (from_node/to_node/node) - an entire event graph builds in ONE request. Ops: add_variable {name,type,default?,instance_editable?,expose_on_spawn?,read_only?,replicated?,category?,tooltip?} | set_variable {name, same options} - configure an existing variable | remove_variable {name} | add_dispatcher {name, inputs?} - declare an event dispatcher | implement_interface {interface} / remove_interface {interface, preserve_functions?} | override_function {name, as_function?} - override a parent or interface function | add_function {name, inputs?, outputs?, pure?, thread_safe?, category?} | add_node {kind: call_function {class,function} | custom_event {name} | event {name - e.g. ReceiveBeginPlay/ReceiveTick; reuses a matching ghost/existing event node} | component_bound_event {component, event - e.g. component 'MyButton' event 'OnClicked', or 'NiagaraComp' + 'OnSystemFinished'} | variable_get/variable_set {name} | branch | sequence | cast {class, pure?} | switch_enum {enum} | switch_int | switch_string | macro {name: ForEachLoop/ForLoop/WhileLoop/DoOnce/Gate/FlipFlop..., library?} | make_struct/break_struct {struct} | make_array | select | self | function_result | bind_dispatcher/unbind_dispatcher/call_dispatcher/clear_dispatcher {name, target?} - pins are 'Delegate' (shown as Event) and 'self' (shown as Target), graph?, x?, y?, ref?} | arrange {graph?} - auto-layout: dependency columns, exec chains as straight horizontal lanes, data nodes below, reroute knots at turns; finish a batch with it | connect {graph?, from_node, from_pin, to_node, to_pin} | break_links {graph?, node, pin} | delete_node {graph?, node} | set_pin_default {graph?, node, pin, value}. New nodes never overlap existing ones (positions are nudged to free space; omit x/y for auto-placement). Node handles are guids from bp_query, the add_node response, or '@ref'. A failed batch op stops the batch (earlier ops stay applied). Set compile=true to compile at the end."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"},"op":{"type":"string","enum":["add_variable","set_variable","remove_variable","add_dispatcher","implement_interface","remove_interface","override_function","add_function","remove_function","add_node","arrange","connect","break_links","delete_node","set_pin_default","set_node_property"]},"ops":{"type":"array","items":{"type":"object"},"description":"Batch mode: sequence of op objects (same fields as single-op form, plus 'ref' on add_node)"},"name":{"type":"string"},"type":{"type":"string"},"default":{"type":"string"},"instance_editable":{"type":"boolean","description":"add/set_variable: editable on placed instances"},"expose_on_spawn":{"type":"boolean","description":"add/set_variable: show as a SpawnActor pin"},"read_only":{"type":"boolean","description":"add/set_variable: blueprint-read-only"},"replicated":{"type":"boolean","description":"add/set_variable: replicate this property"},"tooltip":{"type":"string","description":"add/set_variable"},"kind":{"type":"string","enum":["call_function","custom_event","event","component_bound_event","variable_get","variable_set","branch","sequence","cast","switch_enum","switch_int","switch_string","macro","make_struct","break_struct","make_array","select","self","function_result","call_parent","bind_dispatcher","unbind_dispatcher","call_dispatcher","clear_dispatcher"]},"class":{"type":"string","description":"add_node call_function: class path or 'self'; add_node cast: the class to cast TO"},"struct":{"type":"string","description":"make_struct/break_struct: script struct path, e.g. /Script/CoreUObject.Vector"},"enum":{"type":"string","description":"switch_enum: enum path, e.g. /Game/E_State.E_State"},"library":{"type":"string","description":"macro: Blueprint holding the macro (default the engine StandardMacros)"},"target":{"type":"string","description":"dispatcher kinds: class owning the dispatcher, e.g. /Game/BP_Door.BP_Door_C. Omit for one on this blueprint"},"interface":{"type":"string","description":"implement_interface/remove_interface: /Script/Module.IThing or /Game/BPI_Thing"},"preserve_functions":{"type":"boolean","description":"remove_interface: keep the implementations as normal functions"},"as_function":{"type":"boolean","description":"override_function: force the function-graph form instead of an event node"},"function":{"type":"string"},"component":{"type":"string","description":"component_bound_event: component/widget variable name"},"event":{"type":"string","description":"component_bound_event: delegate name on the component's class"},"ref":{"type":"string","description":"add_node: name this node for '@ref' handles in later batch ops"},"graph":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"from_node":{"type":"string"},"from_pin":{"type":"string"},"to_node":{"type":"string"},"to_pin":{"type":"string"},"node":{"type":"string"},"pin":{"type":"string"},"value":{"type":"string"},"compile":{"type":"boolean"},"thread_safe":{"type":"boolean","description":"add_function: required for anim-graph node functions, which cannot run on the game thread"},"pure":{"type":"boolean","description":"add_function: no exec pins"},"category":{"type":"string","description":"add_function / add_variable / set_variable: category"},"inputs":{"type":"array","items":{"type":"object"},"description":"add_function: [{name, type, by_ref?, const?}] - by_ref+const are needed to match prototype-validated signatures such as anim node bindings"},"outputs":{"type":"array","items":{"type":"object"},"description":"add_function: [{name, type}] return values - creates the Result node, wired to the entry"},"property":{"type":"string","description":"set_node_property: property on the node, dotted paths allowed"},"reconstruct":{"type":"boolean","description":"set_node_property: rebuild the node's pins afterwards (default true)"},"save":{"type":"boolean","default":false,"description":"Write the blueprint to disk afterwards. Edits are in memory until saved, so an editor restart discards them. Skipped if compile:true reported errors."}},"required":["blueprint"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UBlueprint* Blueprint = LoadBlueprint(Ctx, Error);
			if (!Blueprint)
			{
				return FUplinkToolResult::Error(Error);
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			TMap<FString, FString> NodeRefs;

			const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
			if (Ctx.Params->TryGetArrayField(FStringView(TEXT("ops")), Ops))
			{
				TArray<TSharedPtr<FJsonValue>> Results;
				for (int32 Index = 0; Index < Ops->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject>* OpObject = nullptr;
					if (!(*Ops)[Index].IsValid() || !(*Ops)[Index]->TryGetObject(OpObject) || !OpObject->IsValid())
					{
						return FUplinkToolResult::Error(FString::Printf(TEXT("ops[%d] is not an object"), Index));
					}
					TSharedRef<FJsonObject> OpData = MakeShared<FJsonObject>();
					const FUplinkToolResult OpResult = ApplyGraphOp(Blueprint, *OpObject, NodeRefs, OpData);
					if (OpResult.bError)
					{
						// Earlier ops have already been applied, so hand back
						// what succeeded - otherwise the caller cannot tell how
						// far the batch got and has to re-derive it.
						Data->SetArrayField(TEXT("results"), Results);
						Data->SetNumberField(TEXT("failedAt"), Index);
						Data->SetNumberField(TEXT("applied"), Results.Num());
						FUplinkToolResult Out = FUplinkToolResult::Ok(Data, FString::Printf(
							TEXT("ops[%d] (%s) failed: %s - the %d op(s) before it were applied, blueprint not compiled"),
							Index, *GetString(*OpObject, TEXT("op")), *OpResult.Message, Results.Num()));
						Out.bError = true;
						return Out;
					}
					Results.Add(MakeShared<FJsonValueObject>(OpData));
				}
				Data->SetArrayField(TEXT("results"), Results);
			}
			else
			{
				const FUplinkToolResult OpResult = ApplyGraphOp(Blueprint, Ctx.Params, NodeRefs, Data);
				if (OpResult.bError)
				{
					return OpResult;
				}
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

			// Graph edits live only in memory until the package is written, so
			// an editor restart silently discards them. 'save' makes a run of
			// edits durable without a second round trip.
			bool bSave = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("save")), bSave);
			auto SaveIfAsked = [Blueprint, bSave, &Data]()
			{
				if (!bSave)
				{
					return;
				}
				UPackage* Package = Blueprint->GetOutermost();
				TArray<UPackage*> ToSave{ Package };
				Data->SetBoolField(TEXT("saved"), SavePackagesUnattended(ToSave, /*bCheckDirty=*/false));
			};

			bool bCompile = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("compile")), bCompile);
			if (bCompile)
			{
				FUplinkToolResult CompileResult = CompileAndReport(Blueprint);
				if (CompileResult.Data.IsValid())
				{
					Data->SetObjectField(TEXT("compile"), CompileResult.Data);
				}
				// Only persist a blueprint that compiled - saving a broken one
				// makes the breakage survive the restart too.
				if (!CompileResult.bError)
				{
					SaveIfAsked();
				}
				FUplinkToolResult Out = FUplinkToolResult::Ok(Data, CompileResult.Message);
				Out.bError = CompileResult.bError;
				return Out;
			}
			SaveIfAsked();
			return FUplinkToolResult::Ok(Data, TEXT("modified (not compiled)"));
		});

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

	Registry.RegisterQuick(
		TEXT("bp_compile"),
		TEXT("Compile a Blueprint and return its errors and warnings."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"}},"required":["blueprint"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UBlueprint* Blueprint = LoadBlueprint(Ctx, Error);
			if (!Blueprint)
			{
				return FUplinkToolResult::Error(Error);
			}
			return CompileAndReport(Blueprint);
		});
}
