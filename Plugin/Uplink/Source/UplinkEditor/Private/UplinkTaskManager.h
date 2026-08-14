// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UplinkTool.h"

enum class EUplinkTaskStatus : uint8
{
	Running,
	Completed,
	Failed,
	Cancelled,
	TimedOut,
};

struct FUplinkTask
{
	FGuid Id;
	FString ToolName;
	EUplinkTaskStatus Status = EUplinkTaskStatus::Running;
	FUplinkToolResult Result;
	double StartedAt = 0.0;
	double TimeoutSeconds = 30.0;
	double FinishedAt = 0.0;
};

/**
 * Runs every tool invocation as a task on the game thread, driven by a single
 * FTSTicker callback per editor tick. Quick tools complete on the same tick
 * they were submitted (their HTTP response goes out immediately); latent tools
 * are Ticked until Done, timeout, or cancel. Callers attach waiters: a waiter
 * fires when the task finishes OR when the waiter's own deadline passes
 * (whichever is first), which is how HTTP responses are deferred without ever
 * blocking the game thread.
 */
class FUplinkTaskManager
{
public:
	/** Waiter: called exactly once, on the game thread. */
	using FWaiter = TFunction<void(const FUplinkTask& Task, bool bStillRunning)>;

	FUplinkTaskManager();
	~FUplinkTaskManager();

	/**
	 * Submit and immediately run the first step (same tick). Returns task id.
	 * A tool that is not read-only runs inside an editor transaction, so its
	 * edits are one undo step; see StepEntry.
	 */
	FGuid Submit(
		const FString& ToolName,
		TSharedRef<IUplinkInvocation> Invocation,
		FUplinkToolContext Context,
		double TimeoutSeconds,
		bool bReadOnly = true,
		bool bTransactional = true);

	/** Attach a waiter; fires now if the task is already finished or unknown. */
	void Await(const FGuid& TaskId, double MaxWaitSeconds, FWaiter Waiter);

	const FUplinkTask* Find(const FGuid& TaskId) const;
	bool Cancel(const FGuid& TaskId);

	TArray<const FUplinkTask*> Snapshot() const;

	static FString StatusToString(EUplinkTaskStatus Status);

private:
	struct FEntry
	{
		FUplinkTask Task;
		TSharedPtr<IUplinkInvocation> Invocation;
		FUplinkToolContext Context;
		bool bStarted = false;
		bool bReadOnly = true;
		bool bTransactional = true;

		/** True while this task holds an open editor transaction. */
		bool bTransactionOpen = false;

		struct FPendingWaiter
		{
			double Deadline = 0.0;
			FWaiter Fn;
		};
		TArray<FPendingWaiter> Waiters;
	};

	bool TickTasks(float DeltaTime);
	void StepEntry(FEntry& Entry);
	void FinishEntry(FEntry& Entry, EUplinkTaskStatus Status);
	void FlushWaiters(FEntry& Entry, bool bDeadlinesOnly);

	TMap<FGuid, FEntry> Entries;
	FTSTicker::FDelegateHandle TickerHandle;

	/** Completed tasks are kept this long for polling, then purged. */
	static constexpr double ResultRetentionSeconds = 180.0;
};
