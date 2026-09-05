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
		// Containers are walked element by element so the object form above
		// applies inside them too. Handed an array directly, the engine
		// converter writes its objects as
		// /Script/Engine.Material'/Game/M_X.M_X' while the very same object
		// read on its own comes back as /Game/M_X.M_X - one tool reporting one
		// reference in two spellings, and only the second one is a path
		// anything else here accepts back.
		if (const FArrayProperty* AsArray = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(AsArray, ValueAddr);
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Helper.Num());
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				Items.Add(PropertyToJson(AsArray->Inner, Helper.GetRawPtr(Index)));
			}
			return MakeShared<FJsonValueArray>(Items);
		}
		if (const FSetProperty* AsSet = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(AsSet, ValueAddr);
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Helper.Num());
			// Sets are sparse: Num() counts live entries but the indices they
			// sit at are not contiguous, so this walks the capacity and skips
			// the holes rather than reading garbage out of them.
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (Helper.IsValidIndex(Index))
				{
					Items.Add(PropertyToJson(AsSet->ElementProp, Helper.GetElementPtr(Index)));
				}
			}
			return MakeShared<FJsonValueArray>(Items);
		}
		return FJsonObjectConverter::UPropertyToJsonValue(Property, ValueAddr);
	}

	namespace
	{
		/** The UEnum behind an enum-typed property, whichever form it takes. */
		const UEnum* EnumBehind(const FProperty* Property)
		{
			if (const FEnumProperty* AsEnum = CastField<FEnumProperty>(Property))
			{
				return AsEnum->GetEnum();
			}
			if (const FNumericProperty* AsNumeric = CastField<FNumericProperty>(Property))
			{
				return AsNumeric->IsEnum() ? AsNumeric->GetIntPropertyEnum() : nullptr;
			}
			return nullptr;
		}

		/**
		 * Refuse a number that no enumerator answers to.
		 *
		 * The engine's importer calls SetIntPropertyValue with no bounds check,
		 * so a numeric write of 99 to a six-entry enum succeeds at the memory
		 * level and leaves the property holding a value nothing in the enum
		 * names. Nothing logs it. It reads back as an empty string, which is
		 * how it usually gets noticed - somewhere else, much later.
		 *
		 * Bitflag enums are left alone: a combination is a legitimate value
		 * there and is deliberately absent from the names table.
		 */
		bool EnumValueIsInRange(const FProperty* Property, const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			const UEnum* Enum = EnumBehind(Property);
			if (!Enum || !Value.IsValid())
			{
				return true;
			}

			// Booleans first, and refused rather than converted. FJsonValueBoolean
			// answers TryGetNumber with 1 or 0, so true on an enum property would
			// otherwise pass the range check below and land on whichever entry
			// happens to be numbered 1 - a write that reports success and sets
			// something nobody named.
			if (Value->Type == EJson::Boolean)
			{
				OutError = FString::Printf(
					TEXT("%s is the enum %s, not a bool - true and false would read as entries 1 and 0. ")
					TEXT("Pass an entry name or its number."), *Property->GetName(), *Enum->GetName());
				return false;
			}

			double Number = 0.0;
			if (Value->Type != EJson::Number || !Value->TryGetNumber(Number))
			{
				// A name; the importer resolves it and refuses what it cannot.
				return true;
			}
			if (Enum->HasMetaData(TEXT("Bitflags")) || Enum->IsValidEnumValue(static_cast<int64>(Number)))
			{
				return true;
			}

			TArray<FString> Entries;
			for (int32 Index = 0; Index < Enum->NumEnums() - 1 && Entries.Num() < 24; ++Index)
			{
				Entries.Add(FString::Printf(TEXT("%s=%lld"),
					*Enum->GetNameStringByIndex(Index), Enum->GetValueByIndex(Index)));
			}
			OutError = FString::Printf(
				TEXT("%lld is not a value of %s. Valid: %s"),
				static_cast<int64>(Number), *Enum->GetName(), *FString::Join(Entries, TEXT(", ")));
			return false;
		}
	}

	bool JsonToProperty(
		const TSharedPtr<FJsonValue>& Value,
		FProperty* Property,
		void* ValueAddr,
		FString& OutError)
	{
		// Before the write, not after: the importer would already have put the
		// out-of-range number in the property by the time it could be caught.
		if (!EnumValueIsInRange(Property, Value, OutError))
		{
			return false;
		}
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

		// The return value sits in this same frame, so it has to be skipped on
		// the way in: it belongs to the call, not to the caller.
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
