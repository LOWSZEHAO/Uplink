// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Observation tools: watch_events, drain_events, unwatch, wait_until,
// get_world_state, perf_stats.

#include "UplinkTools.h"
#include "UplinkEventRecorder.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMemory.h"
#include "JsonObjectConverter.h"
#include "Misc/App.h"

extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

using namespace UplinkToolUtil;

namespace
{
	FUplinkToolResult EventsToResult(const TArray<FUplinkEventRecorder::FEvent>& Events, int64 NextSeq)
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
				TEXT("event_count"), TEXT("elapsed") };
			if (!KnownTypes.Contains(Type))
			{
				Out = FUplinkToolResult::Error(TEXT("condition.type must be property_equals, actor_exists, actor_gone, event_count, or elapsed"));
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

			if (Type == TEXT("actor_exists") || Type == TEXT("actor_gone"))
			{
				AActor* Actor = FindActor(World, GetString(Condition, TEXT("actor")));
				const bool bExists = Actor != nullptr && IsValid(Actor);
				return Type == TEXT("actor_exists") ? bExists : !bExists;
			}

			// property_equals
			FString ResolveError;
			UObject* Object = ResolveObject(Condition, World, ResolveError);
			if (!Object)
			{
				return false; // object may appear later; keep waiting
			}
			FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *GetString(Condition, TEXT("property")));
			if (!Property)
			{
				OutError = FString::Printf(TEXT("property '%s' not found on %s"),
					*GetString(Condition, TEXT("property")), *Object->GetClass()->GetName());
				return false;
			}
			const TSharedPtr<FJsonValue> Expected = Condition->TryGetField(FStringView(TEXT("value")));
			if (!Expected.IsValid())
			{
				OutError = TEXT("property_equals condition needs 'value'");
				return false;
			}
			const TSharedPtr<FJsonValue> Actual = FJsonObjectConverter::UPropertyToJsonValue(
				Property, Property->ContainerPtrToValuePtr<void>(Object));
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
		TEXT(R"json({"type":"object","properties":{"object_path":{"type":"string"},"actor":{"type":"string"},"component":{"type":"string"},"delegate":{"type":"string","description":"Delegate property name, or '*' for all"},"world":{"type":"string","enum":["editor","pie"]}},"required":["delegate"]})json"),
		/*bReadOnly=*/false,
		[&Recorder](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
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
		TEXT("Read captured delegate events (oldest first). Pass the returned next_seq back as since_seq to read only new events."),
		TEXT(R"json({"type":"object","properties":{"since_seq":{"type":"number"},"watch_id":{"type":"string","description":"Only this watch (optional)"},"max":{"type":"number","default":100}}})json"),
		/*bReadOnly=*/true,
		[&Recorder](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FGuid WatchId;
			const bool bFiltered = FGuid::Parse(GetString(Ctx.Params, TEXT("watch_id")), WatchId);
			const TArray<FUplinkEventRecorder::FEvent> Events = Recorder.Drain(
				static_cast<int64>(GetNumber(Ctx.Params, TEXT("since_seq"), 0)),
				bFiltered ? &WatchId : nullptr,
				FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 100)), 1, 500));
			return EventsToResult(Events, Recorder.NewestSeq());
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
		Info.Description = TEXT("Wait (without blocking the editor) until a condition becomes true, or the timeout passes - the assertion primitive for automated playtests. Condition types: property_equals {actor/object_path, property, value, tolerance?}, actor_exists {actor}, actor_gone {actor}, event_count {watch_id, at_least, since_seq?}, elapsed {seconds}. A timeout is reported as condition_met=false, not as an error.");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"condition":{"type":"object","description":"{type, ...} - see tool description"},"timeout":{"type":"number","default":30,"description":"seconds (0.1-300)"},"world":{"type":"string","enum":["editor","pie"]}},"required":["condition"]})json"));
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
		TEXT(R"json({"type":"object","properties":{"world":{"type":"string","enum":["editor","pie"]},"class_contains":{"type":"string"},"name_contains":{"type":"string"},"properties":{"type":"array","items":{"type":"string"},"description":"Property names to include per actor (when present on its class)"},"max":{"type":"number","default":50}}})json"),
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
		TEXT("Frame timing and memory: smoothed FPS, average frame ms, last delta seconds, used physical memory."),
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
			return FUplinkToolResult::Ok(Data);
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
				Out = FUplinkToolResult::Ok(Data);
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
