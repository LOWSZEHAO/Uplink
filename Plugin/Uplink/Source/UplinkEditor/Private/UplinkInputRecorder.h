// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Records real player input (keys, mouse buttons, analog axes, mouse moves)
// through a Slate input pre-processor, timestamped for later replay.

#pragma once

#include "CoreMinimal.h"

/** One captured input event, relative-timestamped from recording start. */
struct FUplinkRecordedInput
{
	double T = 0.0;        // seconds since recording started
	FString Type;          // key_down | key_up | axis | mouse_move
	FString Key;           // key name (empty for mouse_move)
	float Amount = 0.0f;   // axis value
	float X = 0.0f;        // mouse_move delta
	float Y = 0.0f;
};

class FUplinkInputRecorder
{
public:
	FUplinkInputRecorder();
	~FUplinkInputRecorder();

	bool StartRecording(FString& OutError);
	/** Stops and returns the capture; it also stays cached as the last recording. */
	const TArray<FUplinkRecordedInput>& StopRecording();
	bool IsRecording() const;
	int32 NumEvents() const { return Events.Num(); }
	const TArray<FUplinkRecordedInput>& LastRecording() const { return Events; }
	double RecordingSeconds() const;

private:
	void Add(FUplinkRecordedInput&& Event);

	class FProcessor;
	friend class FProcessor;

	TSharedPtr<FProcessor> Processor;
	TArray<FUplinkRecordedInput> Events;
	double StartTime = 0.0;
	double StopTime = 0.0;
	FDelegateHandle EndPieHandle;
};
