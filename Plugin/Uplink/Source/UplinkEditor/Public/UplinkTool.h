// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UWorld;

/** What a tool invocation reports after Start()/Tick(). */
enum class EUplinkToolStep : uint8
{
	Done,     // Result is final
	Pending,  // Call Tick() again next editor tick
};

/** Final payload of a tool invocation. */
struct FUplinkToolResult
{
	bool bError = false;
	FString Message;
	TSharedPtr<FJsonObject> Data;

	/** Optional PNG payload (screenshots); serialized as an MCP image block. */
	TArray64<uint8> Png;

	static FUplinkToolResult Ok(TSharedPtr<FJsonObject> InData = nullptr, const FString& InMessage = FString())
	{
		FUplinkToolResult R;
		R.Data = InData;
		R.Message = InMessage;
		return R;
	}

	static FUplinkToolResult Error(const FString& InMessage)
	{
		FUplinkToolResult R;
		R.bError = true;
		R.Message = InMessage;
		return R;
	}
};

/**
 * One live world, as the worlds tool reports it and as the world parameter
 * names it.
 *
 * The editor keeps more worlds around than "editor or pie" can say: the level
 * being edited, one world per PIE instance when playing as several clients,
 * and a preview world behind every open asset editor. Addressing them by kind
 * alone means "pie" silently picks whichever instance the editor happens to be
 * playing in, so each world also gets an Id that names exactly one of them.
 */
struct FUplinkWorldEntry
{
	/** What to pass as world: "editor", "pie:0", "preview:World_3". */
	FString Id;

	/** EWorldType spelled for a reader: Editor, PIE, EditorPreview, Game, ... */
	FString Type;

	/** Standalone, DedicatedServer, ListenServer, Client. */
	FString NetMode;

	/** Short map name, PIE prefix stripped. */
	FString Map;

	/** INDEX_NONE unless this is a PIE instance. */
	int32 PieInstance = INDEX_NONE;

	/** The world an omitted world parameter resolves to right now. */
	bool bDefault = false;

	/**
	 * A world the game is running in rather than one being edited. Decides
	 * whether a write is wrapped in an editor transaction: the engine discards
	 * a transaction that only touched play objects, so opening one there buys
	 * nothing and confuses the undo stack.
	 */
	bool bPlayWorld = false;

	/** Simulate-In-Editor rather than Play: a PIE world with no player pawn. */
	bool bSimulating = false;

	UWorld* World = nullptr;
};

namespace UplinkWorlds
{
	/**
	 * Every live world, editor first, then PIE instances in order, then
	 * previews. Rebuilt per call for the same reason contexts do not cache a
	 * UWorld*: worlds appear and die between ticks.
	 */
	UPLINKEDITOR_API TArray<FUplinkWorldEntry> Enumerate();
}

/**
 * Per-invocation context. Worlds are resolved fresh on every call because a
 * UWorld* cached across ticks can die (PIE ending) between them.
 */
struct FUplinkToolContext
{
	TSharedPtr<FJsonObject> Params;

	/**
	 * "editor", "pie", an id from the worlds tool, or empty = PIE if active,
	 * else editor. "pie" means whichever world the editor is currently playing
	 * in; an id like "pie:1" means that instance and no other.
	 */
	FString WorldSpec;

	/** Resolve the target world, or null (with OutError set) if unavailable. */
	UPLINKEDITOR_API UWorld* ResolveWorld(FString& OutError) const;

	UPLINKEDITOR_API bool IsPieWorld() const;
};

/** Why an invocation is being stopped before it finished on its own. */
enum class EUplinkCancelReason : uint8
{
	Cancelled,  // a caller asked for it
	TimedOut,   // the deadline passed
	Shutdown,   // the module is going away
};

/**
 * One running execution of a tool. Quick tools finish inside Start(); latent
 * tools (screenshots, waits, PIE startup) return Pending and are Ticked once
 * per editor tick until Done or the task times out. All calls happen on the
 * game thread.
 */
class IUplinkInvocation
{
public:
	virtual ~IUplinkInvocation() = default;

	virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) = 0;

	virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out)
	{
		Out = FUplinkToolResult::Error(TEXT("Tool returned Pending but does not implement Tick"));
		return EUplinkToolStep::Done;
	}

	/**
	 * Stop early and let go of everything held.
	 *
	 * Marking a task "cancelled" or "timed out" only changes a status field.
	 * The invocation is what actually owns the in-flight work - a queued
	 * screenshot, a delegate waiting on PIE, a latent editor request - and
	 * without a way to tell it, the caller gets an answer while the operation
	 * carries on writing to the editor behind it.
	 *
	 * Called exactly once, on the game thread, before the invocation is
	 * released, and Tick() never runs again afterwards. So the invariant is
	 * narrow but absolute: **once this returns, nothing the tool started may
	 * still mutate the editor.** Unhook delegates, drop callbacks, abandon
	 * pending requests. A tool that only computes synchronously on the game
	 * thread has nothing to release and correctly inherits the default.
	 */
	virtual void Cancel(EUplinkCancelReason Reason) {}

	/**
	 * False for work that cannot be interrupted safely once begun. The task
	 * manager reports the refusal instead of claiming a stop that did not
	 * happen; the task then still ends on its own deadline.
	 */
	virtual bool CanCancel() const { return true; }
};

/** Static description of a tool, served to MCP clients. */
struct FUplinkToolInfo
{
	FString Name;
	FString Description;
	TSharedPtr<FJsonObject> InputSchema;
	bool bReadOnly = false;

	/**
	 * Whether this tool's work belongs in an editor transaction.
	 *
	 * Defaults to true for anything that writes, which is right for asset and
	 * actor edits. It must be false for tools that drive the session rather
	 * than edit it: starting PIE makes the engine cancel whatever transaction
	 * is open ("Cancelling Open Transaction 'Uplink: pie_start'"), so wrapping
	 * those means fighting it for no benefit - none of it is undoable anyway.
	 */
	bool bTransactional = true;

	/**
	 * Risk traits, published to clients so an agent can weigh a call before
	 * making it rather than discovering the cost afterwards. `bReadOnly` alone
	 * flattens "reads the world" and "deletes your level" into one bit.
	 *
	 * These are set from the single table in UplinkToolTraits.cpp, not at the
	 * registration sites - one auditable list of every tool that can do real
	 * damage beats the same fact scattered across twenty files.
	 */

	/** Destroys or overwrites work in a way undo does not reliably bring back. */
	bool bDestructive = false;

	/**
	 * Runs whatever the caller names rather than a fixed operation:
	 * call_function reaches any UFUNCTION, console_command any cvar or exec.
	 * The blast radius is not this tool's parameters, it is the engine.
	 */
	bool bArbitraryExecution = false;

	/** Needs a live PIE session; meaningless in the editor world. */
	bool bRequiresPie = false;

	/** Routinely takes many seconds - slowness here is not a hang. */
	bool bLongRunning = false;

	double TimeoutSeconds = 30.0;
};

/** Registry entry: description + factory for fresh invocations. */
struct FUplinkToolDef
{
	FUplinkToolInfo Info;
	TFunction<TSharedRef<IUplinkInvocation>()> Factory;
};
