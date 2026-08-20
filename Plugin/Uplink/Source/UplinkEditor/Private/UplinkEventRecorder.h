// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UObject/StrongObjectPtr.h"

class UUplinkEventListener;

/**
 * Captures dynamic-multicast delegate broadcasts (OnGrabbed, OnValueChanged,
 * any BlueprintAssignable event) into a ring buffer so an agent can *prove* a
 * gameplay event happened instead of guessing from screenshots.
 *
 * Each watched delegate gets its own listener UObject bound through
 * FScriptDelegate; the listener overrides ProcessEvent to intercept the
 * broadcast and decodes the parameter frame using the delegate's own
 * signature function. Watches are stopped automatically when PIE ends.
 */
class FUplinkEventRecorder
{
public:
	struct FEvent
	{
		int64 Seq = 0;
		double AtSeconds = 0.0;
		FGuid WatchId;
		FString ObjectPath;
		FString Delegate;
		TSharedPtr<FJsonObject> Payload;
	};

	FUplinkEventRecorder();
	~FUplinkEventRecorder();

	/**
	 * Bind to DelegateName on Object ("*" binds every multicast delegate on
	 * its class). Returns a watch id; OutBound lists the delegate names bound.
	 */
	FGuid StartWatch(UObject* Object, const FString& DelegateName, TArray<FString>& OutBound, FString& OutError);

	bool StopWatch(const FGuid& WatchId);
	void StopAllWatches();

	/** Called by listeners on the game thread when a watched delegate fires. */
	void Record(const FGuid& WatchId, const FString& ObjectPath, const FString& Delegate, TSharedPtr<FJsonObject> Payload);

	/** Events with Seq >= SinceSeq (optionally one watch only), oldest first. */
	TArray<FEvent> Drain(int64 SinceSeq, const FGuid* WatchFilter, int32 MaxEvents,
		bool* bOutTruncated = nullptr) const;

	int64 NewestSeq() const { return NextSeq; }
	int32 EventCountForWatch(const FGuid& WatchId, int64 SinceSeq) const;
	TArray<TPair<FGuid, FString>> ActiveWatches() const;

private:
	struct FBinding
	{
		TStrongObjectPtr<UUplinkEventListener> Listener;
		TWeakObjectPtr<UObject> Source;
		FName DelegateName;
	};

	struct FWatch
	{
		FString Description;
		TArray<FBinding> Bindings;
	};

	void ReleaseWatch(FWatch& Watch);
	void HandleEndPie(const bool bIsSimulating);

	TMap<FGuid, FWatch> Watches;
	TArray<FEvent> Events;
	int64 NextSeq = 0;
	FDelegateHandle EndPieHandle;

	static constexpr int32 MaxBufferedEvents = 1000;
};
