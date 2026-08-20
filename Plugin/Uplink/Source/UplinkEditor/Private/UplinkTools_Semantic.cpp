// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// observe - one player-centred snapshot of the running game.
//
// A screenshot tells an agent what the game looks like; it does not tell it
// that the door two metres ahead is the thing with an OnOpened event and no
// collision. Reading that out of the existing tools takes a level_actors call,
// a get_property per actor, a trace and a class_info, and by then the frame has
// moved on. This gathers it in one pass, from the player's position, ordered by
// what is closest.
//
// Everything here is a fact the engine already knows. There is deliberately no
// "interactable: true" field: that would be a guess dressed as an observation,
// and an agent acting on a wrong guess is exactly the failure this project
// keeps trying to design out. Collision profile, current overlap, physics
// state and the actor's own event list are reported instead, because those are
// what an interaction is actually made of.

#include "UplinkTools.h"
#include "UplinkEventRecorder.h"
#include "UplinkObservation.h"
#include "UObject/UObjectIterator.h"
#include "UplinkToolRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "UplinkToolUtil.h"

#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Info.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

using namespace UplinkToolUtil;

namespace
{
	/**
	 * Multicast delegates a Blueprint could bind on this actor, minus the ones
	 * every actor has.
	 *
	 * Without that subtraction the field is noise: AActor itself declares
	 * OnTakeAnyDamage, OnTakePointDamage, OnTakeRadialDamage, OnActorHit and
	 * the rest, so every actor in the list reports an identical set and the one
	 * event that actually distinguishes a door from a wall is buried. What is
	 * worth reporting is what this actor's own class added.
	 */
	TArray<FString> DistinctiveEventNames(const UClass* Class, int32 Max)
	{
		TArray<FString> Names;
		for (TFieldIterator<FMulticastDelegateProperty> It(Class); It && Names.Num() < Max; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_BlueprintAssignable))
			{
				continue;
			}
			// Declared at AActor or above (UObject) means every actor has it.
			const UClass* Owner = Cast<UClass>(It->GetOwnerStruct());
			if (Owner && AActor::StaticClass()->IsChildOf(Owner))
			{
				continue;
			}
			Names.AddUnique(It->GetName());
		}
		return Names;
	}

	FString MovementModeName(const APawn* Pawn)
	{
		const ACharacter* Character = Cast<ACharacter>(Pawn);
		const UCharacterMovementComponent* Movement =
			Character ? Character->GetCharacterMovement() : nullptr;
		if (!Movement)
		{
			return FString();
		}
		if (const UEnum* Enum = StaticEnum<EMovementMode>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Movement->MovementMode.GetValue()));
		}
		return FString();
	}

	/**
	 * AInfo covers GameMode, GameState, PlayerState, WorldSettings and friends -
	 * actors with no meaningful position. They sit at the origin, so a distance
	 * sort puts them first and pushes the things the player can actually reach
	 * off the end of the list.
	 *
	 * The two named classes are engine bookkeeping that a player can never
	 * touch: navigation data and the gameplay debugger's replicator. They are
	 * ordinary AActors, so nothing but their names excludes them, and both sit
	 * near the origin where they crowd out real answers. Naming them is
	 * arbitrary; leaving them in is worse.
	 */
	bool IsSpatiallyMeaningless(const AActor* Actor)
	{
		if (Actor->IsA<AInfo>() || Actor->GetRootComponent() == nullptr)
		{
			return true;
		}
		const FString ClassName = Actor->GetClass()->GetName();
		return ClassName.Contains(TEXT("AbstractNavData"))
			|| ClassName.Contains(TEXT("GameplayDebuggerCategoryReplicator"));
	}

	TSharedRef<FJsonObject> DescribeActor(
		AActor* Actor, double Distance, const TSet<AActor*>& Overlapping)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Actor->GetName());
#if WITH_EDITOR
		const FString Label = Actor->GetActorLabel();
		if (Label != Actor->GetName())
		{
			Obj->SetStringField(TEXT("label"), Label);
		}
#endif
		Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		Obj->SetNumberField(TEXT("distance"), Distance);
		Obj->SetObjectField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
		Obj->SetBoolField(TEXT("overlapping_player"), Overlapping.Contains(Actor));
		Obj->SetBoolField(TEXT("hidden"), Actor->IsHidden());

		// Not necessarily the root. A Blueprint actor's root is usually
		// DefaultSceneRoot, a bare USceneComponent with no collision at all, and
		// the mesh or trigger that decides how the actor behaves hangs under it.
		// Reading only the root reported nothing for every Blueprint actor -
		// which is most of the ones worth asking about.
		const UPrimitiveComponent* Collider = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
		if (!Collider)
		{
			TArray<UPrimitiveComponent*> Primitives;
			Actor->GetComponents<UPrimitiveComponent>(Primitives);
			for (const UPrimitiveComponent* Candidate : Primitives)
			{
				// Prefer one that actually collides over the first one found.
				if (Candidate && Candidate->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
				{
					Collider = Candidate;
					break;
				}
				if (!Collider)
				{
					Collider = Candidate;
				}
			}
		}
		if (Collider)
		{
			Obj->SetStringField(TEXT("collision_profile"), Collider->GetCollisionProfileName().ToString());
			Obj->SetStringField(TEXT("collision_enabled"),
				Collider->GetCollisionEnabled() == ECollisionEnabled::NoCollision ? TEXT("none") : TEXT("some"));
			Obj->SetBoolField(TEXT("simulates_physics"), Collider->IsSimulatingPhysics());
			if (Collider != Actor->GetRootComponent())
			{
				Obj->SetStringField(TEXT("collision_component"), Collider->GetName());
			}
		}

		TArray<FString> Tags;
		for (const FName& Tag : Actor->Tags)
		{
			Tags.Add(Tag.ToString());
		}
		if (Tags.Num() > 0)
		{
			Obj->SetStringField(TEXT("tags"), FString::Join(Tags, TEXT(", ")));
		}

		// What a caller would pass to watch_events to see this actor react.
		const TArray<FString> Events = DistinctiveEventNames(Actor->GetClass(), 12);
		if (Events.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> EventJson;
			for (const FString& Event : Events)
			{
				EventJson.Add(MakeShared<FJsonValueString>(Event));
			}
			Obj->SetArrayField(TEXT("events"), EventJson);
		}
		return Obj;
	}
}

void UplinkTools::RegisterSemantic(FUplinkToolRegistry& Registry, FUplinkEventRecorder& Recorder)
{
	Registry.RegisterQuick(
		TEXT("observe"),
		TEXT("One snapshot of the running game from the player's position: where the pawn is and what it is doing, ")
		TEXT("what is under the crosshair, and the nearest actors ordered by distance - each with its collision profile, ")
		TEXT("whether it is overlapping the player right now, and the names of the events it can fire (pass one to ")
		TEXT("watch_events). Reports facts only: there is no guess at what is 'interactable'. ")
		TEXT("'include' adds the rest of the situation to the same reply - 'screenshot' the frame itself as an image, ")
		TEXT("'ui' the UMG that is on screen with whether a click could land on it, 'events' the watched delegates that ")
		TEXT("have fired (set up with watch_events first; reading here takes nothing away from a drain_events loop). ")
		TEXT("Ask for all three and one call answers where the player is, what is around them, what is on screen and ")
		TEXT("what just happened - taken at one moment. Four separate calls cannot do that: each is a round trip ")
		TEXT("through the game thread and the game moves between them, so the answers describe four different moments."),
		TEXT(R"json({"type":"object","properties":{"radius":{"type":"number","default":1500,"minimum":1,"description":"Search distance in cm"},"max":{"type":"integer","default":20,"minimum":1,"description":"How many nearby actors to report"},"class_contains":{"type":"string","description":"Only report actors whose class name contains this"},"include":{"type":"array","items":{"type":"string","enum":["screenshot","ui","events"]},"description":"Extra sections to gather at the same moment"},"since_seq":{"type":"number","default":0,"description":"With include:['events'], report only events from this sequence on"},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"additionalProperties":false})json"),
		/*bReadOnly=*/true,
		[&Recorder](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return FUplinkToolResult::Error(WorldError);
			}

			APlayerController* PC = World->GetFirstPlayerController();
			APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			if (!Pawn)
			{
				return FUplinkToolResult::Error(
					TEXT("no player pawn in this world - observe is player-centred, so it needs a running game. ")
					TEXT("Start one with pie_start, or use level_actors to list a level without playing it."));
			}

			const double Radius = GetNumber(Ctx.Params, TEXT("radius"), 1500.0);
			const int32 Max = FMath::Max(1, static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 20.0)));
			const FString ClassFilter = GetString(Ctx.Params, TEXT("class_contains"));
			const FVector Origin = Pawn->GetActorLocation();

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> Player = MakeShared<FJsonObject>();
			Player->SetStringField(TEXT("pawn"), Pawn->GetName());
			Player->SetStringField(TEXT("class"), Pawn->GetClass()->GetName());
			Player->SetObjectField(TEXT("location"), VectorToJson(Origin));
			Player->SetObjectField(TEXT("velocity"), VectorToJson(Pawn->GetVelocity()));
			Player->SetNumberField(TEXT("speed"), Pawn->GetVelocity().Size());
			const FRotator Control = PC->GetControlRotation();
			TSharedRef<FJsonObject> ControlJson = MakeShared<FJsonObject>();
			ControlJson->SetNumberField(TEXT("pitch"), Control.Pitch);
			ControlJson->SetNumberField(TEXT("yaw"), Control.Yaw);
			ControlJson->SetNumberField(TEXT("roll"), Control.Roll);
			Player->SetObjectField(TEXT("control_rotation"), ControlJson);
			const FString Movement = MovementModeName(Pawn);
			if (!Movement.IsEmpty())
			{
				Player->SetStringField(TEXT("movement_mode"), Movement);
			}
			Data->SetObjectField(TEXT("player"), Player);

			// What the player is aimed at, which is usually the thing they are
			// about to interact with and rarely the nearest actor by distance.
			FVector ViewLocation = Origin;
			FRotator ViewRotation = Control;
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
			FCollisionQueryParams TraceParams(FName(TEXT("UplinkObserve")), /*bTraceComplex=*/false, Pawn);
			FHitResult Hit;
			const bool bHit = World->LineTraceSingleByChannel(
				Hit, ViewLocation, ViewLocation + ViewRotation.Vector() * Radius, ECC_Visibility, TraceParams);
			if (bHit && Hit.GetActor())
			{
				TSharedRef<FJsonObject> Looking = MakeShared<FJsonObject>();
				Looking->SetStringField(TEXT("actor"), Hit.GetActor()->GetName());
				Looking->SetStringField(TEXT("class"), Hit.GetActor()->GetClass()->GetName());
				Looking->SetNumberField(TEXT("distance"), Hit.Distance);
				Looking->SetObjectField(TEXT("impact"), VectorToJson(Hit.ImpactPoint));
				if (Hit.GetComponent())
				{
					Looking->SetStringField(TEXT("component"), Hit.GetComponent()->GetName());
				}
				Data->SetObjectField(TEXT("looking_at"), Looking);
			}
			else
			{
				// Explicit null, not a missing field. "I looked and there was
				// nothing" and "this tool did not report" are different facts,
				// and an absent key makes them look the same.
				Data->SetField(TEXT("looking_at"), MakeShared<FJsonValueNull>());
			}

			TArray<AActor*> OverlappingActors;
			Pawn->GetOverlappingActors(OverlappingActors);
			const TSet<AActor*> Overlapping(OverlappingActors);

			TArray<TPair<double, AActor*>> Candidates;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor == Pawn || Actor->IsA<AController>() || IsSpatiallyMeaningless(Actor))
				{
					continue;
				}
				if (!ClassFilter.IsEmpty()
					&& !Actor->GetClass()->GetName().Contains(ClassFilter))
				{
					continue;
				}
				const double Distance = FVector::Dist(Origin, Actor->GetActorLocation());
				if (Distance <= Radius)
				{
					Candidates.Add(TPair<double, AActor*>(Distance, Actor));
				}
			}
			Candidates.Sort([](const TPair<double, AActor*>& A, const TPair<double, AActor*>& B)
			{
				return A.Key < B.Key;
			});

			TArray<TSharedPtr<FJsonValue>> Nearby;
			for (int32 i = 0; i < Candidates.Num() && i < Max; ++i)
			{
				Nearby.Add(MakeShared<FJsonValueObject>(
					DescribeActor(Candidates[i].Value, Candidates[i].Key, Overlapping)));
			}
			Data->SetArrayField(TEXT("nearby"), Nearby);

			// Say so when the list was cut, so "nothing else is around" is never
			// inferred from a truncated answer.
			Data->SetNumberField(TEXT("nearby_total"), Candidates.Num());
			Data->SetBoolField(TEXT("truncated"), Candidates.Num() > Nearby.Num());
			Data->SetNumberField(TEXT("radius"), Radius);

			// The rest of the situation, when it was asked for.
			//
			// Each of these is a call an agent was making anyway - ui_live,
			// drain_events, viewport_screenshot - and stitching four replies
			// costs four round trips through the game thread, during which the
			// game has moved on, so the four answers describe four different
			// moments. Gathered here they describe one.
			TSet<FString> Include;
			const TArray<TSharedPtr<FJsonValue>>* IncludeArray = nullptr;
			if (Ctx.Params->TryGetArrayField(FStringView(TEXT("include")), IncludeArray))
			{
				for (const TSharedPtr<FJsonValue>& Entry : *IncludeArray)
				{
					Include.Add(Entry->AsString());
				}
			}

			if (Include.Contains(TEXT("ui")))
			{
				TArray<TSharedPtr<FJsonValue>> Widgets;
				TSet<UWidget*> Seen;

				// The same enumeration and the same walk ui_live uses, so the
				// two tools cannot disagree about what is on screen. A second
				// filter here would be a second answer to one question.
				TArray<UUserWidget*> Screens;
				UplinkObservation::GatherLiveWidgets(World, Screens);
				for (UUserWidget* Screen : Screens)
				{
					if (!Screen || !Screen->WidgetTree)
					{
						continue;
					}
					UplinkObservation::ForEachWidgetInScreen(Screen, Seen, [&Widgets](UWidget* Widget, UUserWidget* Owner)
					{
						if (!Widget || Widgets.Num() >= 40)
						{
							return;
						}
						const FVector2D Size = Widget->GetCachedGeometry().GetAbsoluteSize();
						const bool bLaidOut = Size.X > 0.0 && Size.Y > 0.0;
						if (!bLaidOut)
						{
							return; // exists but is not on screen, so it is not part of the situation
						}
						FString Text;
						if (const UTextBlock* AsText = Cast<UTextBlock>(Widget))
						{
							Text = AsText->GetText().ToString();
						}
						TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
						Row->SetStringField(TEXT("name"), Widget->GetName());
						Row->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
						if (!Text.IsEmpty())
						{
							Row->SetStringField(TEXT("text"), Text);
						}
						// Same test ui_live applies: Visible is the only
						// hit-testable state, and geometry is where a click lands.
						Row->SetBoolField(TEXT("interactive"),
							Widget->GetVisibility() == ESlateVisibility::Visible);
						Widgets.Add(MakeShared<FJsonValueObject>(Row));
					});
				}
				Data->SetArrayField(TEXT("ui"), Widgets);
			}

			if (Include.Contains(TEXT("events")))
			{
				// Read-only: the cursor lives with the caller as since_seq, so
				// looking here takes nothing away from a polling loop.
				bool bEventsTruncated = false;
				const TArray<FUplinkEventRecorder::FEvent> Fired =
					Recorder.Drain(static_cast<int64>(GetNumber(Ctx.Params, TEXT("since_seq"), 0)),
						nullptr, 25, &bEventsTruncated);
				TArray<TSharedPtr<FJsonValue>> Rows;
				for (const FUplinkEventRecorder::FEvent& Event : Fired)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetNumberField(TEXT("seq"), static_cast<double>(Event.Seq));
					Row->SetStringField(TEXT("object"), Event.ObjectPath);
					Row->SetStringField(TEXT("delegate"), Event.Delegate);
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}
				Data->SetArrayField(TEXT("events"), Rows);
				Data->SetNumberField(TEXT("next_seq"), static_cast<double>(Recorder.NewestSeq()));
			}

			FUplinkToolResult Out = FUplinkToolResult::Ok(Data, FString::Printf(
				TEXT("%d actor(s) within %.0fcm"), Candidates.Num(), Radius));

			if (Include.Contains(TEXT("screenshot")))
			{
				FIntPoint Size;
				FString Source;
				FString CaptureError;
				TArray64<uint8> Png;
				if (UplinkObservation::CaptureViewportPng(Png, Size, Source, CaptureError))
				{
					Out.Png = MoveTemp(Png);
					TSharedRef<FJsonObject> Screen = MakeShared<FJsonObject>();
					Screen->SetNumberField(TEXT("width"), Size.X);
					Screen->SetNumberField(TEXT("height"), Size.Y);
					Screen->SetStringField(TEXT("source"), Source);
					Data->SetObjectField(TEXT("screen"), Screen);
				}
				else
				{
					// The facts are still worth having, so a failed capture
					// says so in the reply rather than failing the call - but it
					// must say so, or a missing image reads as an empty screen.
					Data->SetStringField(TEXT("screenshot_error"), CaptureError);
				}
			}

			return Out;
		});
}
