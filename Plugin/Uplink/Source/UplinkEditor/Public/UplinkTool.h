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
 * Per-invocation context. Worlds are resolved fresh on every call because a
 * UWorld* cached across ticks can die (PIE ending) between them.
 */
struct FUplinkToolContext
{
	TSharedPtr<FJsonObject> Params;

	/** "editor", "pie", or empty = PIE if active, else editor. */
	FString WorldSpec;

	/** Resolve the target world, or null (with OutError set) if unavailable. */
	UPLINKEDITOR_API UWorld* ResolveWorld(FString& OutError) const;

	UPLINKEDITOR_API bool IsPieWorld() const;
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
};

/** Static description of a tool, served to MCP clients. */
struct FUplinkToolInfo
{
	FString Name;
	FString Description;
	TSharedPtr<FJsonObject> InputSchema;
	bool bReadOnly = false;
	double TimeoutSeconds = 30.0;
};

/** Registry entry: description + factory for fresh invocations. */
struct FUplinkToolDef
{
	FUplinkToolInfo Info;
	TFunction<TSharedRef<IUplinkInvocation>()> Factory;
};
