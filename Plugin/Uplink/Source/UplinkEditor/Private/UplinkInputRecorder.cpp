// Copyright (c) 2026 Low Sze Hao. MIT License.

#include "UplinkInputRecorder.h"

#include "Editor.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"

/** Passive observer - records and never consumes, so play feels untouched. */
class FUplinkInputRecorder::FProcessor : public IInputProcessor
{
public:
	explicit FProcessor(FUplinkInputRecorder& InOwner) : Owner(InOwner) {}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
	{
		if (!InKeyEvent.IsRepeat())
		{
			FUplinkRecordedInput Event;
			Event.Type = TEXT("key_down");
			Event.Key = InKeyEvent.GetKey().ToString();
			Owner.Add(MoveTemp(Event));
		}
		return false;
	}

	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
	{
		FUplinkRecordedInput Event;
		Event.Type = TEXT("key_up");
		Event.Key = InKeyEvent.GetKey().ToString();
		Owner.Add(MoveTemp(Event));
		return false;
	}

	virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& InAnalogInputEvent) override
	{
		FUplinkRecordedInput Event;
		Event.Type = TEXT("axis");
		Event.Key = InAnalogInputEvent.GetKey().ToString();
		Event.Amount = InAnalogInputEvent.GetAnalogValue();
		Owner.Add(MoveTemp(Event));
		return false;
	}

	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
	{
		const FVector2f Delta = MouseEvent.GetCursorDelta();
		if (!Delta.IsNearlyZero())
		{
			FUplinkRecordedInput Event;
			Event.Type = TEXT("mouse_move");
			Event.X = Delta.X;
			Event.Y = Delta.Y;
			Owner.Add(MoveTemp(Event));
		}
		return false;
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
	{
		FUplinkRecordedInput Event;
		Event.Type = TEXT("key_down");
		Event.Key = MouseEvent.GetEffectingButton().ToString();
		Owner.Add(MoveTemp(Event));
		return false;
	}

	virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
	{
		FUplinkRecordedInput Event;
		Event.Type = TEXT("key_up");
		Event.Key = MouseEvent.GetEffectingButton().ToString();
		Owner.Add(MoveTemp(Event));
		return false;
	}

private:
	FUplinkInputRecorder& Owner;
};

FUplinkInputRecorder::FUplinkInputRecorder()
{
	// A recording left running when PIE ends stops itself and keeps the take.
	EndPieHandle = FEditorDelegates::EndPIE.AddLambda([this](bool)
	{
		if (IsRecording())
		{
			StopRecording();
		}
	});
}

FUplinkInputRecorder::~FUplinkInputRecorder()
{
	FEditorDelegates::EndPIE.Remove(EndPieHandle);
	if (Processor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(Processor);
	}
	Processor.Reset();
}

bool FUplinkInputRecorder::StartRecording(FString& OutError)
{
	if (IsRecording())
	{
		OutError = TEXT("already recording - stop first");
		return false;
	}
	if (!FSlateApplication::IsInitialized())
	{
		OutError = TEXT("Slate is not initialized");
		return false;
	}
	Events.Reset();
	StartTime = FPlatformTime::Seconds();
	StopTime = 0.0;
	Processor = MakeShared<FProcessor>(*this);
	FSlateApplication::Get().RegisterInputPreProcessor(Processor);
	return true;
}

const TArray<FUplinkRecordedInput>& FUplinkInputRecorder::StopRecording()
{
	if (Processor.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(Processor);
		}
		Processor.Reset();
		StopTime = FPlatformTime::Seconds();
	}
	return Events;
}

bool FUplinkInputRecorder::IsRecording() const
{
	return Processor.IsValid();
}

double FUplinkInputRecorder::RecordingSeconds() const
{
	if (IsRecording())
	{
		return FPlatformTime::Seconds() - StartTime;
	}
	return StopTime > StartTime ? StopTime - StartTime : 0.0;
}

void FUplinkInputRecorder::Add(FUplinkRecordedInput&& Event)
{
	Event.T = FPlatformTime::Seconds() - StartTime;
	Events.Add(MoveTemp(Event));
}
