// Copyright (c) 2026 Low Sze Hao. MIT License.

#include "UplinkEventRecorder.h"
#include "UplinkEditorModule.h"
#include "UplinkEventListener.h"

#include "Editor.h"
#include "UObject/UnrealType.h"

FUplinkEventRecorder::FUplinkEventRecorder()
{
	EndPieHandle = FEditorDelegates::EndPIE.AddRaw(this, &FUplinkEventRecorder::HandleEndPie);
}

FUplinkEventRecorder::~FUplinkEventRecorder()
{
	FEditorDelegates::EndPIE.Remove(EndPieHandle);
	StopAllWatches();
}

FGuid FUplinkEventRecorder::StartWatch(UObject* Object, const FString& DelegateName,
	TArray<FString>& OutBound, FString& OutError)
{
	check(IsInGameThread());

	if (!Object)
	{
		OutError = TEXT("no object to watch");
		return FGuid();
	}

	// Collect the delegate properties to bind.
	TArray<FMulticastDelegateProperty*> Properties;
	if (DelegateName == TEXT("*"))
	{
		for (TFieldIterator<FMulticastDelegateProperty> It(Object->GetClass()); It; ++It)
		{
			Properties.Add(*It);
		}
		if (Properties.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s has no multicast delegates"), *Object->GetClass()->GetName());
			return FGuid();
		}
	}
	else
	{
		FMulticastDelegateProperty* Property =
			FindFProperty<FMulticastDelegateProperty>(Object->GetClass(), *DelegateName);
		if (!Property)
		{
			TArray<FString> Available;
			for (TFieldIterator<FMulticastDelegateProperty> It(Object->GetClass()); It; ++It)
			{
				Available.Add(It->GetName());
			}
			OutError = FString::Printf(TEXT("delegate '%s' not found on %s. Available: %s"),
				*DelegateName, *Object->GetClass()->GetName(),
				Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)"));
			return FGuid();
		}
		Properties.Add(Property);
	}

	FWatch Watch;
	Watch.Description = FString::Printf(TEXT("%s : %s"), *Object->GetPathName(), *DelegateName);
	const FGuid WatchId = FGuid::NewGuid();

	for (FMulticastDelegateProperty* Property : Properties)
	{
		UUplinkEventListener* Listener = NewObject<UUplinkEventListener>(GetTransientPackage());
		Listener->Init(this, WatchId, Object, Property->GetFName(), Property->SignatureFunction);

		FScriptDelegate ScriptDelegate;
		ScriptDelegate.BindUFunction(Listener, GET_FUNCTION_NAME_CHECKED(UUplinkEventListener, UplinkEventStub));
		Property->AddDelegate(ScriptDelegate, Object);

		FBinding Binding;
		Binding.Listener = TStrongObjectPtr<UUplinkEventListener>(Listener);
		Binding.Source = Object;
		Binding.DelegateName = Property->GetFName();
		Watch.Bindings.Add(MoveTemp(Binding));
		OutBound.Add(Property->GetName());
	}

	Watches.Add(WatchId, MoveTemp(Watch));
	UE_LOG(LogUplink, Log, TEXT("watching %d delegate(s) on %s"), OutBound.Num(), *Object->GetPathName());
	return WatchId;
}

bool FUplinkEventRecorder::StopWatch(const FGuid& WatchId)
{
	check(IsInGameThread());
	FWatch* Watch = Watches.Find(WatchId);
	if (!Watch)
	{
		return false;
	}
	ReleaseWatch(*Watch);
	Watches.Remove(WatchId);
	return true;
}

void FUplinkEventRecorder::StopAllWatches()
{
	for (auto& Pair : Watches)
	{
		ReleaseWatch(Pair.Value);
	}
	Watches.Empty();
}

void FUplinkEventRecorder::ReleaseWatch(FWatch& Watch)
{
	for (FBinding& Binding : Watch.Bindings)
	{
		if (UObject* Source = Binding.Source.Get())
		{
			if (FMulticastDelegateProperty* Property =
				FindFProperty<FMulticastDelegateProperty>(Source->GetClass(), Binding.DelegateName))
			{
				FScriptDelegate ScriptDelegate;
				ScriptDelegate.BindUFunction(Binding.Listener.Get(),
					GET_FUNCTION_NAME_CHECKED(UUplinkEventListener, UplinkEventStub));
				Property->RemoveDelegate(ScriptDelegate, Source);
			}
		}
		if (Binding.Listener.IsValid())
		{
			Binding.Listener->Disarm();
		}
		Binding.Listener.Reset();
	}
}

void FUplinkEventRecorder::HandleEndPie(const bool bIsSimulating)
{
	if (Watches.Num() > 0)
	{
		UE_LOG(LogUplink, Log, TEXT("PIE ended - stopping %d event watch(es)"), Watches.Num());
		StopAllWatches();
	}
}

void FUplinkEventRecorder::Record(const FGuid& WatchId, const FString& ObjectPath,
	const FString& Delegate, TSharedPtr<FJsonObject> Payload)
{
	FEvent Event;
	Event.Seq = NextSeq++;
	Event.AtSeconds = FPlatformTime::Seconds();
	Event.WatchId = WatchId;
	Event.ObjectPath = ObjectPath;
	Event.Delegate = Delegate;
	Event.Payload = MoveTemp(Payload);
	Events.Add(MoveTemp(Event));

	if (Events.Num() > MaxBufferedEvents)
	{
		Events.RemoveAt(0, Events.Num() - MaxBufferedEvents);
	}
}

TArray<FUplinkEventRecorder::FEvent> FUplinkEventRecorder::Drain(
	int64 SinceSeq, const FGuid* WatchFilter, int32 MaxEvents) const
{
	TArray<FEvent> Out;
	for (const FEvent& Event : Events)
	{
		if (Event.Seq < SinceSeq)
		{
			continue;
		}
		if (WatchFilter && Event.WatchId != *WatchFilter)
		{
			continue;
		}
		Out.Add(Event);
	}
	if (Out.Num() > MaxEvents)
	{
		Out.RemoveAt(0, Out.Num() - MaxEvents);
	}
	return Out;
}

int32 FUplinkEventRecorder::EventCountForWatch(const FGuid& WatchId, int64 SinceSeq) const
{
	int32 Count = 0;
	for (const FEvent& Event : Events)
	{
		if (Event.Seq >= SinceSeq && Event.WatchId == WatchId)
		{
			++Count;
		}
	}
	return Count;
}

TArray<TPair<FGuid, FString>> FUplinkEventRecorder::ActiveWatches() const
{
	TArray<TPair<FGuid, FString>> Out;
	for (const auto& Pair : Watches)
	{
		Out.Emplace(Pair.Key, Pair.Value.Description);
	}
	return Out;
}
