// Copyright (c) 2026 Low Sze Hao. MIT License.

#pragma once

#include "CoreMinimal.h"

class FUplinkLogCapture;

enum class EUplinkPieState : uint8
{
	None,      // no session
	Starting,  // requested / loading, BeginPlay not yet run
	Running,   // fully started (PostPIEStarted fired)
	Stopping,  // ending
};

/**
 * Tracks the Play-In-Editor session through FEditorDelegates so tools never
 * have to guess: Starting means the request is in flight, Running means
 * BeginPlay has executed. SessionSerial increments on every successful start,
 * letting latent tools detect that "their" session ended and a new one began.
 */
class FUplinkPieManager
{
public:
	explicit FUplinkPieManager(FUplinkLogCapture* InLogCapture);
	~FUplinkPieManager();

	EUplinkPieState GetState() const;
	bool IsPaused() const;
	uint64 GetSessionSerial() const { return SessionSerial; }
	double GetSessionStartTime() const { return SessionStartTime; }
	int64 GetLogIndexAtStart() const { return LogIndexAtStart; }
	FString GetPieMapName() const;

private:
	void HandleStartPie(const bool bIsSimulating);
	void HandlePostPieStarted(const bool bIsSimulating);
	void HandleEndPie(const bool bIsSimulating);
	void HandleShutdownPie(const bool bIsSimulating);
	void HandleCancelPie();

	FUplinkLogCapture* LogCapture = nullptr;

	EUplinkPieState State = EUplinkPieState::None;
	uint64 SessionSerial = 0;
	double SessionStartTime = 0.0;
	int64 LogIndexAtStart = 0;

	FDelegateHandle StartHandle;
	FDelegateHandle PostStartedHandle;
	FDelegateHandle EndHandle;
	FDelegateHandle ShutdownHandle;
	FDelegateHandle CancelHandle;
};
