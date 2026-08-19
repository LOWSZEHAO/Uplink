// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// input_record / input_replay - capture a real play session's input and play
// it back through the engine's simulated-input path.

#include "UplinkInputRecorder.h"
#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Misc/App.h"

using namespace UplinkToolUtil;

namespace
{
	/**
	 * The deadline the tool publishes and the one Start() measures a take
	 * against, from one constant: a take longer than this is killed halfway by
	 * the task manager, and half a replay leaves the game holding keys.
	 */
	constexpr double GReplayTimeoutSeconds = 600.0;

	/** Matches the dispatcher's ceiling on the per-call timeout_s override. */
	constexpr double GMaxTimeoutSeconds = 3600.0;

	/** The 'speed' ceiling, shared by the clamp and by the advice that offers it. */
	constexpr double GMaxSpeed = 10.0;

	/** How many unrecognised key names to name back before summarising. */
	constexpr int32 GMaxReportedKeys = 8;

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

	/**
	 * What one row actually did. Replay counted rows consumed and called that
	 * "replayed", which is a number a caller reads as "the game received this
	 * many": a key the build does not have was dropped here in silence and
	 * still counted.
	 */
	enum class EInjectResult : uint8
	{
		Handled,      // InputKey answered true
		Ignored,      // it went in, InputKey answered false
		UnknownKey,   // no such key in this build
		UnknownType,  // not a row shape this tool knows how to send
	};

	/**
	 * One row's outcome, and whether its verdict is worth reading.
	 *
	 * The bool APlayerController::InputKey hands back is not "the game used
	 * this". UPlayerInput::InputKey answers false for every analog and axis
	 * row whatever becomes of it; answers a digital press with whether a
	 * *legacy* ActionMapping covers the key, so false all day in an Enhanced
	 * Input project; and answers a digital release with an unconditional true.
	 * Only that release carries information - a false there means the event
	 * never reached UPlayerInput at all - so a "nothing was accepted" verdict
	 * may only be drawn from a take that contained one.
	 */
	struct FInjectOutcome
	{
		EInjectResult Result = EInjectResult::Ignored;
		bool bVerdictMeansSomething = false;
	};

	/**
	 * Send one row. HeldKeys tracks presses that have not been matched by a
	 * release yet, so an aborted replay can put the pad down before it leaves.
	 */
	FInjectOutcome InjectRecordedInput(APlayerController* PC, const FUplinkRecordedInput& Event, TArray<FKey>& HeldKeys)
	{
		FInjectOutcome Outcome;

		if (Event.Type == TEXT("mouse_move"))
		{
			bool bHandled = false;
			if (!FMath::IsNearlyZero(Event.X))
			{
				FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(EKeys::MouseX, IE_Axis, Event.X);
				Args.DeltaTime = FApp::GetDeltaTime();
				bHandled |= PC->InputKey(Args);
			}
			if (!FMath::IsNearlyZero(Event.Y))
			{
				// Slate counts cursor delta downward, the MouseY axis upward.
				FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(EKeys::MouseY, IE_Axis, -Event.Y);
				Args.DeltaTime = FApp::GetDeltaTime();
				bHandled |= PC->InputKey(Args);
			}
			Outcome.Result = bHandled ? EInjectResult::Handled : EInjectResult::Ignored;
			return Outcome;
		}

		const bool bIsKeyDown = Event.Type == TEXT("key_down");
		const bool bIsKeyUp = Event.Type == TEXT("key_up");
		const bool bIsAxis = Event.Type == TEXT("axis");
		if (!bIsKeyDown && !bIsKeyUp && !bIsAxis)
		{
			// Anything else used to be sent as a key press, so a typo in a
			// hand-written take pressed a key nobody asked for.
			Outcome.Result = EInjectResult::UnknownType;
			return Outcome;
		}

		const FKey Key(FName(*Event.Key));
		if (!EKeys::GetKeyDetails(Key).IsValid())
		{
			Outcome.Result = EInjectResult::UnknownKey;
			return Outcome;
		}

		EInputEvent InputEvent = IE_Pressed;
		float Amount = 1.0f;
		if (bIsKeyUp)
		{
			InputEvent = IE_Released;
			Amount = 0.0f;
		}
		else if (bIsAxis)
		{
			InputEvent = IE_Axis;
			Amount = Event.Amount;
		}

		FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(Key, InputEvent, Amount);
		Args.DeltaTime = FApp::GetDeltaTime();
		const bool bHandled = PC->InputKey(Args);

		if (bIsKeyDown)
		{
			HeldKeys.AddUnique(Key);
		}
		else if (bIsKeyUp)
		{
			HeldKeys.Remove(Key);
		}
		Outcome.Result = bHandled ? EInjectResult::Handled : EInjectResult::Ignored;
		Outcome.bVerdictMeansSomething = bIsKeyUp && !Key.IsAnalog();
		return Outcome;
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

			int32 Malformed = 0;
			const TArray<TSharedPtr<FJsonValue>>* EventArray = nullptr;
			const bool bCallerSuppliedEvents = Ctx.Params->TryGetArrayField(FStringView(TEXT("events")), EventArray);
			if (bCallerSuppliedEvents)
			{
				for (const TSharedPtr<FJsonValue>& Entry : *EventArray)
				{
					const TSharedPtr<FJsonObject>* Row = nullptr;
					FUplinkRecordedInput Event;
					if (Entry.IsValid() && Entry->TryGetObject(Row) && JsonToRecordedInput(*Row, Event))
					{
						Events.Add(Event);
					}
					else
					{
						++Malformed;
					}
				}
			}
			else
			{
				Events = Recorder.LastRecording();
			}

			if (Events.Num() == 0)
			{
				// Telling a caller who just passed 100 rows to "pass events"
				// sends them round the same loop. Say which half went wrong.
				Out = FUplinkToolResult::Error(bCallerSuppliedEvents
					? FString::Printf(
						TEXT("none of the %d rows in 'events' could be read - each row needs at least a 'type' of key_down, key_up, axis or mouse_move, and a 't' in seconds; pass the array input_record stop returned, unedited"),
						EventArray->Num())
					: TEXT("nothing to replay - record with input_record start/stop first, or pass 'events'"));
				return EUplinkToolStep::Done;
			}

			Events.Sort([](const FUplinkRecordedInput& A, const FUplinkRecordedInput& B) { return A.T < B.T; });
			Speed = FMath::Clamp(GetNumber(Ctx.Params, TEXT("speed"), 1.0), 0.1, GMaxSpeed);
			MalformedRows = Malformed;

			// A take that cannot finish inside the deadline is not worth
			// starting: the task manager kills it at the halfway mark and the
			// caller is left guessing why the pawn is still walking.
			//
			// Read timeout_s exactly the way the dispatcher does - a zero or
			// negative one is not an override there, and treating it as one
			// here would refuse a take against a deadline the call never had.
			double Deadline = GReplayTimeoutSeconds;
			double Override = 0.0;
			if (Ctx.Params->TryGetNumberField(FStringView(TEXT("timeout_s")), Override) && Override > 0.0)
			{
				Deadline = FMath::Clamp(Override, 1.0, GMaxTimeoutSeconds);
			}

			const double Expected = Events.Last().T / Speed;
			if (Expected > Deadline)
			{
				// The way out depends on how far over it is, and offering the
				// speed ceiling to someone already past it is advice that
				// fails on the retry.
				const double NeededSpeed = Events.Last().T / Deadline;
				const double AtTopSpeed = Events.Last().T / GMaxSpeed;
				if (NeededSpeed <= GMaxSpeed)
				{
					Out = FUplinkToolResult::Error(FString::Printf(
						TEXT("this take runs %.0fs at speed %.2g and the call gives up after %.0fs - pass speed %.2g or higher, or raise timeout_s (up to %.0f)"),
						Expected, Speed, Deadline, FMath::Min(GMaxSpeed, NeededSpeed * 1.05), GMaxTimeoutSeconds));
				}
				else if (AtTopSpeed <= GMaxTimeoutSeconds)
				{
					Out = FUplinkToolResult::Error(FString::Printf(
						TEXT("this take runs %.0fs at speed %.2g and the call gives up after %.0fs - even speed %.0f, the fastest this tool allows, needs %.0fs, so pass timeout_s %.0f with it"),
						Expected, Speed, Deadline, GMaxSpeed, AtTopSpeed, FMath::CeilToDouble(AtTopSpeed)));
				}
				else
				{
					Out = FUplinkToolResult::Error(FString::Printf(
						TEXT("this take is %.0fs long and needs %.0fs even at speed %.0f, past the %.0fs ceiling on timeout_s - replay it in sections by passing a slice of 'events' at a time"),
						Events.Last().T, AtTopSpeed, GMaxSpeed, GMaxTimeoutSeconds));
				}
				return EUplinkToolStep::Done;
			}

			StartTime = FPlatformTime::Seconds();
			return EUplinkToolStep::Pending;
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			FString Error;
			APlayerController* PC = GetPiePlayerController(Error);
			if (!PC)
			{
				// The world went away mid-take, so the keys went with it -
				// nothing to put down, and nothing left that could.
				HeldKeys.Reset();
				Out = FUplinkToolResult::Error(FString::Printf(
					TEXT("replay aborted after %d of %d events: %s"), NextIndex, Events.Num(), *Error));
				return EUplinkToolStep::Done;
			}

			const double Elapsed = (FPlatformTime::Seconds() - StartTime) * Speed;
			while (NextIndex < Events.Num() && Events[NextIndex].T <= Elapsed)
			{
				const FInjectOutcome Outcome = InjectRecordedInput(PC, Events[NextIndex], HeldKeys);
				if (Outcome.bVerdictMeansSomething)
				{
					++Verdicts;
				}
				switch (Outcome.Result)
				{
				case EInjectResult::Handled:
					++Injected;
					++Handled;
					break;
				case EInjectResult::Ignored:
					++Injected;
					break;
				case EInjectResult::UnknownKey:
					if (UnknownKeys.Num() < GMaxReportedKeys)
					{
						UnknownKeys.AddUnique(Events[NextIndex].Key);
					}
					++Skipped;
					break;
				case EInjectResult::UnknownType:
					if (UnknownTypes.Num() < GMaxReportedKeys)
					{
						UnknownTypes.AddUnique(Events[NextIndex].Type);
					}
					++Skipped;
					break;
				}
				++NextIndex;
			}
			if (NextIndex < Events.Num())
			{
				return EUplinkToolStep::Pending;
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("replayed"), Events.Num());
			Data->SetNumberField(TEXT("injected"), Injected);
			Data->SetNumberField(TEXT("handled"), Handled);
			Data->SetNumberField(TEXT("skipped"), Skipped);
			Data->SetNumberField(TEXT("seconds"), FPlatformTime::Seconds() - StartTime);
			if (MalformedRows > 0)
			{
				Data->SetNumberField(TEXT("malformedRows"), MalformedRows);
			}
			if (UnknownKeys.Num() > 0)
			{
				Data->SetStringField(TEXT("unknownKeys"), FString::Join(UnknownKeys, TEXT(", ")));
			}
			if (UnknownTypes.Num() > 0)
			{
				Data->SetStringField(TEXT("unknownTypes"), FString::Join(UnknownTypes, TEXT(", ")));
			}
			if (HeldKeys.Num() > 0)
			{
				TArray<FString> Names;
				for (const FKey& Key : HeldKeys)
				{
					Names.Add(Key.ToString());
				}
				// Left as the take left them: the recording really did end
				// mid-press. Said out loud, because a pawn that will not stop
				// walking otherwise looks like a bug in the game.
				Data->SetStringField(TEXT("keysDown"), FString::Join(Names, TEXT(", ")));
			}

			if (Injected == 0)
			{
				// Reporting "replayed: N, replay complete" here was the whole
				// problem: nothing reached the game and the caller was told the
				// take had run.
				const FString Reason = UnknownKeys.Num() > 0
					? FString::Printf(TEXT("no such key: %s"), *FString::Join(UnknownKeys, TEXT(", ")))
					: TEXT("unrecognised row types");
				Out = FUplinkToolResult::Error(FString::Printf(
					TEXT("replayed nothing: all %d events were dropped (%s) - the take was recorded against a different build, or hand-edited"),
					Events.Num(), *Reason));
				return EUplinkToolStep::Done;
			}

			// Every key release came back refused, and a release is refused
			// only before UPlayerInput is reached at all. One cause of that is
			// checkable rather than guessable, so check it - and say the
			// checked one as fact and the other as the thing to look at next.
			const UInputSettings* InputSettings = GetDefault<UInputSettings>();
			if (Verdicts > 0 && Handled == 0 && InputSettings && InputSettings->bFilterInputByPlatformUser)
			{
				Out = FUplinkToolResult::Error(FString::Printf(
					TEXT("none of the %d events reached the game: Project Settings > Input > 'Filter Input By Platform User' is on, and it drops simulated input, which carries no platform device behind it - turn it off to replay takes"),
					Injected));
				return EUplinkToolStep::Done;
			}

			FString Message;
			if (Verdicts > 0 && Handled == 0)
			{
				Message = TEXT("the player controller refused every key release, which means it has no player input of its own - the world's first player controller is not the one taking input here, so check for a second local player");
			}
			else if (Skipped > 0)
			{
				Message = FString::Printf(TEXT("replay complete, but %d of %d events were dropped before they reached the game"), Skipped, Events.Num());
			}
			else
			{
				Message = TEXT("replay complete");
			}
			Out = FUplinkToolResult::Ok(Data, Message);
			return EUplinkToolStep::Done;
		}

		/**
		 * A take abandoned halfway has pressed keys it will never release, and
		 * the engine keeps them down long after the tool is gone - a cancelled
		 * replay used to hand back the session with the pawn still running.
		 */
		virtual void Cancel(EUplinkCancelReason Reason) override
		{
			if (HeldKeys.Num() == 0)
			{
				return;
			}
			FString Error;
			if (APlayerController* PC = GetPiePlayerController(Error))
			{
				for (const FKey& Key : HeldKeys)
				{
					FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.0f);
					Args.DeltaTime = FApp::GetDeltaTime();
					PC->InputKey(Args);
				}
			}
			HeldKeys.Reset();
		}

	private:
		FUplinkInputRecorder& Recorder;
		TArray<FUplinkRecordedInput> Events;
		TArray<FKey> HeldKeys;
		TArray<FString> UnknownKeys;
		TArray<FString> UnknownTypes;
		double StartTime = 0.0;
		double Speed = 1.0;
		int32 NextIndex = 0;
		int32 Injected = 0;
		int32 Handled = 0;

		/** Rows whose Handled/Ignored answer was worth anything - see FInjectOutcome. */
		int32 Verdicts = 0;

		int32 Skipped = 0;
		int32 MalformedRows = 0;
	};
}

void UplinkTools::RegisterRecord(FUplinkToolRegistry& Registry, FUplinkInputRecorder& Recorder)
{
	Registry.RegisterQuick(
		TEXT("input_record"),
		TEXT("Record the human's real input through a passive Slate tap: key and mouse-button presses, analog axes, and mouse movement, each timestamped from the start of the take. action: 'start' | 'stop' (returns the events - keep them to replay later) | 'status'. The tap is editor-wide rather than viewport-only, so keys typed into editor panels land in the take too. Not captured: the mouse wheel, and the second press of a double-click. A recording auto-stops when PIE ends and is kept as the last take, which 'stop' still hands back."),
		TEXT(R"json({"type":"object","properties":{"action":{"type":"string","enum":["start","stop","status"]}},"required":["action"]})json"),
		/*bReadOnly=*/false,
		[&Recorder](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Action = GetString(Ctx.Params, TEXT("action"));
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

			if (Action == TEXT("start"))
			{
				// Starting throws the previous take away, and the take is the
				// only copy unless the caller kept what stop returned.
				const int32 Discarded = Recorder.IsRecording() ? 0 : Recorder.NumEvents();
				FString Error;
				if (!Recorder.StartRecording(Error))
				{
					return FUplinkToolResult::Error(Error);
				}
				return FUplinkToolResult::Ok(nullptr, Discarded > 0
					? FString::Printf(TEXT("recording - play the game, then input_record stop (the previous take of %d events is gone)"), Discarded)
					: TEXT("recording - play the game, then input_record stop"));
			}
			if (Action == TEXT("stop"))
			{
				// PIE ending stops the recording for you, which is the normal
				// way a session ends. Refusing here because of that lost the
				// caller the take they had just finished making.
				const bool bWasRecording = Recorder.IsRecording();
				if (!bWasRecording && Recorder.NumEvents() == 0)
				{
					return FUplinkToolResult::Error(
						TEXT("not recording, and no take is held - call input_record start, play, then stop"));
				}
				const TArray<FUplinkRecordedInput>& Events = bWasRecording
					? Recorder.StopRecording()
					: Recorder.LastRecording();
				TArray<TSharedPtr<FJsonValue>> Rows;
				for (const FUplinkRecordedInput& Event : Events)
				{
					Rows.Add(MakeShared<FJsonValueObject>(RecordedInputToJson(Event)));
				}
				Data->SetArrayField(TEXT("events"), Rows);
				Data->SetNumberField(TEXT("count"), Events.Num());
				Data->SetNumberField(TEXT("seconds"), Recorder.RecordingSeconds());
				return FUplinkToolResult::Ok(Data, bWasRecording
					? FString()
					: TEXT("this take had already been stopped by PIE ending; these are its events"));
			}
			if (Action == TEXT("status"))
			{
				const bool bRecording = Recorder.IsRecording();
				Data->SetBoolField(TEXT("recording"), bRecording);
				Data->SetNumberField(TEXT("count"), Recorder.NumEvents());
				Data->SetNumberField(TEXT("seconds"), Recorder.RecordingSeconds());
				// Whether stop still has something to give back.
				Data->SetBoolField(TEXT("hasTake"), !bRecording && Recorder.NumEvents() > 0);
				return FUplinkToolResult::Ok(Data);
			}
			return FUplinkToolResult::Error(TEXT("action must be start, stop, or status"));
		});

	FUplinkToolInfo Info;
	Info.Name = TEXT("input_replay");
	Info.Description = TEXT("Replay a recorded take into the running game by driving the player controller's simulated-input path. Uses the last input_record take, or pass 'events' from an earlier stop. Events fire at their recorded timestamps rather than all at once; 'speed' scales that clock, and a take too long to finish inside the call's deadline is refused up front rather than cut in half. What this reaches is the player controller, so gameplay input reproduces but UI does not - a menu button clicked in the take is not clicked again, because UMG is driven by Slate rather than by player input (use click_widget for that). Reports how many rows went in ('injected'), how many were dropped for naming a key this build does not have ('skipped', with 'unknownKeys'), and which keys the take left held down ('keysDown'). 'handled' is the raw verdict of the engine's InputKey and is a floor, not a total: an analog or mouse row always answers false and a key release always answers true, so read only this much from it - zero across a take that contained key releases means nothing reached the game at all, and the message then names the reason.");
	Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"events":{"type":"array","items":{"type":"object"},"description":"Events from input_record stop (omit to use the last take)"},"speed":{"type":"number","default":1.0,"description":"Playback rate; clamped to 0.1 - 10"}}})json"));
	Info.bReadOnly = false;
	Info.bTransactional = false; // drives the session, not an undoable edit
	Info.TimeoutSeconds = GReplayTimeoutSeconds;
	Registry.Register(MoveTemp(Info), [&Recorder]() -> TSharedRef<IUplinkInvocation>
	{
		return MakeShared<FReplayInvocation>(Recorder);
	});
}
