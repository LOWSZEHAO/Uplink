// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkPieManager.h"
#include "UplinkEditorModule.h"
#include "UplinkLogCapture.h"
#include "Editor.h"

FUplinkPieManager::FUplinkPieManager(FUplinkLogCapture* InLogCapture)
	: LogCapture(InLogCapture)
{
	StartHandle = FEditorDelegates::StartPIE.AddRaw(this, &FUplinkPieManager::HandleStartPie);
	PostStartedHandle = FEditorDelegates::PostPIEStarted.AddRaw(this, &FUplinkPieManager::HandlePostPieStarted);
	EndHandle = FEditorDelegates::EndPIE.AddRaw(this, &FUplinkPieManager::HandleEndPie);
	ShutdownHandle = FEditorDelegates::ShutdownPIE.AddRaw(this, &FUplinkPieManager::HandleShutdownPie);
	CancelHandle = FEditorDelegates::CancelPIE.AddRaw(this, &FUplinkPieManager::HandleCancelPie);
}

FUplinkPieManager::~FUplinkPieManager()
{
	FEditorDelegates::StartPIE.Remove(StartHandle);
	FEditorDelegates::PostPIEStarted.Remove(PostStartedHandle);
	FEditorDelegates::EndPIE.Remove(EndHandle);
	FEditorDelegates::ShutdownPIE.Remove(ShutdownHandle);
	FEditorDelegates::CancelPIE.Remove(CancelHandle);
}

EUplinkPieState FUplinkPieManager::GetState() const
{
	// The delegates are the primary source, but they can be missed, so
	// reconcile against the live PlayWorld in BOTH directions.
	//
	// Missing the downward case used to strand the manager: a start that fails
	// while the game thread is blocked (a modal error dialog is enough) fires
	// EndPIE without a matching ShutdownPIE, leaving the state at Stopping
	// forever - pie_status reported "stopping" while the engine had no session
	// at all, and pie_stop waited for an end that had already happened.
	const bool bHasPlayWorld = GEditor && GEditor->PlayWorld;

	if (bHasPlayWorld)
	{
		if (State == EUplinkPieState::None)
		{
			State = EUplinkPieState::Running;
		}
	}
	// Starting legitimately has no PlayWorld yet, so it is left alone; the
	// start request times out on its own if it never arrives.
	else if (State == EUplinkPieState::Running || State == EUplinkPieState::Stopping)
	{
		State = EUplinkPieState::None;
	}

	return State;
}

bool FUplinkPieManager::IsPaused() const
{
	return GEditor && GEditor->PlayWorld && GEditor->PlayWorld->bDebugPauseExecution;
}

FString FUplinkPieManager::GetPieMapName() const
{
	if (GEditor && GEditor->PlayWorld)
	{
		return GEditor->PlayWorld->GetOutermost()->GetName();
	}
	return FString();
}

void FUplinkPieManager::HandleStartPie(const bool bIsSimulating)
{
	State = EUplinkPieState::Starting;
}

void FUplinkPieManager::HandlePostPieStarted(const bool bIsSimulating)
{
	State = EUplinkPieState::Running;
	++SessionSerial;
	SessionStartTime = FPlatformTime::Seconds();
	LogIndexAtStart = LogCapture ? LogCapture->NewestIndex() : 0;
	UE_LOG(LogUplink, Log, TEXT("PIE session %llu started (%s)"), SessionSerial, *GetPieMapName());
}

void FUplinkPieManager::HandleEndPie(const bool bIsSimulating)
{
	// EndPIE also fires on failed starts, so it always means "winding down".
	State = EUplinkPieState::Stopping;
}

void FUplinkPieManager::HandleShutdownPie(const bool bIsSimulating)
{
	State = EUplinkPieState::None;
	UE_LOG(LogUplink, Log, TEXT("PIE session shut down"));
}

void FUplinkPieManager::HandleCancelPie()
{
	if (State == EUplinkPieState::Starting)
	{
		State = EUplinkPieState::None;
	}
}
