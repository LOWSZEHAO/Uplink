// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Misc/Optional.h"
#include "UplinkTool.h"

/**
 * Where a task is in its life. Running and Cancelling are live; everything
 * below them is terminal and the Result is final. Test with
 * FUplinkTaskManager::IsTerminal rather than comparing against Running -
 * Running stopped being the only live state when Cancelling arrived.
 */
enum class EUplinkTaskStatus : uint8
{
	Running,

	/**
	 * A caller asked this task to stop and the invocation reported that it
	 * cannot be interrupted safely (IUplinkInvocation::CanCancel). The work is
	 * still ticking and still owns whatever it owns; it ends when its deadline
	 * passes.
	 *
	 * This is the only gap there is between "asked to stop" and "stopped".
	 * Cancel() is synchronous, so a cancel that is honoured completes inside
	 * the call that asks for it and is never observable from outside - which is
	 * why there is no state for "unwinding".
	 */
	Cancelling,

	Completed,
	Failed,
	Cancelled,
	TimedOut,
};

/** What came of asking a task to stop. */
enum class EUplinkCancelOutcome : uint8
{
	/** The invocation was told to stop and the task is finished. */
	Stopped,

	/** The invocation refuses interruption; the task runs on to its deadline. */
	Refused,

	/** The task exists but had already reached a terminal state. */
	AlreadyEnded,

	/** No task with that id - it never existed, or its result was purged. */
	UnknownTask,
};

/**
 * The kinds of thing a task can hold while it runs.
 *
 * Every task runs on the game thread, so there is no data race to prevent
 * here. What is still possible is logical interleaving: two latent tasks each
 * stepping once per tick can both be halfway through editing the same
 * Blueprint, or one can start a PIE session while the other is mid-way through
 * an asset edit. Neither task is wrong on its own; together they produce
 * nonsense.
 *
 * The kinds form a shallow containment tree - the editor over everything, the
 * world over the PIE session, an asset over the Blueprint stored in it. That
 * containment is what makes a coarse lock worth taking: without it a task
 * holding the whole editor would not conflict with a task holding one asset,
 * and the coarse lock would protect nothing.
 */
enum class EUplinkResourceKind : uint8
{
	Editor,     // global editor state; parent of every other kind
	World,      // the editor or PIE world
	Pie,        // starting or stopping a session
	Asset,      // one asset, keyed by package path
	Blueprint,  // one Blueprint, keyed by package path
};

/**
 * One resource a task needs, and how it needs it. Shared holders never exclude
 * each other, which is what keeps reads out of each other's way.
 *
 * This lives here rather than in UplinkTool.h because nothing outside the task
 * manager declares resources yet; see FUplinkTaskManager::DeriveResources for
 * how they are guessed in the meantime, and for what changes when tools start
 * declaring their own.
 */
struct FUplinkResourceLock
{
	EUplinkResourceKind Kind = EUplinkResourceKind::Editor;

	/** Normalised package path for Asset/Blueprint; empty for the global kinds. */
	FString Key;

	bool bExclusive = false;

	/** "the editor (exclusive)", "blueprint /game/foo (shared)". */
	FString Describe() const;
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

	/**
	 * Set while the task has been accepted but has not started, because
	 * something else holds a resource it needs; WaitingOn names that resource
	 * and who holds it. A waiting task is still Running to everyone outside,
	 * and its clock is still running too, so one that can never acquire times
	 * out rather than queueing forever.
	 */
	bool bWaiting = false;
	FString WaitingOn;
};

/**
 * Runs every tool invocation as a task on the game thread, driven by a single
 * FTSTicker callback per editor tick. Quick tools complete on the same tick
 * they were submitted (their HTTP response goes out immediately); latent tools
 * are Ticked until Done, timeout, or cancel. Callers attach waiters: a waiter
 * fires when the task finishes OR when the waiter's own deadline passes
 * (whichever is first), which is how HTTP responses are deferred without ever
 * blocking the game thread.
 *
 * A task does not start until it holds the resources it needs (see
 * EUplinkResourceKind), so two tasks cannot be mid-edit on the same thing at
 * once. Tasks that cannot acquire wait in submission order; they hold nothing
 * while they wait, which is the whole of the deadlock argument.
 */
class FUplinkTaskManager
{
public:
	/** Waiter: called exactly once, on the game thread. */
	using FWaiter = TFunction<void(const FUplinkTask& Task, bool bStillRunning)>;

	FUplinkTaskManager();
	~FUplinkTaskManager();

	/**
	 * Submit and immediately run the first step (same tick), unless another
	 * task holds a resource this one needs - then it starts as soon as that
	 * frees up. Returns task id.
	 *
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

	/**
	 * Ask a task to stop, and say what came of it. OutMessage is written for
	 * the caller of the tool, not for the log: it names the next thing to do
	 * when the answer is not a plain "cancelled".
	 *
	 * Prefer this over Cancel below - a bool cannot express a refusal, and
	 * reporting "not running" for a task that is very much still running is the
	 * kind of answer that sends someone hunting in the wrong place.
	 */
	EUplinkCancelOutcome RequestCancel(const FGuid& TaskId, FString& OutMessage);

	/** Older form: true only when the task actually stopped. */
	bool Cancel(const FGuid& TaskId);

	TArray<const FUplinkTask*> Snapshot() const;

	static FString StatusToString(EUplinkTaskStatus Status);

	/** True once the task will not step again and its Result is final. */
	static bool IsTerminal(EUplinkTaskStatus Status);

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

		/** True between acquiring this task's resources and releasing them. */
		bool bHoldsLocks = false;

		/**
		 * Cancel() reaches an invocation once and once only, however a stop
		 * arrives - a caller cancelling on the same tick a deadline passes must
		 * not tell the tool twice.
		 */
		bool bCancelDelivered = false;

		TArray<FUplinkResourceLock> Resources;

		struct FPendingWaiter
		{
			double Deadline = 0.0;
			FWaiter Fn;
		};
		TArray<FPendingWaiter> Waiters;
	};

	bool TickTasks(float DeltaTime);

	/** Start whatever queued tasks can now hold everything they need. */
	void PumpPending();

	void StepEntry(FEntry& Entry);

	/**
	 * The one chokepoint every terminal path runs through. StopReason unset
	 * means the invocation finished on its own and must not be told to stop.
	 */
	void FinishEntry(
		FEntry& Entry,
		EUplinkTaskStatus Status,
		TOptional<EUplinkCancelReason> StopReason = TOptional<EUplinkCancelReason>());

	/** Everything an entry must let go of, exactly once, when it stops running. */
	void ReleaseEntry(FEntry& Entry, TOptional<EUplinkCancelReason> StopReason);

	void FlushWaiters(FEntry& Entry, bool bDeadlinesOnly);

	/** What a tool needs, guessed from what Submit is given. */
	static TArray<FUplinkResourceLock> DeriveResources(
		const FString& ToolName, bool bReadOnly, const FUplinkToolContext& Ctx);

	/** All of Wanted or none of it; OutBlockedBy names the first refusal. */
	bool TryAcquire(const FGuid& Owner, const TArray<FUplinkResourceLock>& Wanted, FString& OutBlockedBy);

	void ReleaseLocks(const FGuid& Owner);

	FEntry* FindEntry(const FGuid& Id);

	/**
	 * Entries are held by pointer so a reference to one survives the map
	 * growing. Tool code can submit another task from inside its own Start(),
	 * and a TMap that rehashes moves its values.
	 */
	TMap<FGuid, TUniquePtr<FEntry>> Entries;

	struct FHeldLock
	{
		FGuid Owner;
		FUplinkResourceLock Lock;
	};

	/**
	 * Granted locks, one row per (task, resource). A flat array scanned in full
	 * is the right shape for a handful of live tasks; an index would cost more
	 * to maintain than it saves.
	 */
	TArray<FHeldLock> HeldLocks;

	/** Accepted but not yet started, oldest first. */
	TArray<FGuid> PendingStarts;

	/** Guards against starting a task from inside another task's start. */
	bool bPumping = false;

	FTSTicker::FDelegateHandle TickerHandle;

	/** Completed tasks are kept this long for polling, then purged. */
	static constexpr double ResultRetentionSeconds = 180.0;
};
