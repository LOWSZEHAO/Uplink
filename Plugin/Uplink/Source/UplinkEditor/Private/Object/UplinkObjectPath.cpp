// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "Object/UplinkObjectPath.h"

#include "UObject/UnrealType.h"

namespace
{
	/**
	 * Split "Items[2]" into the name and the index. An inventory, a spawn list,
	 * a set of waypoints - the interesting value is usually in one element, and
	 * without this a caller can only fetch the whole array and pick through the
	 * JSON, which an assertion cannot do at all.
	 */
	bool ParseSegment(const FString& Segment, FString& OutName, int32& OutIndex, FString& OutError)
	{
		OutIndex = INDEX_NONE;
		int32 Open = INDEX_NONE;
		if (!Segment.FindChar(TEXT('['), Open))
		{
			OutName = Segment;
			return true;
		}
		int32 Close = INDEX_NONE;
		if (!Segment.FindLastChar(TEXT(']'), Close) || Close < Open)
		{
			OutError = FString::Printf(TEXT("'%s' has an unclosed ["), *Segment);
			return false;
		}
		const FString IndexText = Segment.Mid(Open + 1, Close - Open - 1);
		if (IndexText.IsEmpty() || !IndexText.IsNumeric())
		{
			OutError = FString::Printf(TEXT("'%s' needs a whole number between the brackets"), *Segment);
			return false;
		}
		OutName = Segment.Left(Open);
		OutIndex = FCString::Atoi(*IndexText);
		return true;
	}
}

FProperty* UplinkObject::ResolvePropertyPath(
	UObject* Object,
	const FString& Path,
	void*& OutContainer,
	FString& OutError,
	UObject** OutOwningObject,
	FProperty** OutMemberProperty)
{
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("."), true);
	if (Segments.Num() == 0)
	{
		OutError = TEXT("'property' is required");
		return nullptr;
	}

	void* Container = Object;
	UStruct* Owner = Object->GetClass();
	UObject* OwningObject = Object;
	FProperty* MemberProperty = nullptr;

	for (int32 i = 0; i < Segments.Num(); ++i)
	{
		FString SegmentName;
		int32 ElementIndex = INDEX_NONE;
		if (!ParseSegment(Segments[i], SegmentName, ElementIndex, OutError))
		{
			return nullptr;
		}

		FProperty* Found = Owner->FindPropertyByName(FName(*SegmentName));
		if (!Found)
		{
			OutError = FString::Printf(TEXT("property '%s' not found on %s (path '%s')"),
				*SegmentName, *Owner->GetName(), *Path);
			return nullptr;
		}
		if (!MemberProperty)
		{
			MemberProperty = Found;
		}

		// Resolve the segment to one address, indexed or not, so every step
		// below works the same whether or not brackets were used.
		FProperty* Effective = Found;
		void* ValuePtr = Found->ContainerPtrToValuePtr<void>(Container);
		if (ElementIndex != INDEX_NONE)
		{
			FArrayProperty* AsArray = CastField<FArrayProperty>(Found);
			if (!AsArray)
			{
				OutError = FString::Printf(
					TEXT("'%s' is a %s, not an array, so '%s[%d]' means nothing"),
					*SegmentName, *Found->GetClass()->GetName(), *SegmentName, ElementIndex);
				return nullptr;
			}
			FScriptArrayHelper Helper(AsArray, ValuePtr);
			if (!Helper.IsValidIndex(ElementIndex))
			{
				// Say how many there are: an assertion against an empty
				// inventory otherwise reads as a broken path rather than as
				// the empty inventory it is describing.
				OutError = FString::Printf(TEXT("'%s' has %d element(s), so index %d is out of range"),
					*SegmentName, Helper.Num(), ElementIndex);
				return nullptr;
			}
			ValuePtr = Helper.GetRawPtr(ElementIndex);
			Effective = AsArray->Inner;
		}

		if (i == Segments.Num() - 1)
		{
			OutContainer = ValuePtr;
			if (OutOwningObject)
			{
				*OutOwningObject = OwningObject;
			}
			if (OutMemberProperty)
			{
				*OutMemberProperty = MemberProperty;
			}
			return Effective;
		}
		if (FStructProperty* AsStruct = CastField<FStructProperty>(Effective))
		{
			Container = ValuePtr;
			Owner = AsStruct->Struct;
			continue;
		}
		// Step through an object reference too, so a path can cross from
		// an actor into a component and on into that component's structs -
		// RootComponent.RelativeLocation.X and the like.
		if (const FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(Effective))
		{
			UObject* Next = AsObject->GetObjectPropertyValue(ValuePtr);
			if (!Next)
			{
				OutError = FString::Printf(
					TEXT("'%s' is null, so '%s' cannot be reached"), *Segments[i], *Path);
				return nullptr;
			}
			Container = Next;
			Owner = Next->GetClass();
			// Everything from here belongs to Next, including the member
			// that PostEditChangeProperty will be told about.
			OwningObject = Next;
			MemberProperty = nullptr;
			continue;
		}
		OutError = FString::Printf(
			TEXT("'%s' is a %s - only structs and object references can be stepped through, so '%s' cannot be reached"),
			*Segments[i], *Effective->GetClass()->GetName(), *Path);
		return nullptr;
	}
	return nullptr;
}

bool UplinkObject::IsDeprecatedProperty(const FProperty* Property, FString& OutMessage)
{
	OutMessage.Reset();
	if (!Property)
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	static const FName DeprecatedKey(TEXT("DeprecatedProperty"));
	static const FName MessageKey(TEXT("DeprecationMessage"));
	if (!Property->HasMetaData(DeprecatedKey))
	{
		return false;
	}
	OutMessage = Property->GetMetaData(MessageKey);
	return true;
#else
	return false;
#endif
}
