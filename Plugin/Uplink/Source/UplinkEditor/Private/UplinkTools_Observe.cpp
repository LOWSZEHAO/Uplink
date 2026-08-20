// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Observation tools: watch_events, drain_events, unwatch, wait_until,
// get_world_state, perf_stats, profile_capture.

#include "UplinkTools.h"
#include "Object/UplinkObjectPath.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "NavigationData.h"
#include "GameFramework/PlayerController.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "UObject/UObjectIterator.h"
#include "UplinkEventRecorder.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "DynamicRHI.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMemory.h"
#include "JsonObjectConverter.h"
#include "Misc/App.h"
#include "RHIStats.h"

extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

using namespace UplinkToolUtil;

namespace UplinkPerf
{
	/**
	 * Graphics memory, and how close it is to the budget.
	 *
	 * An agent driving a heavy project unattended has no other way to see
	 * memory pressure building, and the frame times it already reads are the
	 * natural place to notice it. Reported for the device rather than the
	 * process: this is a budget every application on the machine shares.
	 *
	 * Not a crash predictor. It was added while chasing D3D12 page faults on a
	 * 9 GB project that turned out to be compute-shader MMU faults - memory sat
	 * near budget in those dumps because UE prints these stats with every fault
	 * report, not because it caused them. Useful, and not evidence.
	 */
	void AddGraphicsMemory(const TSharedRef<FJsonObject>& Data)
	{
		FTextureMemoryStats Stats;
		RHIGetTextureMemoryStats(Stats);

		const int64 Budget = Stats.GetTotalDeviceWorkingMemory();
		if (Budget <= 0)
		{
			return; // the RHI reports -1 for anything it does not know
		}
		const int64 Used = static_cast<int64>(Stats.StreamingMemorySize + Stats.NonStreamingMemorySize);
		Data->SetNumberField(TEXT("gpu_budget_mb"), Budget / (1024.0 * 1024.0));
		Data->SetNumberField(TEXT("gpu_textures_mb"), Used / (1024.0 * 1024.0));
	}

	/**
	 * Why a frame-time reading cannot be trusted, or empty when it can.
	 *
	 * The editor caps itself hard when its window is not in front - a real
	 * measurement came back at exactly 333.33ms a frame, which is 3 FPS to the
	 * decimal and obviously a cap rather than a game. That matters more here
	 * than in normal editor use: an agent drives the editor through HTTP and
	 * never brings the window forward, so every number it takes is the
	 * throttle's, not the project's, and nothing said so.
	 */
	FString DescribeThrottle()
	{
		const UEditorPerformanceSettings* Settings = GetDefault<UEditorPerformanceSettings>();
		const bool bThrottleEnabled = Settings && Settings->bThrottleCPUWhenNotForeground;
		const bool bForeground = FSlateApplication::IsInitialized()
			&& FSlateApplication::Get().IsActive();
		if (!bThrottleEnabled || bForeground)
		{
			return FString();
		}
		return TEXT("the editor window is in the background and 'Use Less CPU when in Background' is on, ")
			TEXT("so these frame times are the editor's throttle rather than the project's - a capture here reads about 3 fps whatever the project does. ")
			TEXT("Bringing the window to the front fixes it. Setting bThrottleCPUWhenNotForeground false on ")
			TEXT("/Script/UnrealEd.Default__EditorPerformanceSettings also works and needs no clicking, but weigh it first: ")
			TEXT("that throttle is the only thing keeping an unattended editor from rendering a heavy project flat out, ")
			TEXT("and doing it on a 9 GB project during play took the GPU down with a D3D device removal. ")
			TEXT("It is a runtime-only change either way - a restart puts it back.");
	}
}

namespace
{
	FUplinkToolResult EventsToResult(const TArray<FUplinkEventRecorder::FEvent>& Events, int64 NextSeq,
		bool bTruncated = false)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FUplinkEventRecorder::FEvent& Event : Events)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("seq"), static_cast<double>(Event.Seq));
			Row->SetStringField(TEXT("watch_id"), Event.WatchId.ToString(EGuidFormats::DigitsWithHyphens));
			Row->SetStringField(TEXT("object"), Event.ObjectPath);
			Row->SetStringField(TEXT("delegate"), Event.Delegate);
			if (Event.Payload.IsValid())
			{
				Row->SetObjectField(TEXT("payload"), Event.Payload);
			}
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("events"), Rows);
		Data->SetNumberField(TEXT("next_seq"), static_cast<double>(NextSeq));
		Data->SetBoolField(TEXT("truncated"), bTruncated);
		return FUplinkToolResult::Ok(Data);
	}

	// wait_until: evaluated once per editor tick; hitting the caller's timeout
	// is a VALID result (condition_met=false), not a task failure - agents use
	// this as an assertion primitive.
	class FWaitUntilInvocation final : public IUplinkInvocation
	{
	public:
		explicit FWaitUntilInvocation(FUplinkEventRecorder& InRecorder) : Recorder(InRecorder) {}

		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			const TSharedPtr<FJsonObject>* ConditionPtr = nullptr;
			if (!Ctx.Params->TryGetObjectField(FStringView(TEXT("condition")), ConditionPtr) || !ConditionPtr->IsValid())
			{
				Out = FUplinkToolResult::Error(TEXT("'condition' object is required"));
				return EUplinkToolStep::Done;
			}
			Condition = *ConditionPtr;
			Type = GetString(Condition, TEXT("type"));

			static const TSet<FString> KnownTypes = {
				TEXT("property_equals"), TEXT("actor_exists"), TEXT("actor_gone"),
				TEXT("event_count"), TEXT("elapsed"), TEXT("ui_visible"), TEXT("navmesh_ready") };
			if (!KnownTypes.Contains(Type))
			{
				Out = FUplinkToolResult::Error(TEXT("condition.type must be property_equals, actor_exists, actor_gone, event_count, elapsed, ui_visible, or navmesh_ready"));
				return EUplinkToolStep::Done;
			}
			if (Type == TEXT("event_count") &&
				!FGuid::Parse(GetString(Condition, TEXT("watch_id")), EventWatchId))
			{
				Out = FUplinkToolResult::Error(TEXT("event_count condition needs a valid 'watch_id'"));
				return EUplinkToolStep::Done;
			}

			StartedAt = FPlatformTime::Seconds();
			Deadline = StartedAt + FMath::Clamp(GetNumber(Ctx.Params, TEXT("timeout"), 30.0), 0.1, 300.0);
			return Evaluate(Ctx, Out);
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			return Evaluate(Ctx, Out);
		}

	private:
		EUplinkToolStep Evaluate(const FUplinkToolContext& Ctx, FUplinkToolResult& Out)
		{
			FString EvalError;
			const bool bMet = EvaluateCondition(Ctx, EvalError);
			if (!EvalError.IsEmpty())
			{
				Out = FUplinkToolResult::Error(EvalError);
				return EUplinkToolStep::Done;
			}

			const double Now = FPlatformTime::Seconds();
			if (bMet || Now >= Deadline)
			{
				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetBoolField(TEXT("condition_met"), bMet);
				Data->SetBoolField(TEXT("timed_out"), !bMet);
				Data->SetNumberField(TEXT("waited_seconds"), Now - StartedAt);
				Out = FUplinkToolResult::Ok(Data,
					bMet ? TEXT("condition met") : TEXT("timeout reached without the condition becoming true"));
				return EUplinkToolStep::Done;
			}
			return EUplinkToolStep::Pending;
		}

		bool EvaluateCondition(const FUplinkToolContext& Ctx, FString& OutError)
		{
			if (Type == TEXT("elapsed"))
			{
				return FPlatformTime::Seconds() - StartedAt >= GetNumber(Condition, TEXT("seconds"), 1.0);
			}

			if (Type == TEXT("event_count"))
			{
				const int32 AtLeast = FMath::Max(1, static_cast<int32>(GetNumber(Condition, TEXT("at_least"), 1)));
				return Recorder.EventCountForWatch(EventWatchId,
					static_cast<int64>(GetNumber(Condition, TEXT("since_seq"), 0))) >= AtLeast;
			}

			// Remaining conditions need a world; while it's unavailable
			// (e.g. mid PIE transition) just keep waiting.
			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return false;
			}

			// Navmesh tiles are built across frames, so the mesh is not there
			// when BeginPlay returns. Pathing into that window fails with a
			// message about the goal being off the navmesh - which reads as a
			// level fault and is not one - and the usual workaround is to sleep
			// a guessed number of seconds, which is a fudge factor that has to
			// be retuned per level and per machine.
			if (Type == TEXT("navmesh_ready"))
			{
				UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
				if (!Nav)
				{
					OutError = TEXT("this world has no navigation system, so no navmesh will ever be ready - check the level has a nav mesh bounds volume");
					return false;
				}
				const APlayerController* PC = World->GetFirstPlayerController();
				APawn* Pawn = PC ? PC->GetPawn() : nullptr;
				if (!Pawn)
				{
					// No pawn yet is a reason to keep waiting, not an error:
					// this is usually the first thing after pie_start.
					return false;
				}

				// Ask the question the caller actually has - can a path be
				// built - rather than a proxy for it. Two proxies were tried
				// and both answered yes while pathing still failed:
				//
				//   GetNumRemainingBuildTasks() == 0 is also true BEFORE
				//   generation starts, so it reported ready 400ms after
				//   BeginPlay with no mesh in the level at all.
				//
				//   ProjectPointToNavigation succeeded at 121ms because its
				//   query extent finds mesh NEAR the point rather than AT it,
				//   and a goal 200 units from the nearest tile still projects.
				//
				// Building the path costs more than either, and it is the only
				// answer that cannot be true while navigate_to fails.
				FVector Goal;
				if (!TryGetVector(Condition, TEXT("location"), Goal))
				{
					OutError = TEXT("navmesh_ready needs 'location' - the point you intend to path to. The pawn stands on mesh long before the far side of a level has any, so a test that omits the destination is the misleading half of the failure this exists to prevent.");
					return false;
				}

				UNavigationPath* Path = Nav->FindPathToLocationSynchronously(
					World, Pawn->GetActorLocation(), Goal, Pawn);
				return Path != nullptr && Path->IsValid() && !Path->IsPartial();
			}

			// A menu is built a frame or two after the screen that owns it is
			// added, and a key sent into that gap goes to the viewport instead.
			// Measured on a shipping title: the same call answered handled and
			// did nothing, twice, because nothing had focus yet. Waiting on a
			// widget rather than on a stopwatch is the fix.
			if (Type == TEXT("ui_visible"))
			{
				const FString Needle = GetString(Condition, TEXT("contains"));
				if (Needle.IsEmpty())
				{
					OutError = TEXT("ui_visible condition needs 'contains' - part of a widget name or of the text it displays");
					return false;
				}
				bool bFound = false;
				for (TObjectIterator<UUserWidget> It; It; ++It)
				{
					UUserWidget* Screen = *It;
					if (!Screen || Screen->GetWorld() != World || !Screen->IsInViewport())
					{
						continue;
					}
					if (Screen->GetName().Contains(Needle, ESearchCase::IgnoreCase))
					{
						bFound = true;
						break;
					}
					if (UWidgetTree* Tree = Screen->WidgetTree)
					{
						Tree->ForEachWidget([&bFound, &Needle](UWidget* Widget)
						{
							if (bFound || !Widget)
							{
								return;
							}
							if (Widget->GetName().Contains(Needle, ESearchCase::IgnoreCase))
							{
								bFound = true;
								return;
							}
							if (const UTextBlock* Text = Cast<UTextBlock>(Widget))
							{
								if (Text->GetText().ToString().Contains(Needle, ESearchCase::IgnoreCase))
								{
									bFound = true;
								}
							}
						});
					}
					if (bFound)
					{
						break;
					}
				}
				return bFound;
			}

			if (Type == TEXT("actor_exists") || Type == TEXT("actor_gone"))
			{
				const FString ActorName = GetString(Condition, TEXT("actor"));
				if (ActorName.IsEmpty())
				{
					// FindActor falls back to a substring match, and every
					// string contains the empty string - so an omitted 'actor'
					// matched the first actor in the world and answered
					// "condition met" instantly, naming nothing.
					OutError = FString::Printf(
						TEXT("%s condition needs 'actor' - the name or label to look for"), *Type);
					return false;
				}
				AActor* Actor = FindActor(World, ActorName);
				const bool bExists = Actor != nullptr && IsValid(Actor);
				return Type == TEXT("actor_exists") ? bExists : !bExists;
			}

			// property_equals
			if (!Condition->HasField(TEXT("actor")) && !Condition->HasField(TEXT("object_path")))
			{
				// An object that has not spawned yet is worth waiting for, which
				// is why an unresolved target below is not an error. A condition
				// naming no target at all is a different thing: it can never
				// become true, so waiting the full timeout only delays saying so.
				OutError = TEXT("property_equals condition needs 'actor' or 'object_path' - the object to read the property from");
				return false;
			}
			FString ResolveError;
			UObject* Object = ResolveObject(Condition, World, ResolveError);
			if (!Object)
			{
				return false; // object may appear later; keep waiting
			}
			void* ValueAddr = nullptr;
			FString PathError;
			FProperty* Property = UplinkObject::ResolvePropertyPath(
				Object, GetString(Condition, TEXT("property")), ValueAddr, PathError);
			if (!Property || !ValueAddr)
			{
				OutError = PathError.IsEmpty()
					? FString::Printf(TEXT("property '%s' not found on %s"),
						*GetString(Condition, TEXT("property")), *Object->GetClass()->GetName())
					: PathError;
				return false;
			}
			const TSharedPtr<FJsonValue> Expected = Condition->TryGetField(FStringView(TEXT("value")));
			if (!Expected.IsValid())
			{
				OutError = TEXT("property_equals condition needs 'value'");
				return false;
			}
			const TSharedPtr<FJsonValue> Actual = FJsonObjectConverter::UPropertyToJsonValue(Property, ValueAddr);
			if (!Actual.IsValid())
			{
				return false;
			}

			double ExpectedNumber = 0.0, ActualNumber = 0.0;
			if (Expected->TryGetNumber(ExpectedNumber) && Actual->TryGetNumber(ActualNumber))
			{
				const double Tolerance = GetNumber(Condition, TEXT("tolerance"), 0.0001);
				return FMath::Abs(ExpectedNumber - ActualNumber) <= Tolerance;
			}
			return FJsonValue::CompareEqual(*Expected, *Actual);
		}

		FUplinkEventRecorder& Recorder;
		TSharedPtr<FJsonObject> Condition;
		FString Type;
		FGuid EventWatchId;
		double StartedAt = 0.0;
		double Deadline = 0.0;
	};
}

void UplinkTools::RegisterObserve(FUplinkToolRegistry& Registry, FUplinkEventRecorder& Recorder)
{
	Registry.RegisterQuick(
		TEXT("watch_events"),
		TEXT("Start recording broadcasts of a dynamic multicast delegate (any BlueprintAssignable event like OnGrabbed, OnValueChanged) with their parameter payloads. delegate '*' watches every delegate on the object. Read captures with drain_events. Watches stop automatically when PIE ends."),
		TEXT(R"json({"type":"object","properties":{"object_path":{"type":"string"},"actor":{"type":"string"},"component":{"type":"string"},"delegate":{"type":"string","description":"Delegate property name, or '*' for all"},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["delegate"]})json"),
		/*bReadOnly=*/false,
		[&Recorder](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			if (!Error.IsEmpty())
			{
				// ResolveWorld names the world ids that do exist. ResolveObject
				// overwrites Error with a generic line, so an explicitly named
				// world that does not resolve has to be reported here.
				return FUplinkToolResult::Error(Error);
			}
			UObject* Object = ResolveObject(Ctx.Params, World, Error);
			if (!Object)
			{
				return FUplinkToolResult::Error(Error);
			}

			TArray<FString> Bound;
			const FGuid WatchId = Recorder.StartWatch(Object, GetString(Ctx.Params, TEXT("delegate")), Bound, Error);
			if (!WatchId.IsValid())
			{
				return FUplinkToolResult::Error(Error);
			}

			TArray<TSharedPtr<FJsonValue>> BoundJson;
			for (const FString& Name : Bound)
			{
				BoundJson.Add(MakeShared<FJsonValueString>(Name));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("watch_id"), WatchId.ToString(EGuidFormats::DigitsWithHyphens));
			Data->SetArrayField(TEXT("bound"), BoundJson);
			Data->SetNumberField(TEXT("next_seq"), static_cast<double>(Recorder.NewestSeq()));
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("drain_events"),
		TEXT("Read captured delegate events (oldest first). Pass the returned next_seq back as since_seq to read only new events. A read capped by 'max' returns the oldest of what was waiting, sets truncated:true, and leaves next_seq just after the last event handed over - so polling picks the remainder up rather than stepping over it."),
		TEXT(R"json({"type":"object","properties":{"since_seq":{"type":"number"},"watch_id":{"type":"string","description":"Only this watch (optional)"},"max":{"type":"number","default":100}}})json"),
		/*bReadOnly=*/true,
		[&Recorder](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FGuid WatchId;
			const bool bFiltered = FGuid::Parse(GetString(Ctx.Params, TEXT("watch_id")), WatchId);
			bool bTruncated = false;
			const TArray<FUplinkEventRecorder::FEvent> Events = Recorder.Drain(
				static_cast<int64>(GetNumber(Ctx.Params, TEXT("since_seq"), 0)),
				bFiltered ? &WatchId : nullptr,
				FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 100)), 1, 500),
				&bTruncated);

			// Resume from just after what was actually handed over, so a capped
			// read leaves the remainder to the next call rather than skipping it.
			const int64 NextSeq = (bTruncated && Events.Num() > 0)
				? Events.Last().Seq + 1
				: Recorder.NewestSeq();
			return EventsToResult(Events, NextSeq, bTruncated);
		});

	Registry.RegisterQuick(
		TEXT("unwatch"),
		TEXT("Stop one event watch (watch_id) or all of them (all=true)."),
		TEXT(R"json({"type":"object","properties":{"watch_id":{"type":"string"},"all":{"type":"boolean"}}})json"),
		/*bReadOnly=*/false,
		[&Recorder](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			bool bAll = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("all")), bAll);
			if (bAll)
			{
				Recorder.StopAllWatches();
				return FUplinkToolResult::Ok(nullptr, TEXT("all watches stopped"));
			}
			FGuid WatchId;
			if (!FGuid::Parse(GetString(Ctx.Params, TEXT("watch_id")), WatchId) || !Recorder.StopWatch(WatchId))
			{
				return FUplinkToolResult::Error(TEXT("watch not found (pass watch_id, or all=true)"));
			}
			return FUplinkToolResult::Ok(nullptr, TEXT("watch stopped"));
		});

	{
		FUplinkToolInfo Info;
		Info.Name = TEXT("wait_until");
		Info.Description = TEXT("Wait (without blocking the editor) until a condition becomes true, or the timeout passes - the assertion primitive for automated playtests. Condition types: property_equals {actor/object_path, property, value, tolerance?} - 'property' takes the same dotted paths get_property does, so nested struct members and values behind an object reference are assertable, actor_exists {actor}, actor_gone {actor}, event_count {watch_id, at_least, since_seq?}, elapsed {seconds}, ui_visible {contains}, navmesh_ready {location?}. navmesh_ready {location} waits until a complete path can actually be built from the player pawn to that location, which is the question navigate_to is about to ask. 'location' is required. It deliberately avoids the two cheaper proxies, both of which answer yes while pathing still fails: the engine's remaining-build-task count reads zero before generation has even started, and projecting a point onto the mesh succeeds when there is mesh merely NEAR it. Navmesh tiles build across frames, so pathing issued straight after BeginPlay fails with a message about the goal being off the navmesh - which reads as a level fault and is not one - and the usual workaround is sleeping a guessed number of seconds that has to be retuned per level and per machine. Navmesh tiles are built across frames, so the mesh is absent for a while after BeginPlay and pathing into that window fails with a message about the goal being off the navmesh, which reads as a level fault and is not one; the usual workaround is sleeping a guessed number of seconds, which has to be retuned per level and per machine. ui_visible waits for a UMG widget that is actually on screen, matching part of a widget name or of the text it displays - a menu is constructed a frame or two after the screen holding it is added, and a key sent into that gap reaches the viewport instead of the menu and still answers handled. A timeout is reported as condition_met=false, not as an error.");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"condition":{"type":"object","description":"{type, ...} - see tool description"},"timeout":{"type":"number","default":30,"description":"seconds (0.1-300)"},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["condition"]})json"));
		Info.bReadOnly = true;
		Info.TimeoutSeconds = 310.0;
		Registry.Register(MoveTemp(Info), [&Recorder]() -> TSharedRef<IUplinkInvocation>
		{
			return MakeShared<FWaitUntilInvocation>(Recorder);
		});
	}

	Registry.RegisterQuick(
		TEXT("get_world_state"),
		TEXT("Compact snapshot of actors in the current world: name, class, location, plus any requested property values - a delta-friendly alternative to screenshots for checking game state."),
		TEXT(R"json({"type":"object","properties":{"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"},"class_contains":{"type":"string"},"name_contains":{"type":"string"},"properties":{"type":"array","items":{"type":"string"},"description":"Property names to include per actor (when present on its class)"},"max":{"type":"number","default":50}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			if (!World)
			{
				return FUplinkToolResult::Error(Error);
			}

			TArray<FString> PropertyNames;
			const TArray<TSharedPtr<FJsonValue>>* PropertyArray = nullptr;
			if (Ctx.Params->TryGetArrayField(FStringView(TEXT("properties")), PropertyArray))
			{
				for (const TSharedPtr<FJsonValue>& Value : *PropertyArray)
				{
					PropertyNames.Add(Value->AsString());
				}
			}

			const FString ClassFilter = GetString(Ctx.Params, TEXT("class_contains"));
			const FString NameFilter = GetString(Ctx.Params, TEXT("name_contains"));
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 50)), 1, 500);

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Total = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				const FString ClassName = Actor->GetClass()->GetPathName();
				if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter))
				{
					continue;
				}
				if (!NameFilter.IsEmpty() && !Actor->GetName().Contains(NameFilter))
				{
					continue;
				}

				// Counted before the cap, so a capped reply can say so. A world
				// summary that silently stops at 50 actors reads as "there are
				// 50 actors", which is the wrong conclusion to hand an agent.
				++Total;
				if (Rows.Num() >= Max)
				{
					continue;
				}

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Actor->GetName());
				Row->SetStringField(TEXT("class"), ClassName);
				Row->SetObjectField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
				for (const FString& PropertyName : PropertyNames)
				{
					if (FProperty* Property = FindFProperty<FProperty>(Actor->GetClass(), *PropertyName))
					{
						Row->SetField(PropertyName, FJsonObjectConverter::UPropertyToJsonValue(
							Property, Property->ContainerPtrToValuePtr<void>(Actor)));
					}
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("actors"), Rows);
			Data->SetNumberField(TEXT("total_matching"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Rows.Num());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("perf_stats"),
		TEXT("Frame timing and memory: smoothed FPS, average frame ms, last delta seconds, used physical memory, and graphics memory as gpu_textures_mb against gpu_budget_mb. Reports 'throttled' when the editor is capping the frame rate because its window is in the background - the numbers mean nothing then, and an agent never has the window in front. Read gpu_budget_mb as the card's dedicated memory rather than a promise - the driver enforces a smaller runtime budget under pressure, and on one machine this reported 7948 MB where the driver allowed 6995 MB."),
		TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("average_fps"), GAverageFPS);
			Data->SetNumberField(TEXT("average_frame_ms"), GAverageMS);
			Data->SetNumberField(TEXT("last_delta_seconds"), FApp::GetDeltaTime());
			Data->SetNumberField(TEXT("used_physical_mb"), MemoryStats.UsedPhysical / (1024.0 * 1024.0));
			UplinkPerf::AddGraphicsMemory(Data);
			const FString Throttle = UplinkPerf::DescribeThrottle();
			Data->SetBoolField(TEXT("throttled"), !Throttle.IsEmpty());
			return FUplinkToolResult::Ok(Data, Throttle);
		});

	{
		// profile_capture: sample frame times for N seconds and report the
		// distribution plus hitch counts - the frame-budget truth for a scene
		// or scenario, not a single-moment FPS read.
		class FProfileCaptureInvocation final : public IUplinkInvocation
		{
		public:
			virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
			{
				DurationSeconds = FMath::Clamp(GetNumber(Ctx.Params, TEXT("seconds"), 5.0), 1.0, 120.0);
				StartedAt = FPlatformTime::Seconds();
				return EUplinkToolStep::Pending;
			}

			virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
			{
				SamplesMs.Add(FApp::GetDeltaTime() * 1000.0);
				if (FPlatformTime::Seconds() - StartedAt < DurationSeconds)
				{
					return EUplinkToolStep::Pending;
				}

				SamplesMs.Sort();
				double Total = 0.0;
				int32 HitchesOver33 = 0;
				int32 HitchesOver100 = 0;
				for (double Sample : SamplesMs)
				{
					Total += Sample;
					HitchesOver33 += Sample > 33.4 ? 1 : 0;
					HitchesOver100 += Sample > 100.0 ? 1 : 0;
				}
				const double AverageMs = SamplesMs.Num() ? Total / SamplesMs.Num() : 0.0;
				auto Percentile = [this](double P) -> double
				{
					if (SamplesMs.Num() == 0) { return 0.0; }
					const int32 Index = FMath::Clamp(
						static_cast<int32>(P * (SamplesMs.Num() - 1)), 0, SamplesMs.Num() - 1);
					return SamplesMs[Index];
				};

				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetNumberField(TEXT("frames"), SamplesMs.Num());
				Data->SetNumberField(TEXT("seconds"), FPlatformTime::Seconds() - StartedAt);
				Data->SetNumberField(TEXT("avg_fps"), AverageMs > 0.0 ? 1000.0 / AverageMs : 0.0);
				Data->SetNumberField(TEXT("avg_ms"), AverageMs);
				Data->SetNumberField(TEXT("p50_ms"), Percentile(0.50));
				Data->SetNumberField(TEXT("p95_ms"), Percentile(0.95));
				Data->SetNumberField(TEXT("p99_ms"), Percentile(0.99));
				Data->SetNumberField(TEXT("worst_ms"), SamplesMs.Num() ? SamplesMs.Last() : 0.0);
				Data->SetNumberField(TEXT("hitches_over_33ms"), HitchesOver33);
				Data->SetNumberField(TEXT("hitches_over_100ms"), HitchesOver100);
				Data->SetBoolField(TEXT("pie_active"), GEditor && GEditor->PlayWorld != nullptr);
				const FString Throttle = UplinkPerf::DescribeThrottle();
				Data->SetBoolField(TEXT("throttled"), !Throttle.IsEmpty());
				Out = FUplinkToolResult::Ok(Data, Throttle);
				return EUplinkToolStep::Done;
			}

		private:
			TArray<double> SamplesMs;
			double DurationSeconds = 5.0;
			double StartedAt = 0.0;
		};

		FUplinkToolInfo Info;
		Info.Name = TEXT("profile_capture");
		Info.Description = TEXT("Sample frame times for 'seconds' and report the distribution: average FPS/ms, p50/p95/p99, worst frame, and hitch counts (>33ms, >100ms). Run it while a scenario or PIE session plays to judge the frame budget properly - perf_stats is a single-moment read, this is the truth over time.");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"seconds":{"type":"number","default":5}}})json"));
		Info.bReadOnly = true;
		Info.TimeoutSeconds = 180.0;
		Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
		{
			return MakeShared<FProfileCaptureInvocation>();
		});
	}
}
