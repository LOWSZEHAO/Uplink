// Copyright (c) 2026 Low Sze Hao. MIT License.

#include "UplinkLogCapture.h"

FUplinkLogCapture::FUplinkLogCapture()
{
	if (GLog)
	{
		GLog->AddOutputDevice(this);
	}
}

FUplinkLogCapture::~FUplinkLogCapture()
{
	if (GLog)
	{
		GLog->RemoveOutputDevice(this);
	}
}

void FUplinkLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock Guard(&Lock);

	FLine Line;
	Line.Index = NextIndex++;
	Line.AtSeconds = FPlatformTime::Seconds();
	Line.Category = Category;
	Line.Verbosity = Verbosity;
	Line.Text = V;
	Buffer.Add(MoveTemp(Line));

	if (Buffer.Num() > MaxBuffered)
	{
		Buffer.RemoveAt(0, Buffer.Num() - MaxBuffered);
	}
}

TArray<FUplinkLogCapture::FLine> FUplinkLogCapture::Get(int64 SinceIndex, int32 MaxLines,
	const FString& CategoryFilter, const FString& ContainsFilter, ELogVerbosity::Type MinVerbosity) const
{
	FScopeLock Guard(&Lock);

	TArray<FLine> Out;
	for (const FLine& Line : Buffer)
	{
		if (Line.Index < SinceIndex)
		{
			continue;
		}
		if (Line.Verbosity > MinVerbosity) // lower enum value = more severe
		{
			continue;
		}
		if (!CategoryFilter.IsEmpty() && !Line.Category.ToString().Contains(CategoryFilter))
		{
			continue;
		}
		if (!ContainsFilter.IsEmpty() && !Line.Text.Contains(ContainsFilter))
		{
			continue;
		}
		Out.Add(Line);
	}

	if (Out.Num() > MaxLines)
	{
		Out.RemoveAt(0, Out.Num() - MaxLines); // keep the newest
	}
	return Out;
}

int64 FUplinkLogCapture::NewestIndex() const
{
	FScopeLock Guard(&Lock);
	return NextIndex;
}
