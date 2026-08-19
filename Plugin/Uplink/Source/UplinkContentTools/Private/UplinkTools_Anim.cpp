// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Animation tools: anim_query (read timing data - the frame truth needed to
// place gameplay events correctly instead of guessing with Delay nodes) and
// anim_modify (add/remove notifies at exact times).

#include "UplinkContentTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Animation/Skeleton.h"
#include "AnimationStateMachineGraph.h"
#include "EdGraph/EdGraph.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"

using namespace UplinkToolUtil;

namespace
{
	UAnimSequenceBase* LoadAnim(const FUplinkToolContext& Ctx, FString& OutError)
	{
		const FString Path = GetString(Ctx.Params, TEXT("asset"));
		UAnimSequenceBase* Anim = LoadObject<UAnimSequenceBase>(nullptr, *Path);
		if (!Anim)
		{
			OutError = FString::Printf(TEXT("animation asset not found: %s (montage or sequence)"), *Path);
		}
		return Anim;
	}
}

void UplinkContentTools::RegisterAnim(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("anim_query"),
		TEXT("Read an animation montage or sequence: play length, frame rate (sequences), montage sections, and every notify with its exact trigger time - the data needed to hook gameplay (damage, collision, sounds) to the right frame instead of guessing with delays."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string","description":"UAnimMontage or UAnimSequence asset path"}},"required":["asset"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UAnimSequenceBase* Anim = LoadAnim(Ctx, Error);
			if (!Anim)
			{
				return FUplinkToolResult::Error(Error);
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("asset"), Anim->GetPathName());
			Data->SetStringField(TEXT("class"), Anim->GetClass()->GetName());
			Data->SetNumberField(TEXT("length_seconds"), Anim->GetPlayLength());

			if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Anim))
			{
				const double FrameRate = Sequence->GetSamplingFrameRate().AsDecimal();
				Data->SetNumberField(TEXT("frame_rate"), FrameRate);
				Data->SetNumberField(TEXT("num_frames"), Sequence->GetNumberOfSampledKeys());
			}

			if (const UAnimMontage* Montage = Cast<UAnimMontage>(Anim))
			{
				TArray<TSharedPtr<FJsonValue>> Sections;
				for (const FCompositeSection& Section : Montage->CompositeSections)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Section.SectionName.ToString());
					Row->SetNumberField(TEXT("time"), Section.GetTime());
					Sections.Add(MakeShared<FJsonValueObject>(Row));
				}
				Data->SetArrayField(TEXT("sections"), Sections);
			}

			TArray<TSharedPtr<FJsonValue>> Notifies;
			for (const FAnimNotifyEvent& Event : Anim->Notifies)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Event.NotifyName.ToString());
				Row->SetNumberField(TEXT("time"), Event.GetTriggerTime());
				if (Event.GetDuration() > 0.0f)
				{
					Row->SetNumberField(TEXT("duration"), Event.GetDuration());
				}
				Row->SetNumberField(TEXT("track"), Event.TrackIndex);
				if (Event.Notify)
				{
					Row->SetStringField(TEXT("notify_class"), Event.Notify->GetClass()->GetPathName());
				}
				else if (Event.NotifyStateClass)
				{
					Row->SetStringField(TEXT("notify_state_class"), Event.NotifyStateClass->GetClass()->GetPathName());
				}
				Notifies.Add(MakeShared<FJsonValueObject>(Row));
			}
			Data->SetArrayField(TEXT("notifies"), Notifies);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("anim_modify"),
		TEXT("Edit an animation's notifies. op add_notify {name, time (seconds) or frame (sequences only), notify_class?}: a name-only notify becomes a skeleton notify (fires AnimNotify_<Name> in the Anim Blueprint and is delivered through montage OnNotifyBegin); pass notify_class for a UAnimNotify subclass instead. op remove_notify {name or index}. The asset is marked dirty, not saved."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"op":{"type":"string","enum":["add_notify","remove_notify"]},"name":{"type":"string"},"time":{"type":"number","description":"Trigger time in seconds"},"frame":{"type":"number","description":"Alternative to time, sequences only"},"track":{"type":"number","default":0},"notify_class":{"type":"string","description":"Optional UAnimNotify subclass path"},"index":{"type":"number","description":"remove_notify: notify index from anim_query order"}},"required":["asset","op"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UAnimSequenceBase* Anim = LoadAnim(Ctx, Error);
			if (!Anim)
			{
				return FUplinkToolResult::Error(Error);
			}
			const FString Op = GetString(Ctx.Params, TEXT("op"));
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

			if (Op == TEXT("add_notify"))
			{
				const FName NotifyName(*GetString(Ctx.Params, TEXT("name")));
				if (NotifyName.IsNone())
				{
					return FUplinkToolResult::Error(TEXT("'name' is required"));
				}

				double Time = GetNumber(Ctx.Params, TEXT("time"), -1.0);
				if (Time < 0.0 && Ctx.Params->HasField(FStringView(TEXT("frame"))))
				{
					const UAnimSequence* Sequence = Cast<UAnimSequence>(Anim);
					if (!Sequence)
					{
						return FUplinkToolResult::Error(TEXT("'frame' only works on sequences; use 'time' for montages"));
					}
					Time = GetNumber(Ctx.Params, TEXT("frame"), 0.0) / Sequence->GetSamplingFrameRate().AsDecimal();
				}
				if (Time < 0.0)
				{
					return FUplinkToolResult::Error(TEXT("provide 'time' (seconds) or 'frame'"));
				}
				Time = FMath::Clamp(Time, 0.0, static_cast<double>(Anim->GetPlayLength()));

				FAnimNotifyEvent NewEvent;
				NewEvent.NotifyName = NotifyName;
				NewEvent.Link(Anim, static_cast<float>(Time));
				NewEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(
					Anim->CalculateOffsetForNotify(static_cast<float>(Time)));
				NewEvent.TrackIndex = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("track"), 0)), 0,
					FMath::Max(0, Anim->AnimNotifyTracks.Num() - 1));

				const FString NotifyClassPath = GetString(Ctx.Params, TEXT("notify_class"));
				if (!NotifyClassPath.IsEmpty())
				{
					UClass* NotifyClass = StaticLoadClass(UAnimNotify::StaticClass(), nullptr, *NotifyClassPath);
					if (!NotifyClass)
					{
						return FUplinkToolResult::Error(TEXT("notify_class not found or not a UAnimNotify subclass"));
					}
					NewEvent.Notify = NewObject<UAnimNotify>(Anim, NotifyClass);
				}
				else if (USkeleton* Skeleton = Anim->GetSkeleton())
				{
					// Register the skeleton notify name so it shows up in editor UI.
					Skeleton->AddNewAnimationNotify(NotifyName);
				}

				Anim->Modify();
				Anim->Notifies.Add(NewEvent);
				Anim->RefreshCacheData();
				Anim->MarkPackageDirty();

				Data->SetStringField(TEXT("notify"), NotifyName.ToString());
				Data->SetNumberField(TEXT("time"), Time);
				return FUplinkToolResult::Ok(Data, TEXT("notify added (asset dirty, not saved)"));
			}

			if (Op == TEXT("remove_notify"))
			{
				Anim->Modify();
				int32 Removed = 0;
				if (Ctx.Params->HasField(FStringView(TEXT("index"))))
				{
					const int32 Index = static_cast<int32>(GetNumber(Ctx.Params, TEXT("index"), -1));
					if (!Anim->Notifies.IsValidIndex(Index))
					{
						return FUplinkToolResult::Error(TEXT("notify index out of range"));
					}
					Anim->Notifies.RemoveAt(Index);
					Removed = 1;
				}
				else
				{
					const FName NotifyName(*GetString(Ctx.Params, TEXT("name")));
					Removed = Anim->Notifies.RemoveAll([&NotifyName](const FAnimNotifyEvent& Event)
					{
						return Event.NotifyName == NotifyName;
					});
				}
				Anim->RefreshCacheData();
				Anim->MarkPackageDirty();
				Data->SetNumberField(TEXT("removed"), Removed);
				return FUplinkToolResult::Ok(Data);
			}

			return FUplinkToolResult::Error(TEXT("op must be add_notify or remove_notify"));
		});

	Registry.RegisterQuick(
		TEXT("skeleton_query"),
		TEXT("Read a skeleton's bone hierarchy and sockets. 'asset' is a Skeleton or SkeletalMesh. Bones: name, parent, index (+ local ref-pose transforms with transforms:true). Sockets: name, bone, relative location/rotation."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"transforms":{"type":"boolean","default":false},"bone_contains":{"type":"string"}},"required":["asset"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Path = GetString(Ctx.Params, TEXT("asset"));
			UObject* Asset = LoadObject<UObject>(nullptr, *Path);
			USkeleton* Skeleton = Cast<USkeleton>(Asset);
			USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset);
			if (Mesh && !Skeleton)
			{
				Skeleton = Mesh->GetSkeleton();
			}
			if (!Skeleton)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("no skeleton found at %s (pass a Skeleton or SkeletalMesh)"), *Path));
			}

			const FReferenceSkeleton& RefSkeleton = Mesh ? Mesh->GetRefSkeleton() : Skeleton->GetReferenceSkeleton();
			bool bTransforms = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("transforms")), bTransforms);
			const FString BoneFilter = GetString(Ctx.Params, TEXT("bone_contains"));

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());

			const TArray<FMeshBoneInfo>& BoneInfo = RefSkeleton.GetRefBoneInfo();
			const TArray<FTransform>& BonePose = RefSkeleton.GetRefBonePose();
			TArray<TSharedPtr<FJsonValue>> Bones;
			for (int32 Index = 0; Index < BoneInfo.Num(); ++Index)
			{
				const FString BoneName = BoneInfo[Index].Name.ToString();
				if (!BoneFilter.IsEmpty() && !BoneName.Contains(BoneFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), BoneName);
				Row->SetNumberField(TEXT("index"), Index);
				const int32 ParentIndex = BoneInfo[Index].ParentIndex;
				if (ParentIndex != INDEX_NONE)
				{
					Row->SetStringField(TEXT("parent"), BoneInfo[ParentIndex].Name.ToString());
				}
				if (bTransforms && BonePose.IsValidIndex(Index))
				{
					Row->SetObjectField(TEXT("location"), VectorToJson(BonePose[Index].GetLocation()));
					const FRotator Rotation = BonePose[Index].GetRotation().Rotator();
					TSharedRef<FJsonObject> RotJson = MakeShared<FJsonObject>();
					RotJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
					RotJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
					RotJson->SetNumberField(TEXT("roll"), Rotation.Roll);
					Row->SetObjectField(TEXT("rotation"), RotJson);
				}
				Bones.Add(MakeShared<FJsonValueObject>(Row));
			}
			Data->SetArrayField(TEXT("bones"), Bones);
			Data->SetNumberField(TEXT("total_bones"), BoneInfo.Num());

			TArray<TSharedPtr<FJsonValue>> Sockets;
			auto AddSocket = [&Sockets](const USkeletalMeshSocket* Socket, const TCHAR* Source)
			{
				if (!Socket)
				{
					return;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Socket->SocketName.ToString());
				Row->SetStringField(TEXT("bone"), Socket->BoneName.ToString());
				Row->SetObjectField(TEXT("location"), VectorToJson(Socket->RelativeLocation));
				TSharedRef<FJsonObject> RotJson = MakeShared<FJsonObject>();
				RotJson->SetNumberField(TEXT("pitch"), Socket->RelativeRotation.Pitch);
				RotJson->SetNumberField(TEXT("yaw"), Socket->RelativeRotation.Yaw);
				RotJson->SetNumberField(TEXT("roll"), Socket->RelativeRotation.Roll);
				Row->SetObjectField(TEXT("rotation"), RotJson);
				Row->SetStringField(TEXT("owner"), Source);
				Sockets.Add(MakeShared<FJsonValueObject>(Row));
			};
			for (const TObjectPtr<USkeletalMeshSocket>& Socket : Skeleton->Sockets)
			{
				AddSocket(Socket, TEXT("skeleton"));
			}
			if (Mesh)
			{
				for (const USkeletalMeshSocket* Socket : Mesh->GetMeshOnlySocketList())
				{
					AddSocket(Socket, TEXT("mesh"));
				}
			}
			Data->SetArrayField(TEXT("sockets"), Sockets);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("socket_modify"),
		TEXT("Add, update, or remove a skeleton socket - attachment points for weapons/props. 'asset' is a Skeleton or SkeletalMesh (writes go to the skeleton). op: add {name, bone, location?, rotation?, scale?} | update {name, bone?, location?, rotation?, scale?} | remove {name}. Marked dirty, not saved."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"op":{"type":"string","enum":["add","update","remove"]},"name":{"type":"string"},"bone":{"type":"string"},"location":{"type":"object"},"rotation":{"type":"object"},"scale":{"type":"object"}},"required":["asset","op","name"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Path = GetString(Ctx.Params, TEXT("asset"));
			UObject* Asset = LoadObject<UObject>(nullptr, *Path);
			USkeleton* Skeleton = Cast<USkeleton>(Asset);
			if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
			{
				Skeleton = Mesh->GetSkeleton();
			}
			if (!Skeleton)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("no skeleton found at %s"), *Path));
			}

			const FString Op = GetString(Ctx.Params, TEXT("op"));
			const FName SocketName(*GetString(Ctx.Params, TEXT("name")));
			USkeletalMeshSocket* Existing = nullptr;
			for (const TObjectPtr<USkeletalMeshSocket>& Socket : Skeleton->Sockets)
			{
				if (Socket && Socket->SocketName == SocketName)
				{
					Existing = Socket;
					break;
				}
			}

			Skeleton->Modify();
			if (Op == TEXT("add"))
			{
				if (Existing)
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("socket '%s' already exists"), *SocketName.ToString()));
				}
				const FName BoneName(*GetString(Ctx.Params, TEXT("bone")));
				if (Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) == INDEX_NONE)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("bone '%s' not found on %s (skeleton_query lists bones)"), *BoneName.ToString(), *Skeleton->GetName()));
				}
				USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Skeleton);
				Socket->SocketName = SocketName;
				Socket->BoneName = BoneName;
				Existing = Socket;
				Skeleton->Sockets.Add(Socket);
			}
			else if (Op == TEXT("remove"))
			{
				if (!Existing)
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("socket '%s' not found"), *SocketName.ToString()));
				}
				Skeleton->Sockets.Remove(Existing);
				Skeleton->MarkPackageDirty();
				return FUplinkToolResult::Ok(nullptr, TEXT("socket removed"));
			}
			else if (Op != TEXT("update"))
			{
				return FUplinkToolResult::Error(TEXT("op must be add, update, or remove"));
			}
			if (!Existing)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("socket '%s' not found"), *SocketName.ToString()));
			}

			const FString NewBone = GetString(Ctx.Params, TEXT("bone"));
			if (Op == TEXT("update") && !NewBone.IsEmpty())
			{
				const FName BoneName(*NewBone);
				if (Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) == INDEX_NONE)
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("bone '%s' not found"), *NewBone));
				}
				Existing->BoneName = BoneName;
			}
			FVector Location, Scale;
			FRotator Rotation;
			if (TryGetVector(Ctx.Params, TEXT("location"), Location))
			{
				Existing->RelativeLocation = Location;
			}
			if (TryGetRotator(Ctx.Params, TEXT("rotation"), Rotation))
			{
				Existing->RelativeRotation = Rotation;
			}
			if (TryGetVector(Ctx.Params, TEXT("scale"), Scale))
			{
				Existing->RelativeScale = Scale;
			}
			Skeleton->MarkPackageDirty();

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("socket"), Existing->SocketName.ToString());
			Data->SetStringField(TEXT("bone"), Existing->BoneName.ToString());
			Data->SetObjectField(TEXT("location"), VectorToJson(Existing->RelativeLocation));
			Data->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("animbp_query"),
		TEXT("Read an Animation Blueprint's anim-specific structure: target skeleton, state machines with their states and transitions, and every anim graph node (class + title, which names the assets it plays). Pair with bp_query for the event graph. State machines are always returned in full; the flat node list is capped, and the reply carries 'total_anim_nodes' and 'truncated'."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"},"max":{"type":"number","default":150,"description":"Maximum anim graph nodes returned"}},"required":["blueprint"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Path = GetString(Ctx.Params, TEXT("blueprint"));
			UAnimBlueprint* AnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *Path);
			if (!AnimBlueprint)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("anim blueprint not found: %s"), *Path));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("blueprint"), AnimBlueprint->GetPathName());
			Data->SetStringField(TEXT("skeleton"), AnimBlueprint->TargetSkeleton ? AnimBlueprint->TargetSkeleton->GetPathName() : TEXT(""));

			TArray<UEdGraph*> AllGraphs;
			AnimBlueprint->GetAllGraphs(AllGraphs);

			// A production anim blueprint runs to hundreds of nodes; the state
			// machines are the useful summary, so cap the flat node list first.
			const int32 MaxNodes = FMath::Clamp(
				static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 150.0)), 1, 2000);
			int32 TotalAnimNodes = 0;

			TArray<TSharedPtr<FJsonValue>> StateMachines;
			TArray<TSharedPtr<FJsonValue>> AnimNodes;
			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph)
				{
					continue;
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UAnimGraphNode_StateMachineBase* Machine = Cast<UAnimGraphNode_StateMachineBase>(Node))
					{
						TSharedRef<FJsonObject> MachineRow = MakeShared<FJsonObject>();
						MachineRow->SetStringField(TEXT("name"), Machine->GetNodeTitle(ENodeTitleType::ListView).ToString());
						TArray<TSharedPtr<FJsonValue>> States;
						TArray<TSharedPtr<FJsonValue>> Transitions;
						if (Machine->EditorStateMachineGraph)
						{
							for (UEdGraphNode* StateGraphNode : Machine->EditorStateMachineGraph->Nodes)
							{
								if (UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(StateGraphNode))
								{
									TSharedRef<FJsonObject> TransitionRow = MakeShared<FJsonObject>();
									UAnimStateNodeBase* From = Transition->GetPreviousState();
									UAnimStateNodeBase* To = Transition->GetNextState();
									TransitionRow->SetStringField(TEXT("from"), From ? From->GetStateName() : TEXT(""));
									TransitionRow->SetStringField(TEXT("to"), To ? To->GetStateName() : TEXT(""));
									Transitions.Add(MakeShared<FJsonValueObject>(TransitionRow));
								}
								else if (UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(StateGraphNode))
								{
									States.Add(MakeShared<FJsonValueString>(State->GetStateName()));
								}
							}
						}
						MachineRow->SetArrayField(TEXT("states"), States);
						MachineRow->SetArrayField(TEXT("transitions"), Transitions);
						StateMachines.Add(MakeShared<FJsonValueObject>(MachineRow));
					}
					else if (Node && Node->GetClass()->GetName().StartsWith(TEXT("AnimGraphNode_")))
					{
						++TotalAnimNodes;
						if (AnimNodes.Num() >= MaxNodes)
						{
							continue;
						}
						TSharedRef<FJsonObject> NodeRow = MakeShared<FJsonObject>();
						NodeRow->SetStringField(TEXT("class"), Node->GetClass()->GetName());
						NodeRow->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
						NodeRow->SetStringField(TEXT("graph"), Graph->GetName());
						AnimNodes.Add(MakeShared<FJsonValueObject>(NodeRow));
					}
				}
			}
			Data->SetArrayField(TEXT("state_machines"), StateMachines);
			Data->SetArrayField(TEXT("anim_nodes"), AnimNodes);
			Data->SetNumberField(TEXT("total_anim_nodes"), TotalAnimNodes);
			Data->SetBoolField(TEXT("truncated"), TotalAnimNodes > AnimNodes.Num());
			return FUplinkToolResult::Ok(Data);
		});
}
