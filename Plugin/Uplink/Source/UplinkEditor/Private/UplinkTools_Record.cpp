// Copyright (c) 2026 Low Sze Hao. MIT License.
// input_record / input_replay - capture a real play session's input and play
// it back through the engine's simulated-input path as a regression test.

#include "UplinkInputRecorder.h"
#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Misc/App.h"

using namespace UplinkToolUtil;

namespace
{
	TSharedRef<FJsonObject> RecordedInputToJson(const FUplinkRecordedInput& Event)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("t"), Event.T);
		Row->SetStringField(TEXT("type"), Event.Type);
		if (!Event.Key.IsEmpty())
		{
			Row->SetStringField(TEXT("key"), Event.Key);
		}
		if (Event.Type == TEXT("axis"))
		{
			Row->SetNumberField(TEXT("amount"), Event.Amount);
		}
		if (Event.Type == TEXT("mouse_move"))
		{
			Row->SetNumberField(TEXT("x"), Event.X);
			Row->SetNumberField(TEXT("y"), Event.Y);
		}
		return Row;
	}

	bool JsonToRecordedInput(const TSharedPtr<FJsonObject>& Row, FUplinkRecordedInput& Out)
	{
		if (!Row.IsValid())
		{
			return false;
		}
		Out.T = GetNumber(Row, TEXT("t"), 0.0);
		Out.Type = GetString(Row, TEXT("type"));
		Out.Key = GetString(Row, TEXT("key"));
		Out.Amount = static_cast<float>(GetNumber(Row, TEXT("amount"), 0.0));
		Out.X = static_cast<float>(GetNumber(Row, TEXT("x"), 0.0));
		Out.Y = static_cast<float>(GetNumber(Row, TEXT("y"), 0.0));
		return !Out.Type.IsEmpty();
	}

	APlayerController* GetPiePlayerController(FString& OutError)
	{
		if (!GEditor || !GEditor->PlayWorld)
		{
			OutError = TEXT("no PIE session is running");
			return nullptr;
		}
		APlayerController* PC = GEditor->PlayWorld->GetFirstPlayerController();
		if (!PC)
		{
			OutError = TEXT("PIE has no player controller");
		}
		return PC;
	}

	void InjectRecordedInput(APlayerController* PC, const FUplinkRecordedInput& Event)
	{
		if (Event.Type == TEXT("mouse_move"))
		{
			if (!FMath::IsNearlyZero(Event.X))
			{
				FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(EKeys::MouseX, IE_Axis, Event.X);
				Args.DeltaTime = FApp::GetDeltaTime();
				PC->InputKey(Args);
			}
			if (!FMath::IsNearlyZero(Event.Y))
			{
				FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(EKeys::MouseY, IE_Axis, -Event.Y);
				Args.DeltaTime = FApp::GetDeltaTime();
				PC->InputKey(Args);
			}
			return;
		}

		const FKey Key(FName(*Event.Key));
		if (!EKeys::GetKeyDetails(Key).IsValid())
		{
			return;
		}
		EInputEvent InputEvent = IE_Pressed;
		float Amount = 1.0f;
		if (Event.Type == TEXT("key_up"))
		{
			InputEvent = IE_Released;
			Amount = 0.0f;
		}
		else if (Event.Type == TEXT("axis"))
		{
			InputEvent = IE_Axis;
			Amount = Event.Amount;
		}
		FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(Key, InputEvent, Amount);
		Args.DeltaTime = FApp::GetDeltaTime();
		PC->InputKey(Args);
	}

	/** Latent playback: fires each event once the wall clock passes its timestamp. */
	class FReplayInvocation : public IUplinkInvocation
	{
	public:
		explicit FReplayInvocation(FUplinkInputRecorder& InRecorder) : Recorder(InRecorder) {}

		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			FString Error;
			if (!GetPiePlayerController(Error))
			{
				Out = FUplinkToolResult::Error(Error);
				return EUplinkToolStep::Done;
			}

			const TArray<TSharedPtr<FJsonValue>>* EventArray = nullptr;
			if (Ctx.Params->TryGetArrayField(FStringView(TEXT("events")), EventArray))
			{
				for (const TSharedPtr<FJsonValue>& Entry : *EventArray)
				{
					const TSharedPtr<FJsonObject>* Row = nullptr;
					FUplinkRecordedInput Event;
					if (Entry.IsValid() && Entry->TryGetObject(Row) && JsonToRecordedInput(*Row, Event))
					{
						Events.Add(Event);
					}
				}
			}
			else
			{
				Events = Recorder.LastRecording();
			}
			if (Events.Num() == 0)
			{
				Out = FUplinkToolResult::Error(TEXT("nothing to replay - record with input_record first, or pass 'events'"));
				return EUplinkToolStep::Done;
			}

			Events.Sort([](const FUplinkRecordedInput& A, const FUplinkRecordedInput& B) { return A.T < B.T; });
			Speed = FMath::Clamp(GetNumber(Ctx.Params, TEXT("speed"), 1.0), 0.1, 10.0);
			StartTime = FPlatformTime::Seconds();
			return EUplinkToolStep::Pending;
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			FString Error;
			APlayerController* PC = GetPiePlayerController(Error);
			if (!PC)
			{
				Out = FUplinkToolResult::Error(FString::Printf(TEXT("replay aborted after %d events: %s"), NextIndex, *Error));
				return EUplinkToolStep::Done;
			}

			const double Elapsed = (FPlatformTime::Seconds() - StartTime) * Speed;
			while (NextIndex < Events.Num() && Events[NextIndex].T <= Elapsed)
			{
				InjectRecordedInput(PC, Events[NextIndex]);
				++NextIndex;
			}
			if (NextIndex < Events.Num())
			{
				return EUplinkToolStep::Pending;
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("replayed"), Events.Num());
			Data->SetNumberField(TEXT("seconds"), FPlatformTime::Seconds() - StartTime);
			Out = FUplinkToolResult::Ok(Data, TEXT("replay complete"));
			return EUplinkToolStep::Done;
		}

	private:
		FUplinkInputRecorder& Recorder;
		TArray<FUplinkRecordedInput> Events;
		double StartTime = 0.0;
		double Speed = 1.0;
		int32 NextIndex = 0;
	};
}

void UplinkTools::RegisterRecord(FUplinkToolRegistry& Registry, FUplinkInputRecorder& Recorder)
{
	Registry.RegisterQuick(
		TEXT("input_record"),
		TEXT("Record the human's real input (keys, mouse buttons, axes, mouse moves) via a passive Slate tap. action: 'start' | 'stop' (returns the timestamped events - keep them to replay later) | 'status'. A recording auto-stops when PIE ends and is kept as the last take."),
		TEXT(R"json({"type":"object","properties":{"action":{"type":"string","enum":["start","stop","status"]}},"required":["action"]})json"),
		/*bReadOnly=*/false,
		[&Recorder](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Action = GetString(Ctx.Params, TEXT("action"));
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

			if (Action == TEXT("start"))
			{
				FString Error;
				if (!Recorder.StartRecording(Error))
				{
					return FUplinkToolResult::Error(Error);
				}
				return FUplinkToolResult::Ok(nullptr, TEXT("recording - play the game, then input_record stop"));
			}
			if (Action == TEXT("stop"))
			{
				if (!Recorder.IsRecording())
				{
					return FUplinkToolResult::Error(TEXT("not recording"));
				}
				const TArray<FUplinkRecordedInput>& Events = Recorder.StopRecording();
				TArray<TSharedPtr<FJsonValue>> Rows;
				for (const FUplinkRecordedInput& Event : Events)
				{
					Rows.Add(MakeShared<FJsonValueObject>(RecordedInputToJson(Event)));
				}
				Data->SetArrayField(TEXT("events"), Rows);
				Data->SetNumberField(TEXT("count"), Events.Num());
				Data->SetNumberField(TEXT("seconds"), Recorder.RecordingSeconds());
				return FUplinkToolResult::Ok(Data);
			}
			if (Action == TEXT("status"))
			{
				Data->SetBoolField(TEXT("recording"), Recorder.IsRecording());
				Data->SetNumberField(TEXT("count"), Recorder.NumEvents());
				Data->SetNumberField(TEXT("seconds"), Recorder.RecordingSeconds());
				return FUplinkToolResult::Ok(Data);
			}
			return FUplinkToolResult::Error(TEXT("action must be start, stop, or status"));
		});

	FUplinkToolInfo Info;
	Info.Name = TEXT("input_replay");
	Info.Description = TEXT("Replay recorded input into the running game through the engine's simulated-input path - a regression test from a real play session. Uses the last input_record take, or pass 'events' from an earlier stop. 'speed' scales playback rate.");
	Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"events":{"type":"array","items":{"type":"object"},"description":"Events from input_record stop (omit to use the last take)"},"speed":{"type":"number","default":1.0}}})json"));
	Info.bReadOnly = false;
	Info.TimeoutSeconds = 600.0;
	Registry.Register(MoveTemp(Info), [&Recorder]() -> TSharedRef<IUplinkInvocation>
	{
		return MakeShared<FReplayInvocation>(Recorder);
	});
}
