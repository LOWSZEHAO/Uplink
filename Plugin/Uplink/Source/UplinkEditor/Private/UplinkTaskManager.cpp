// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkTaskManager.h"
#include "UplinkEditorModule.h"
#include "Misc/App.h"
#include "Editor.h"

namespace
{
	FString KindToString(EUplinkResourceKind Kind)
	{
		switch (Kind)
		{
		case EUplinkResourceKind::Editor:    return TEXT("the editor");
		case EUplinkResourceKind::World:     return TEXT("the world");
		case EUplinkResourceKind::Pie:       return TEXT("the PIE session");
		case EUplinkResourceKind::Asset:     return TEXT("asset");
		case EUplinkResourceKind::Blueprint: return TEXT("blueprint");
		}
		return TEXT("unknown resource");
	}

	/**
	 * "/Game/Foo.Foo" and "/Game/Foo" name one asset, and a caller may spell it
	 * either way in the same session. Two spellings that compare unequal are two
	 * resources that never conflict, which is a lock that quietly does nothing.
	 */
	FString NormalizeAssetKey(const FString& In)
	{
		FString Path = In.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), Dot))
		{
			Path = Path.Left(Dot);
		}
		return Path.ToLower();
	}

	FUplinkResourceLock MakeLock(EUplinkResourceKind Kind, const FString& Key, bool bExclusive)
	{
		FUplinkResourceLock Lock;
		Lock.Kind = Kind;
		Lock.Key = (Kind == EUplinkResourceKind::Asset || Kind == EUplinkResourceKind::Blueprint)
			? NormalizeAssetKey(Key) : FString();
		Lock.bExclusive = bExclusive;
		return Lock;
	}

	/** True when Outer's scope wholly contains Inner's. */
	bool Contains(const FUplinkResourceLock& Outer, const FUplinkResourceLock& Inner)
	{
		switch (Outer.Kind)
		{
		case EUplinkResourceKind::Editor:
			return true;
		case EUplinkResourceKind::World:
			return Inner.Kind == EUplinkResourceKind::World || Inner.Kind == EUplinkResourceKind::Pie;
		case EUplinkResourceKind::Pie:
			return Inner.Kind == EUplinkResourceKind::Pie;
		case EUplinkResourceKind::Asset:
			return (Inner.Kind == EUplinkResourceKind::Asset || Inner.Kind == EUplinkResourceKind::Blueprint)
				&& Inner.Key == Outer.Key;
		case EUplinkResourceKind::Blueprint:
			return Inner.Kind == EUplinkResourceKind::Blueprint && Inner.Key == Outer.Key;
		}

		// A kind nobody taught this function about conflicts with everything
		// rather than with nothing: a new resource that is silently compatible
		// with all the others is worse than one that is too strict and noticed.
		return true;
	}

	bool Conflicts(const FUplinkResourceLock& A, const FUplinkResourceLock& B)
	{
		if (!A.bExclusive && !B.bExclusive)
		{
			return false;
		}
		return Contains(A, B) || Contains(B, A);
	}

	/**
	 * Tools that only read or poke the task manager itself. Locking these would
	 * be a deadlock by construction: task_cancel would queue behind the very
	 * task it exists to cancel, and task_status would stop answering exactly
	 * when a caller most needs to know what is going on.
	 */
	bool IsTaskControlTool(const FString& ToolName)
	{
		return ToolName == TEXT("task_status")
			|| ToolName == TEXT("task_result")
			|| ToolName == TEXT("task_cancel")
			|| ToolName == TEXT("task_list");
	}
}

FString FUplinkResourceLock::Describe() const
{
	const TCHAR* Mode = bExclusive ? TEXT("exclusive") : TEXT("shared");
	return Key.IsEmpty()
		? FString::Printf(TEXT("%s (%s)"), *KindToString(Kind), Mode)
		: FString::Printf(TEXT("%s %s (%s)"), *KindToString(Kind), *Key, Mode);
}

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
		TickerHandle.Reset();
	}

	// Dropping the map is not enough. A latent invocation is the thing that
	// holds the editor delegate, the queued screenshot, the PIE callback - let
	// it die unasked and that work outlives the module that owns the code it
	// calls back into. Every live invocation hears Shutdown first, and any
	// transaction still open is closed while GEditor is still there to close it.
	TArray<FGuid> Ids;
	Entries.GetKeys(Ids);
	for (const FGuid& Id : Ids)
	{
		FEntry* Entry = FindEntry(Id);
		if (!Entry || IsTerminal(Entry->Task.Status))
		{
			continue;
		}
		ReleaseEntry(*Entry, EUplinkCancelReason::Shutdown);
		Entry->Task.Status = EUplinkTaskStatus::Cancelled;
		Entry->Task.FinishedAt = FPlatformTime::Seconds();
	}

	// Waiters are dropped rather than fired. They are HTTP completion callbacks
	// and the server that owns them is already gone by the time this runs.
	Entries.Empty();
	HeldLocks.Empty();
	PendingStarts.Empty();
}

FGuid FUplinkTaskManager::Submit(
	const FString& ToolName,
	TSharedRef<IUplinkInvocation> Invocation,
	FUplinkToolContext Context,
	double TimeoutSeconds,
	bool bReadOnly,
	bool bTransactional)
{
	check(IsInGameThread());

	TUniquePtr<FEntry> Entry = MakeUnique<FEntry>();
	Entry->Task.Id = FGuid::NewGuid();
	Entry->Task.ToolName = ToolName;
	Entry->Task.StartedAt = FPlatformTime::Seconds();
	Entry->Task.TimeoutSeconds = TimeoutSeconds;
	Entry->Invocation = Invocation;
	Entry->Context = MoveTemp(Context);
	Entry->bReadOnly = bReadOnly;
	Entry->bTransactional = bTransactional;
	Entry->Resources = DeriveResources(ToolName, bReadOnly, Entry->Context);

	const FGuid Id = Entry->Task.Id;
	Entries.Add(Id, MoveTemp(Entry));
	PendingStarts.Add(Id);

	// The queue is normally empty and the locks normally free, so this still
	// runs the first step on the submitting tick and quick tools still answer
	// immediately. When it does not, the task starts as soon as what it needs
	// frees up, and the caller gets a task id to poll in the meantime.
	PumpPending();
	return Id;
}

void FUplinkTaskManager::Await(const FGuid& TaskId, double MaxWaitSeconds, FWaiter Waiter)
{
	check(IsInGameThread());

	FEntry* Entry = FindEntry(TaskId);
	if (!Entry)
	{
		FUplinkTask Ghost;
		Ghost.Id = TaskId;
		Ghost.Status = EUplinkTaskStatus::Failed;
		Ghost.Result = FUplinkToolResult::Error(TEXT("unknown task id (results are retained ~3 minutes)"));
		Waiter(Ghost, false);
		return;
	}

	if (IsTerminal(Entry->Task.Status))
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
	const TUniquePtr<FEntry>* Entry = Entries.Find(TaskId);
	return Entry ? &(*Entry)->Task : nullptr;
}

EUplinkCancelOutcome FUplinkTaskManager::RequestCancel(const FGuid& TaskId, FString& OutMessage)
{
	check(IsInGameThread());

	FEntry* Entry = FindEntry(TaskId);
	if (!Entry)
	{
		OutMessage = TEXT("unknown task id (results are retained ~3 minutes). Use task_list to see live ones.");
		return EUplinkCancelOutcome::UnknownTask;
	}

	if (IsTerminal(Entry->Task.Status))
	{
		OutMessage = FString::Printf(
			TEXT("task already finished as '%s'; read it with task_result"),
			*StatusToString(Entry->Task.Status));
		return EUplinkCancelOutcome::AlreadyEnded;
	}

	// CanCancel is asked only of a task that has started. One still queued
	// behind someone else's lock owns nothing and can always be dropped, and
	// its invocation is never told to stop work it never began.
	if (Entry->bStarted && Entry->Invocation.IsValid() && !Entry->Invocation->CanCancel())
	{
		const double Remaining = FMath::Max(
			0.0, Entry->Task.StartedAt + Entry->Task.TimeoutSeconds - FPlatformTime::Seconds());

		OutMessage = FString::Printf(
			TEXT("'%s' reports it cannot be interrupted safely; the task is still running and ends on its own deadline in %.0fs. Poll it with task_status."),
			*Entry->Task.ToolName, Remaining);

		// Say the same thing through task_result. The status alone is a word,
		// and a caller who asked for a stop and did not get one deserves the
		// sentence rather than having to infer it.
		Entry->Task.Status = EUplinkTaskStatus::Cancelling;
		Entry->Task.Result = FUplinkToolResult::Error(OutMessage);
		return EUplinkCancelOutcome::Refused;
	}

	Entry->Task.Result = FUplinkToolResult::Error(TEXT("cancelled"));
	FinishEntry(*Entry, EUplinkTaskStatus::Cancelled, EUplinkCancelReason::Cancelled);
	OutMessage = TEXT("cancelled");
	return EUplinkCancelOutcome::Stopped;
}

bool FUplinkTaskManager::Cancel(const FGuid& TaskId)
{
	// Kept for call sites that predate RequestCancel. A refusal reads as false
	// here, the same as an unknown id, which is the reason to move off it.
	FString Message;
	return RequestCancel(TaskId, Message) == EUplinkCancelOutcome::Stopped;
}

TArray<const FUplinkTask*> FUplinkTaskManager::Snapshot() const
{
	TArray<const FUplinkTask*> Out;
	for (const TPair<FGuid, TUniquePtr<FEntry>>& Pair : Entries)
	{
		Out.Add(&Pair.Value->Task);
	}
	return Out;
}

FString FUplinkTaskManager::StatusToString(EUplinkTaskStatus Status)
{
	switch (Status)
	{
	case EUplinkTaskStatus::Running:    return TEXT("running");
	case EUplinkTaskStatus::Cancelling: return TEXT("cancelling");
	case EUplinkTaskStatus::Completed:  return TEXT("completed");
	case EUplinkTaskStatus::Failed:     return TEXT("failed");
	case EUplinkTaskStatus::Cancelled:  return TEXT("cancelled");
	case EUplinkTaskStatus::TimedOut:   return TEXT("timed_out");
	}
	return TEXT("unknown");
}

bool FUplinkTaskManager::IsTerminal(EUplinkTaskStatus Status)
{
	return Status != EUplinkTaskStatus::Running && Status != EUplinkTaskStatus::Cancelling;
}

bool FUplinkTaskManager::TickTasks(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();

	// Walk a copy of the keys. Stepping a task runs tool code, and tool code
	// can submit another task - adding to a TMap invalidates its iterators, and
	// an iterator is the one thing heap-held entries do not protect. Nothing in
	// the tree submits from inside a step today; this is the price of not
	// having to keep proving that.
	TArray<FGuid> Ids;
	Entries.GetKeys(Ids);

	TArray<FGuid> ToPurge;
	for (const FGuid& Id : Ids)
	{
		FEntry* Found = FindEntry(Id);
		if (!Found)
		{
			continue;
		}
		FEntry& Entry = *Found;

		if (!IsTerminal(Entry.Task.Status))
		{
			if (Now - Entry.Task.StartedAt > Entry.Task.TimeoutSeconds)
			{
				if (!Entry.bStarted)
				{
					const FString Blocker = Entry.Task.WaitingOn.IsEmpty()
						? TEXT("the resources it needs") : Entry.Task.WaitingOn;
					Entry.Task.Result = FUplinkToolResult::Error(FString::Printf(
						TEXT("tool '%s' timed out after %.0fs without starting: it was waiting for %s. Stop the blocking task with task_cancel, or resubmit with a larger timeout_s."),
						*Entry.Task.ToolName, Entry.Task.TimeoutSeconds, *Blocker));
				}
				else if (Entry.Task.Status == EUplinkTaskStatus::Cancelling)
				{
					Entry.Task.Result = FUplinkToolResult::Error(FString::Printf(
						TEXT("cancel requested, but '%s' could not be interrupted; stopped when its %.0fs deadline passed"),
						*Entry.Task.ToolName, Entry.Task.TimeoutSeconds));
				}
				else
				{
					Entry.Task.Result = FUplinkToolResult::Error(FString::Printf(
						TEXT("tool '%s' timed out after %.0fs"), *Entry.Task.ToolName, Entry.Task.TimeoutSeconds));
				}

				// A refused cancel still ends as cancelled: the caller asked for
				// this, the deadline only decided when. Either way the
				// invocation is told to stop before it is let go, so a task the
				// caller has been told is over cannot still be writing to the
				// editor.
				const bool bWasCancelling = Entry.Task.Status == EUplinkTaskStatus::Cancelling;
				FinishEntry(
					Entry,
					bWasCancelling ? EUplinkTaskStatus::Cancelled : EUplinkTaskStatus::TimedOut,
					bWasCancelling ? EUplinkCancelReason::Cancelled : EUplinkCancelReason::TimedOut);
			}
			else
			{
				// Queued tasks are started by the pump, never here: starting is
				// where locks are taken, and that decision belongs in one place.
				if (Entry.bStarted)
				{
					StepEntry(Entry);
				}
				FlushWaiters(Entry, /*bDeadlinesOnly=*/true);
			}
		}
		else if (Entry.Waiters.Num() == 0 && Now - Entry.Task.FinishedAt > ResultRetentionSeconds)
		{
			ToPurge.Add(Id);
		}
	}

	for (const FGuid& Id : ToPurge)
	{
		Entries.Remove(Id);
	}

	// Last, so a task that finished this tick has already given its locks back
	// and whoever was waiting on them starts now rather than a frame later.
	PumpPending();
	return true; // keep ticking
}

void FUplinkTaskManager::PumpPending()
{
	if (bPumping)
	{
		// Re-entered from a tool's Start(). The outer walk will reach anything
		// appended behind it, and anything it has already passed starts on the
		// next tick. Beginning a task from inside another task's first step is
		// not worth what it costs to reason about.
		return;
	}
	TGuardValue<bool> Guard(bPumping, true);

	// Strict submission order among tasks that need something: letting a
	// compatible task overtake a blocked one would starve exclusive work behind
	// a steady trickle of shared readers, and "it ran eventually" is not
	// something a caller can plan around.
	//
	// A task that needs nothing is never held up by the queue, whatever is
	// stuck ahead of it. The task tools have to keep answering while everything
	// else waits, or the way out of a jam sits behind the jam.
	bool bBlocked = false;
	FString BlockedTool;

	for (int32 i = 0; i < PendingStarts.Num(); )
	{
		const FGuid Id = PendingStarts[i];
		FEntry* Found = FindEntry(Id);
		if (!Found || IsTerminal(Found->Task.Status))
		{
			// Cancelled or timed out while queued. Cancel deliberately leaves
			// the id here rather than editing this array from underneath a pump.
			PendingStarts.RemoveAt(i);
			continue;
		}

		if (bBlocked && Found->Resources.Num() > 0)
		{
			Found->Task.bWaiting = true;
			Found->Task.WaitingOn = FString::Printf(
				TEXT("the tasks queued ahead of it, starting with '%s'"), *BlockedTool);
			++i;
			continue;
		}

		FString BlockedBy;
		if (!TryAcquire(Id, Found->Resources, BlockedBy))
		{
			Found->Task.bWaiting = true;
			Found->Task.WaitingOn = BlockedBy;
			bBlocked = true;
			BlockedTool = Found->Task.ToolName;
			++i;
			continue;
		}

		Found->bHoldsLocks = true;
		Found->Task.bWaiting = false;
		Found->Task.WaitingOn.Reset();
		PendingStarts.RemoveAt(i);

		// Safe to keep using Found across this: entries live on the heap, so
		// tool code that submits another task cannot move this one.
		StepEntry(*Found);
	}
}

void FUplinkTaskManager::StepEntry(FEntry& Entry)
{
	if (IsTerminal(Entry.Task.Status) || !Entry.Invocation.IsValid())
	{
		return;
	}

	FUplinkToolResult Out;
	EUplinkToolStep Step;

	// Hold a reference of our own for the duration of the call. A waiter firing
	// during the step can reach back in and finish this task, which drops the
	// entry's reference - and that is the object whose code we are inside.
	const TSharedPtr<IUplinkInvocation> Invocation = Entry.Invocation;

	if (!Entry.bStarted)
	{
		Entry.bStarted = true;

		// Group everything a mutating tool touches into one undo step, so the
		// user can ctrl+Z an agent's edit the same way they undo their own.
		// The transaction spans the whole invocation (latent tools included)
		// and is always closed in FinishEntry, which every terminal path runs
		// through.
		//
		// Nothing transacts while a session is live: changes to a running game
		// are not undoable editor edits, the engine discards transactions that
		// only touch PIE objects, and holding one open across play is asking
		// for trouble. Tools that START a session opt out explicitly with
		// bTransactional - the engine cancels any open transaction to begin
		// PIE, so wrapping those means fighting it.
		const bool bSessionLive = GEditor && GEditor->PlayWorld;
		if (!Entry.bReadOnly && Entry.bTransactional && GEditor
			&& !bSessionLive && !Entry.Context.IsPieWorld())
		{
			GEditor->BeginTransaction(FText::FromString(
				FString::Printf(TEXT("Uplink: %s"), *Entry.Task.ToolName)));
			Entry.bTransactionOpen = true;
		}

		Step = Invocation->Start(Entry.Context, Out);
	}
	else
	{
		Step = Invocation->Tick(Entry.Context, Out);
	}

	// If the task ended during the call, that ending is the real one; a result
	// produced after the fact does not get to overwrite it.
	if (IsTerminal(Entry.Task.Status))
	{
		return;
	}

	if (Step == EUplinkToolStep::Done)
	{
		Entry.Task.Result = MoveTemp(Out);
		FinishEntry(Entry, Entry.Task.Result.bError ? EUplinkTaskStatus::Failed : EUplinkTaskStatus::Completed);
	}
}

void FUplinkTaskManager::FinishEntry(
	FEntry& Entry, EUplinkTaskStatus Status, TOptional<EUplinkCancelReason> StopReason)
{
	if (IsTerminal(Entry.Task.Status))
	{
		// A cancel racing a deadline on the same tick reaches here twice. The
		// first one is the one that counts.
		return;
	}

	ReleaseEntry(Entry, StopReason);

	Entry.Task.Status = Status;
	Entry.Task.FinishedAt = FPlatformTime::Seconds();
	FlushWaiters(Entry, /*bDeadlinesOnly=*/false);
}

void FUplinkTaskManager::ReleaseEntry(FEntry& Entry, TOptional<EUplinkCancelReason> StopReason)
{
	// Close the undo group before anyone is told the task ended. An empty
	// transaction (a tool that turned out to change nothing) is dropped by
	// the engine, so this does not litter the undo stack.
	if (Entry.bTransactionOpen)
	{
		Entry.bTransactionOpen = false;
		if (GEditor)
		{
			GEditor->EndTransaction();
		}
	}

	// Tell the invocation to stop before letting go of it, and only when the
	// task is being stopped early - a tool that returned Done has already
	// finished and has nothing to unwind. An invocation that never started has
	// nothing either; telling it to stop would only invite tools to defend
	// against a call that means nothing.
	const TSharedPtr<IUplinkInvocation> Invocation = Entry.Invocation;
	Entry.Invocation.Reset();

	if (StopReason.IsSet() && Entry.bStarted && !Entry.bCancelDelivered && Invocation.IsValid())
	{
		Entry.bCancelDelivered = true;
		Invocation->Cancel(StopReason.GetValue());
	}

	if (Entry.bHoldsLocks)
	{
		Entry.bHoldsLocks = false;
		ReleaseLocks(Entry.Task.Id);
	}
	Entry.Task.bWaiting = false;
	Entry.Task.WaitingOn.Reset();
}

void FUplinkTaskManager::FlushWaiters(FEntry& Entry, bool bDeadlinesOnly)
{
	const double Now = FPlatformTime::Seconds();
	const bool bRunning = !IsTerminal(Entry.Task.Status);

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

TArray<FUplinkResourceLock> FUplinkTaskManager::DeriveResources(
	const FString& ToolName, bool bReadOnly, const FUplinkToolContext& Ctx)
{
	// Guessed, not declared. Submit is handed a tool name, the read-only flag
	// and the params, and nothing else - so this reads what those three can
	// honestly support and is deliberately coarse everywhere else. The next
	// step is for FUplinkToolInfo to carry the resources a tool actually
	// touches, at which point this function becomes a fallback for tools that
	// have not declared any, and locks get as narrow as the tools are.
	//
	// The default is asymmetric on purpose, because the safe answer differs by
	// direction:
	//
	//   A write we do not recognise takes the editor exclusively. Coarse, and
	//   it serialises writers against each other, which is exactly the
	//   interleaving worth preventing - two tasks mid-edit on one Blueprint, or
	//   a session starting under an asset edit.
	//
	//   A read we cannot pin down takes nothing at all. A read cannot corrupt
	//   anything, and the editor it observes mid-edit is the same editor a
	//   person sees mid-edit. Queueing every unattributable read behind every
	//   write would stall polling and screenshots behind a ten-minute
	//   input_replay - and watching a session while something else drives it is
	//   the workflow the whole async design exists for. A read that genuinely
	//   must not see a half-finished edit says so below, by naming the asset.
	TArray<FUplinkResourceLock> Out;

	if (IsTaskControlTool(ToolName))
	{
		return Out;
	}

	if (bReadOnly)
	{
		// Shared, so reads of one asset never wait on each other, and narrow, so
		// reading a Blueprint is not held up by a session starting - the two
		// have nothing to do with each other. An exclusive holder of the whole
		// editor still excludes this, through containment.
		//
		// 'path' is not read here: several tools use it for a directory to
		// search, and a lock on a folder-shaped string that only looks like an
		// asset would be a lock on nothing.
		FString Named;
		if (Ctx.Params.IsValid()
			&& Ctx.Params->TryGetStringField(FStringView(TEXT("blueprint")), Named) && !Named.IsEmpty())
		{
			Out.Add(MakeLock(EUplinkResourceKind::Blueprint, Named, /*bExclusive=*/false));
		}
		else if (Ctx.Params.IsValid()
			&& Ctx.Params->TryGetStringField(FStringView(TEXT("asset")), Named) && !Named.IsEmpty())
		{
			Out.Add(MakeLock(EUplinkResourceKind::Asset, Named, /*bExclusive=*/false));
		}
		return Out;
	}

	// pie_start and pie_stop replace the world; pie_pause, pie_resume and
	// pie_step poke the one already running. Both conflict with an exclusive
	// hold on the editor, so session control still serialises against asset
	// edits without either side naming the other.
	if (ToolName == TEXT("pie_start") || ToolName == TEXT("pie_stop"))
	{
		Out.Add(MakeLock(EUplinkResourceKind::Pie, FString(), /*bExclusive=*/true));
		return Out;
	}
	if (ToolName.StartsWith(TEXT("pie_")))
	{
		Out.Add(MakeLock(EUplinkResourceKind::World, FString(), /*bExclusive=*/true));
		return Out;
	}

	Out.Add(MakeLock(EUplinkResourceKind::Editor, FString(), /*bExclusive=*/true));
	return Out;
}

bool FUplinkTaskManager::TryAcquire(
	const FGuid& Owner, const TArray<FUplinkResourceLock>& Wanted, FString& OutBlockedBy)
{
	// All of them or none of them. A task that took what it could and waited
	// for the rest would be holding one resource while wanting another, which
	// is the only ingredient a deadlock needs here.
	for (const FUplinkResourceLock& Want : Wanted)
	{
		for (const FHeldLock& Held : HeldLocks)
		{
			if (Held.Owner == Owner || !Conflicts(Want, Held.Lock))
			{
				continue;
			}

			const FEntry* Holder = FindEntry(Held.Owner);
			OutBlockedBy = FString::Printf(TEXT("%s, held by '%s'"),
				*Held.Lock.Describe(), Holder ? *Holder->Task.ToolName : TEXT("another task"));
			return false;
		}
	}

	for (const FUplinkResourceLock& Want : Wanted)
	{
		FHeldLock Held;
		Held.Owner = Owner;
		Held.Lock = Want;
		HeldLocks.Add(MoveTemp(Held));
	}
	return true;
}

void FUplinkTaskManager::ReleaseLocks(const FGuid& Owner)
{
	HeldLocks.RemoveAll([&Owner](const FHeldLock& Held) { return Held.Owner == Owner; });
}

FUplinkTaskManager::FEntry* FUplinkTaskManager::FindEntry(const FGuid& Id)
{
	TUniquePtr<FEntry>* Entry = Entries.Find(Id);
	return Entry ? Entry->Get() : nullptr;
}
