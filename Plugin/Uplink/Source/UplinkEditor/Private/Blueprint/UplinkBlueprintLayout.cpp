// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Where a node lands: overlap-free placement for a newly added node, and the
// whole-graph tidy behind the 'arrange' op.

#include "Blueprint/UplinkBlueprintCommon.h"
#include "UplinkToolUtil.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Knot.h"

using namespace UplinkToolUtil;

namespace UplinkBlueprint
{
	static FVector2D EstimateNodeSize(const UEdGraphNode* Node)
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
	static void PlaceNodeWithoutOverlap(UEdGraph* Graph, UEdGraphNode* Node, bool bHasExplicitPos)
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

	/** Estimated Y of a pin's row, for headless layout decisions. */
	static float EstimatePinY(const UEdGraphNode* Node, const UEdGraphPin* Pin)
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
	static void RemoveKnots(UEdGraph* Graph)
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
	static int32 InsertKnotsAtTurns(UEdGraph* Graph)
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
}
