// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkEventListener.h"
#include "UplinkEventRecorder.h"
#include "JsonObjectConverter.h"

void UUplinkEventListener::Init(FUplinkEventRecorder* InRecorder, const FGuid& InWatchId,
	UObject* InSource, FName InDelegateName, UFunction* InSignature)
{
	Recorder = InRecorder;
	WatchId = InWatchId;
	Source = InSource;
	DelegateName = InDelegateName;
	Signature = InSignature;
}

void UUplinkEventListener::ProcessEvent(UFunction* Function, void* Parms)
{
	if (Recorder && Function && Function->GetFName() == GET_FUNCTION_NAME_CHECKED(UUplinkEventListener, UplinkEventStub))
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		if (UFunction* SignatureFunction = Signature.Get())
		{
			// The frame is laid out by the delegate's signature - decode it
			// with that signature, not with the stub's (empty) one.
			FJsonObjectConverter::UStructToJsonObject(
				SignatureFunction, Parms, Payload.ToSharedRef(), CPF_Parm, 0);
		}
		Recorder->Record(WatchId,
			Source.IsValid() ? Source->GetPathName() : TEXT("<gone>"),
			DelegateName.ToString(), Payload);
		return; // never run the stub body for captured events
	}

	Super::ProcessEvent(Function, Parms);
}

void UUplinkEventListener::UplinkEventStub()
{
	// Intentionally empty - ProcessEvent intercepts before this runs.
}
