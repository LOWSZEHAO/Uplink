// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/**
 * Ring buffer over GLog so tools can read recent output without touching log
 * files. Serialize() can be called from any thread; the buffer is lock-guarded.
 */
class FUplinkLogCapture final : public FOutputDevice
{
public:
	struct FLine
	{
		int64 Index = 0;
		double AtSeconds = 0.0;
		FName Category;
		ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
		FString Text;
	};

	FUplinkLogCapture();
	virtual ~FUplinkLogCapture() override;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	/** Lines with Index >= SinceIndex, newest last, capped at MaxLines. */
	TArray<FLine> Get(int64 SinceIndex, int32 MaxLines, const FString& CategoryFilter,
		const FString& ContainsFilter, ELogVerbosity::Type MinVerbosity) const;

	int64 NewestIndex() const;

private:
	mutable FCriticalSection Lock;
	TArray<FLine> Buffer;
	int64 NextIndex = 0;
	static constexpr int32 MaxBuffered = 6000;
};
