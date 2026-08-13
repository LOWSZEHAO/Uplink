// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkTaskManager.h"
#include "UplinkEditorModule.h"
#include "Misc/App.h"

FUplinkTaskManager::FUplinkTaskManager()
{
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FUplinkTaskManager::TickTasks), 0.0f);
}

FUplinkTaskManager::~FUplinkTaskManager()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	}
}

FGuid FUplinkTaskManager::Submit(
	const FString& ToolName,
	TSharedRef<IUplinkInvocation> Invocation,
	FUplinkToolContext Context,
	double TimeoutSeconds)
{
	check(IsInGameThread());

	FEntry Entry;
	Entry.Task.Id = FGuid::NewGuid();
	Entry.Task.ToolName = ToolName;
	Entry.Task.StartedAt = FPlatformTime::Seconds();
	Entry.Task.TimeoutSeconds = TimeoutSeconds;
	Entry.Invocation = Invocation;
	Entry.Context = MoveTemp(Context);

	const FGuid Id = Entry.Task.Id;
	FEntry& Stored = Entries.Add(Id, MoveTemp(Entry));

	// First step right away so quick tools answer on the submitting tick.
	StepEntry(Stored);
	return Id;
}

void FUplinkTaskManager::Await(const FGuid& TaskId, double MaxWaitSeconds, FWaiter Waiter)
{
	check(IsInGameThread());

	FEntry* Entry = Entries.Find(TaskId);
	if (!Entry)
	{
		FUplinkTask Ghost;
		Ghost.Id = TaskId;
		Ghost.Status = EUplinkTaskStatus::Failed;
		Ghost.Result = FUplinkToolResult::Error(TEXT("unknown task id (results are retained ~3 minutes)"));
		Waiter(Ghost, false);
		return;
	}

	if (Entry->Task.Status != EUplinkTaskStatus::Running)
	{
		Waiter(Entry->Task, false);
		return;
	}

	FEntry::FPendingWaiter Pending;
	Pending.Deadline = FPlatformTime::Seconds() + MaxWaitSeconds;
	Pending.Fn = MoveTemp(Waiter);
	Entry->Waiters.Add(MoveTemp(Pending));
}

const FUplinkTask* FUplinkTaskManager::Find(const FGuid& TaskId) const
{
	const FEntry* Entry = Entries.Find(TaskId);
	return Entry ? &Entry->Task : nullptr;
}

bool FUplinkTaskManager::Cancel(const FGuid& TaskId)
{
	check(IsInGameThread());
	FEntry* Entry = Entries.Find(TaskId);
	if (!Entry || Entry->Task.Status != EUplinkTaskStatus::Running)
	{
		return false;
	}
	Entry->Task.Result = FUplinkToolResult::Error(TEXT("cancelled"));
	FinishEntry(*Entry, EUplinkTaskStatus::Cancelled);
	return true;
}

TArray<const FUplinkTask*> FUplinkTaskManager::Snapshot() const
{
	TArray<const FUplinkTask*> Out;
	for (const auto& Pair : Entries)
	{
		Out.Add(&Pair.Value.Task);
	}
	return Out;
}

FString FUplinkTaskManager::StatusToString(EUplinkTaskStatus Status)
{
	switch (Status)
	{
	case EUplinkTaskStatus::Running:   return TEXT("running");
	case EUplinkTaskStatus::Completed: return TEXT("completed");
	case EUplinkTaskStatus::Failed:    return TEXT("failed");
	case EUplinkTaskStatus::Cancelled: return TEXT("cancelled");
	case EUplinkTaskStatus::TimedOut:  return TEXT("timed_out");
	}
	return TEXT("unknown");
}

bool FUplinkTaskManager::TickTasks(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();

	TArray<FGuid> ToPurge;
	for (auto& Pair : Entries)
	{
		FEntry& Entry = Pair.Value;

		if (Entry.Task.Status == EUplinkTaskStatus::Running)
		{
			if (Now - Entry.Task.StartedAt > Entry.Task.TimeoutSeconds)
			{
				Entry.Task.Result = FUplinkToolResult::Error(
					FString::Printf(TEXT("tool '%s' timed out after %.0fs"), *Entry.Task.ToolName, Entry.Task.TimeoutSeconds));
				FinishEntry(Entry, EUplinkTaskStatus::TimedOut);
			}
			else
			{
				StepEntry(Entry);
				FlushWaiters(Entry, /*bDeadlinesOnly=*/true);
			}
		}
		else if (Entry.Waiters.Num() == 0 && Now - Entry.Task.FinishedAt > ResultRetentionSeconds)
		{
			ToPurge.Add(Pair.Key);
		}
	}

	for (const FGuid& Id : ToPurge)
	{
		Entries.Remove(Id);
	}
	return true; // keep ticking
}

void FUplinkTaskManager::StepEntry(FEntry& Entry)
{
	if (Entry.Task.Status != EUplinkTaskStatus::Running)
	{
		return;
	}

	FUplinkToolResult Out;
	EUplinkToolStep Step;
	if (!Entry.bStarted)
	{
		Entry.bStarted = true;
		Step = Entry.Invocation->Start(Entry.Context, Out);
	}
	else
	{
		Step = Entry.Invocation->Tick(Entry.Context, Out);
	}

	if (Step == EUplinkToolStep::Done)
	{
		Entry.Task.Result = MoveTemp(Out);
		FinishEntry(Entry, Entry.Task.Result.bError ? EUplinkTaskStatus::Failed : EUplinkTaskStatus::Completed);
	}
}

void FUplinkTaskManager::FinishEntry(FEntry& Entry, EUplinkTaskStatus Status)
{
	Entry.Task.Status = Status;
	Entry.Task.FinishedAt = FPlatformTime::Seconds();
	Entry.Invocation.Reset();
	FlushWaiters(Entry, /*bDeadlinesOnly=*/false);
}

void FUplinkTaskManager::FlushWaiters(FEntry& Entry, bool bDeadlinesOnly)
{
	const double Now = FPlatformTime::Seconds();
	const bool bRunning = Entry.Task.Status == EUplinkTaskStatus::Running;

	TArray<FEntry::FPendingWaiter> Fire;
	for (int32 i = Entry.Waiters.Num() - 1; i >= 0; --i)
	{
		if (!bDeadlinesOnly || Entry.Waiters[i].Deadline <= Now)
		{
			Fire.Add(MoveTemp(Entry.Waiters[i]));
			Entry.Waiters.RemoveAt(i);
		}
	}
	for (FEntry::FPendingWaiter& Waiter : Fire)
	{
		Waiter.Fn(Entry.Task, bRunning);
	}
}
