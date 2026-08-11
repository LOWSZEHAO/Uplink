// Copyright (c) 2026 Low Sze Hao. MIT License.

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
	// Delegate-tracked state, with the live PlayWorld as a safety net in case
	// this object was created while a session already existed.
	if (State == EUplinkPieState::None && GEditor && GEditor->PlayWorld)
	{
		return EUplinkPieState::Running;
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
