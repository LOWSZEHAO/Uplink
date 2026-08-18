// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkValueConverter.h"

#include "UplinkCompat.h"
#include "UplinkToolUtil.h"

#include "Engine/World.h"
#include "JsonObjectConverter.h"
#include "UObject/UnrealType.h"

namespace UplinkValue
{
	TSharedPtr<FJsonValue> PropertyToJson(FProperty* Property, const void* ValueAddr)
	{
		if (const FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Referenced = AsObject->GetObjectPropertyValue(ValueAddr);
			return MakeShared<FJsonValueString>(Referenced ? Referenced->GetPathName() : FString());
		}
		return FJsonObjectConverter::UPropertyToJsonValue(Property, ValueAddr);
	}

	bool JsonToProperty(
		const TSharedPtr<FJsonValue>& Value,
		FProperty* Property,
		void* ValueAddr,
		FString& OutError)
	{
		if (!FJsonObjectConverter::JsonValueToUProperty(Value, Property, ValueAddr))
		{
			OutError = FString::Printf(
				TEXT("could not convert JSON to %s (%s)"), *Property->GetName(), *Property->GetCPPType());
			return false;
		}

		// A path that resolves to nothing would otherwise be written as null
		// and reported as a success - this is how a material ended up with no
		// parent and rendered black with nothing in the logs.
		return ResolveObjectReference(Property, Value, ValueAddr, OutError);
	}

	bool ResolveObjectReference(
		const FProperty* Property,
		const TSharedPtr<FJsonValue>& Supplied,
		const void* ValueAddr,
		FString& OutError)
	{
		return UplinkToolUtil::NamedObjectResolved(Property, Supplied, ValueAddr, OutError);
	}

	bool JsonToFunctionParams(
		UFunction* Function,
		const TSharedPtr<FJsonObject>& Args,
		void* Frame,
		const FString& FunctionLabel,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> ArgObject = Args.IsValid() ? Args.ToSharedRef() : MakeShared<FJsonObject>();

		// Reject arg names that match no parameter - otherwise a typo'd arg
		// silently becomes a zero-default (found during live testing).
		TArray<FString> ParamNames;
		for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
		{
			if (ParamIt->HasAnyPropertyFlags(CPF_Parm) && !ParamIt->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ParamNames.Add(ParamIt->GetName());
			}
		}
		for (const auto& ArgPair : ArgObject->Values)
		{
			const FString ArgName = UplinkCompat::JsonKeyToString(ArgPair.Key);
			const bool bKnown = ParamNames.ContainsByPredicate([&ArgName](const FString& ParamName)
			{
				return ParamName.Equals(ArgName, ESearchCase::IgnoreCase);
			});
			if (!bKnown)
			{
				OutError = FString::Printf(
					TEXT("unknown arg '%s' for %s. Expected parameters: %s"),
					*ArgName, *FunctionLabel, *FString::Join(ParamNames, TEXT(", ")));
				return false;
			}
		}

		// A parameter frame starts zeroed, which is NOT what a C++ default
		// argument means, and the difference is silent. Calling
		// SetMaterialInstanceScalarParameterValue without naming its
		// Association parameter gave it 0 - LayerParameter - where the
		// declared default is GlobalParameter; the call then matched no
		// parameter, returned false, and logged nothing. The engine keeps
		// each default as CPP_Default_<Name> metadata on the function, which
		// is the same source the Blueprint editor seeds its pins from.
		for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
		{
			if (!ParamIt->HasAnyPropertyFlags(CPF_Parm)
				|| ParamIt->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
			{
				continue;
			}
			const FString ParamName = ParamIt->GetName();
			bool bSupplied = false;
			for (const auto& ArgPair : ArgObject->Values)
			{
				if (UplinkCompat::JsonKeyToString(ArgPair.Key).Equals(ParamName, ESearchCase::IgnoreCase))
				{
					bSupplied = true;
					break;
				}
			}
			if (bSupplied)
			{
				continue;
			}
			const FString DefaultText = Function->GetMetaData(FName(*(TEXT("CPP_Default_") + ParamName)));
			if (DefaultText.IsEmpty())
			{
				continue;
			}
			ParamIt->ImportText_Direct(
				*DefaultText,
				ParamIt->ContainerPtrToValuePtr<void>(Frame),
				nullptr, PPF_None);
		}

		FText FailReason;
		if (!FJsonObjectConverter::JsonObjectToUStruct(
			ArgObject, Function, Frame,
			CPF_Parm, CPF_ReturnParm, /*bStrictMode=*/false, &FailReason))
		{
			OutError = FString::Printf(
				TEXT("could not map 'args' onto %s parameters: %s"), *FunctionLabel, *FailReason.ToString());
			return false;
		}

		// Catch object args that resolved to null before calling. Passing a
		// silently-null asset into a function is how a wrong result gets
		// blamed on the function instead of on the path that was typed.
		for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
		{
			if (!ParamIt->HasAnyPropertyFlags(CPF_Parm) || ParamIt->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			const TSharedPtr<FJsonValue>* SuppliedValue = nullptr;
			for (const auto& ArgPair : ArgObject->Values)
			{
				if (UplinkCompat::JsonKeyToString(ArgPair.Key).Equals(ParamIt->GetName(), ESearchCase::IgnoreCase))
				{
					SuppliedValue = &ArgPair.Value;
					break;
				}
			}
			if (!SuppliedValue)
			{
				continue;
			}
			FString ObjectError;
			if (!ResolveObjectReference(*ParamIt, *SuppliedValue,
				ParamIt->ContainerPtrToValuePtr<void>(Frame), ObjectError))
			{
				OutError = FString::Printf(TEXT("arg %s"), *ObjectError);
				return false;
			}
		}
		return true;
	}

	void InvokeFunction(UObject* Target, UFunction* Function, void* Frame)
	{
		// An actor sitting in the editor world will not run a function
		// without this, and it will not say so either.
		//
		// AActor::GetFunctionCallspace answers Absorbed for an actor whose
		// world is not a game world, and ProcessEvent's reaction to a
		// callspace without the Local bit is a bare `return`. Nothing is
		// logged and no error surfaces, so the call came back "successful"
		// carrying the parameter frame exactly as it went in - which reads
		// as a real return value. K2_GetActorLocation on an actor at
		// z=12345 answered (0,0,0): not the actor's location, just the
		// zeroed frame handed back untouched.
		//
		// GAllowActorScriptExecutionInEditor is the engine's own switch for
		// this, and its comment says what it is for: "only true when we know
		// it's being called on an editor-placed object". The editor sets it
		// the same way to run Blueprint functions on placed actors.
		//
		// Only for non-game worlds. Inside PIE the callspace logic is doing
		// real work - deciding what is absorbed on a client and what goes
		// over the wire - and forcing Local there would quietly change what
		// a networked test is actually testing.
		const UWorld* ObjectWorld = Target->GetWorld();
		const bool bGameWorld = ObjectWorld && ObjectWorld->IsGameWorld();
		TGuardValue<bool> EditorExecutionGuard(
			GAllowActorScriptExecutionInEditor, bGameWorld ? GAllowActorScriptExecutionInEditor : true);
		Target->ProcessEvent(Function, Frame);
	}

	TSharedRef<FJsonObject> FunctionResultToJson(const UFunction* Function, const void* Frame)
	{
		// CheckFlags must be 0, not CPF_Parm: the frame only ever contains
		// parameters, but the filter also recurses into struct parameters and
		// strips their members (which are not themselves parameters), so a
		// struct out-param would come back as an empty object.
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		FJsonObjectConverter::UStructToJsonObject(Function, Frame, Out, 0, 0);
		return Out;
	}
}
