// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "Object/UplinkObjectPath.h"

#include "UObject/UnrealType.h"

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
		FProperty* Found = Owner->FindPropertyByName(FName(*Segments[i]));
		if (!Found)
		{
			OutError = FString::Printf(TEXT("property '%s' not found on %s (path '%s')"),
				*Segments[i], *Owner->GetName(), *Path);
			return nullptr;
		}
		if (!MemberProperty)
		{
			MemberProperty = Found;
		}
		if (i == Segments.Num() - 1)
		{
			OutContainer = Found->ContainerPtrToValuePtr<void>(Container);
			if (OutOwningObject)
			{
				*OutOwningObject = OwningObject;
			}
			if (OutMemberProperty)
			{
				*OutMemberProperty = MemberProperty;
			}
			return Found;
		}
		if (FStructProperty* AsStruct = CastField<FStructProperty>(Found))
		{
			Container = AsStruct->ContainerPtrToValuePtr<void>(Container);
			Owner = AsStruct->Struct;
			continue;
		}
		// Step through an object reference too, so a path can cross from
		// an actor into a component and on into that component's structs -
		// RootComponent.RelativeLocation.X and the like.
		if (const FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(Found))
		{
			UObject* Next = AsObject->GetObjectPropertyValue(
				AsObject->ContainerPtrToValuePtr<void>(Container));
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
			*Segments[i], *Found->GetClass()->GetName(), *Path);
		return nullptr;
	}
	return nullptr;
}
