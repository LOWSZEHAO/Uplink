// Copyright (c) 2026 Low Sze Hao. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"

/** Small param/lookup helpers shared by the tool implementations. */
namespace UplinkToolUtil
{
	inline FString GetString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const FString& Default = FString())
	{
		FString Value = Default;
		if (Params.IsValid())
		{
			Params->TryGetStringField(FStringView(Field), Value);
		}
		return Value;
	}

	inline double GetNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, double Default)
	{
		double Value = Default;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(FStringView(Field), Value);
		}
		return Value;
	}

	inline bool TryGetVector(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FVector& Out)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(FStringView(Field), Obj) || !Obj->IsValid())
		{
			return false;
		}
		Out.X = (*Obj)->GetNumberField(FStringView(TEXT("x")));
		Out.Y = (*Obj)->GetNumberField(FStringView(TEXT("y")));
		Out.Z = (*Obj)->GetNumberField(FStringView(TEXT("z")));
		return true;
	}

	inline bool TryGetRotator(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FRotator& Out)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(FStringView(Field), Obj) || !Obj->IsValid())
		{
			return false;
		}
		Out.Pitch = (*Obj)->GetNumberField(FStringView(TEXT("pitch")));
		Out.Yaw = (*Obj)->GetNumberField(FStringView(TEXT("yaw")));
		Out.Roll = (*Obj)->GetNumberField(FStringView(TEXT("roll")));
		return true;
	}

	inline TSharedRef<FJsonObject> VectorToJson(const FVector& V)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	/** Find an actor by exact FName, then by exact editor label, then label-contains. */
	inline AActor* FindActor(UWorld* World, const FString& NameOrLabel)
	{
		AActor* ContainsMatch = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor->GetName() == NameOrLabel)
			{
				return Actor;
			}
#if WITH_EDITOR
			const FString Label = Actor->GetActorLabel();
			if (Label == NameOrLabel)
			{
				return Actor;
			}
			if (!ContainsMatch && Label.Contains(NameOrLabel))
			{
				ContainsMatch = Actor;
			}
#endif
		}
		return ContainsMatch;
	}

	/**
	 * Resolve the target UObject for property/function tools:
	 *  - "object_path": any loaded object by full path (e.g. /Game/L.L:PersistentLevel.Actor_0)
	 *  - "actor": actor by name/label in the resolved world
	 *  - "component" (with "actor"): component of that actor whose name matches or contains
	 */
	inline UObject* ResolveObject(const TSharedPtr<FJsonObject>& Params, UWorld* World, FString& OutError)
	{
		const FString ObjectPath = GetString(Params, TEXT("object_path"));
		if (!ObjectPath.IsEmpty())
		{
			UObject* Found = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
			if (!Found)
			{
				Found = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
			}
			if (!Found)
			{
				OutError = FString::Printf(TEXT("object not found: %s"), *ObjectPath);
			}
			return Found;
		}

		const FString ActorName = GetString(Params, TEXT("actor"));
		if (ActorName.IsEmpty())
		{
			OutError = TEXT("provide 'object_path' or 'actor'");
			return nullptr;
		}
		if (!World)
		{
			OutError = TEXT("no world available to search for the actor");
			return nullptr;
		}

		AActor* Actor = FindActor(World, ActorName);
		if (!Actor)
		{
			OutError = FString::Printf(TEXT("actor not found: %s"), *ActorName);
			return nullptr;
		}

		const FString ComponentName = GetString(Params, TEXT("component"));
		if (ComponentName.IsEmpty())
		{
			return Actor;
		}

		UActorComponent* ContainsMatch = nullptr;
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (!Component)
			{
				continue;
			}
			if (Component->GetName() == ComponentName)
			{
				return Component;
			}
			if (!ContainsMatch && Component->GetName().Contains(ComponentName))
			{
				ContainsMatch = Component;
			}
		}
		if (!ContainsMatch)
		{
			OutError = FString::Printf(TEXT("component '%s' not found on actor '%s'"), *ComponentName, *ActorName);
		}
		return ContainsMatch;
	}
}
