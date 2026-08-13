// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UplinkEventListener.generated.h"

class FUplinkEventRecorder;

/**
 * Generic delegate-capture target. FScriptDelegate::BindUFunction points a
 * watched delegate at UplinkEventStub (a real UFUNCTION, required because the
 * engine resolves the target with FindFunctionChecked at broadcast time), and
 * the ProcessEvent override intercepts the call before the stub runs, decoding
 * the parameter frame with the DELEGATE's signature function - so one stub
 * captures any delegate signature. One listener instance per watched delegate
 * (the stub name alone cannot tell two delegates apart).
 */
UCLASS()
class UUplinkEventListener : public UObject
{
	GENERATED_BODY()

public:
	void Init(FUplinkEventRecorder* InRecorder, const FGuid& InWatchId,
		UObject* InSource, FName InDelegateName, UFunction* InSignature);

	/** Detach from the recorder (e.g. recorder shutting down). */
	void Disarm() { Recorder = nullptr; }

	virtual void ProcessEvent(UFunction* Function, void* Parms) override;

	/** The one bindable stub; its body never runs for captured events. */
	UFUNCTION()
	void UplinkEventStub();

private:
	FUplinkEventRecorder* Recorder = nullptr;
	FGuid WatchId;
	TWeakObjectPtr<UObject> Source;
	FName DelegateName;
	TWeakObjectPtr<UFunction> Signature;
};
