// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// AI tools: ai_query - what the AI controllers in a running game are actually
// doing. Blackboard values are reachable by reflection one key at a time, but
// which behaviour tree node is currently active is not exposed to Blueprint at
// all, and that is usually the question being asked.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BlackboardData.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"

using namespace UplinkToolUtil;

namespace
{
	/** Every key on the blackboard with its current value, as the debugger shows it. */
	TSharedRef<FJsonObject> DescribeBlackboard(const UBlackboardComponent* Blackboard)
	{
		TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
		if (!Blackboard)
		{
			return Values;
		}
		const int32 NumKeys = Blackboard->GetNumKeys();
		for (int32 Index = 0; Index < NumKeys; ++Index)
		{
			const FBlackboard::FKey Key(Index);
			const FName KeyName = Blackboard->GetKeyName(Key);
			if (KeyName.IsNone())
			{
				continue;
			}
			Values->SetStringField(KeyName.ToString(),
				Blackboard->DescribeKeyValue(Key, EBlackboardDescription::OnlyValue));
		}
		return Values;
	}
}

void UplinkTools::RegisterAI(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("ai_query"),
		TEXT("What the AI in the running game is doing: for each AI controller, its pawn, the behaviour tree it is running, the currently active node, the active task chain, and the full blackboard. The active node answers 'why is this enemy doing that' and is not reachable any other way - it is not exposed to Blueprint. Filter with 'actor' (matches controller or pawn name/label). Normally used against PIE; an editor world has no running AI."),
		TEXT(R"json({"type":"object","properties":{"actor":{"type":"string","description":"Only controllers whose own name/label, or their pawn's, contains this"},"blackboard":{"type":"boolean","default":true},"max":{"type":"number","default":20},"world":{"type":"string","enum":["editor","pie"]}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			if (!World)
			{
				return FUplinkToolResult::Error(Error);
			}

			const FString Filter = GetString(Ctx.Params, TEXT("actor"));
			bool bWantBlackboard = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("blackboard")), bWantBlackboard);
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 20.0)), 1, 200);

			TArray<TSharedPtr<FJsonValue>> Controllers;
			int32 TotalSeen = 0;
			for (TActorIterator<AAIController> It(World); It; ++It)
			{
				AAIController* Controller = *It;
				if (!Controller)
				{
					continue;
				}
				const APawn* Pawn = Controller->GetPawn();
				if (!Filter.IsEmpty())
				{
					const bool bMatches =
						Controller->GetName().Contains(Filter, ESearchCase::IgnoreCase) ||
						Controller->GetActorLabel().Contains(Filter, ESearchCase::IgnoreCase) ||
						(Pawn && (Pawn->GetName().Contains(Filter, ESearchCase::IgnoreCase) ||
							Pawn->GetActorLabel().Contains(Filter, ESearchCase::IgnoreCase)));
					if (!bMatches)
					{
						continue;
					}
				}
				++TotalSeen;
				if (Controllers.Num() >= Max)
				{
					continue;
				}

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("controller"), Controller->GetName());
				Row->SetStringField(TEXT("controllerClass"), Controller->GetClass()->GetName());
				Row->SetStringField(TEXT("pawn"), Pawn ? Pawn->GetName() : FString());
				if (Pawn)
				{
					Row->SetStringField(TEXT("pawnClass"), Pawn->GetClass()->GetName());
				}

				if (const UBehaviorTreeComponent* Tree = Cast<UBehaviorTreeComponent>(Controller->GetBrainComponent()))
				{
					Row->SetStringField(TEXT("behaviorTree"),
						Tree->GetCurrentTree() ? Tree->GetCurrentTree()->GetName() : FString());
					Row->SetBoolField(TEXT("running"), Tree->IsRunning());
					if (const UBTNode* Active = Tree->GetActiveNode())
					{
						Row->SetStringField(TEXT("activeNode"), Active->GetNodeName());
						Row->SetStringField(TEXT("activeNodeClass"), Active->GetClass()->GetName());
					}
					Row->SetStringField(TEXT("activeTasks"), Tree->DescribeActiveTasks());
				}
				else if (Controller->GetBrainComponent())
				{
					// A brain that is not a behaviour tree (a StateTree, or a
					// custom one) still deserves to be named rather than hidden.
					Row->SetStringField(TEXT("brain"),
						Controller->GetBrainComponent()->GetClass()->GetName());
				}

				if (bWantBlackboard)
				{
					if (const UBlackboardComponent* Blackboard = Controller->GetBlackboardComponent())
					{
						Row->SetObjectField(TEXT("blackboard"), DescribeBlackboard(Blackboard));
					}
				}
				Controllers.Add(MakeShared<FJsonValueObject>(Row));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("controllers"), Controllers);
			Data->SetNumberField(TEXT("count"), Controllers.Num());
			Data->SetNumberField(TEXT("totalMatching"), TotalSeen);
			Data->SetBoolField(TEXT("truncated"), TotalSeen > Controllers.Num());
			return FUplinkToolResult::Ok(Data, Controllers.Num() == 0
				? TEXT("no AI controllers here (is the game running? AI only exists in a PIE world)")
				: FString());
		});
}
