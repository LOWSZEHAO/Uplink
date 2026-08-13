// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/UObjectGlobals.h"

/** Small param/lookup helpers shared by the tool implementations. */
namespace UplinkToolUtil
{
	/**
	 * The JSON converter resolves an object path it cannot find to null and
	 * reports success, so a typo'd asset path used to sail through and only
	 * show up much later as a wrong-looking result. This checks, after
	 * conversion, that every hard object reference the caller actually named
	 * came back non-null.
	 *
	 * Only hard object and class properties are checked. A soft reference is
	 * allowed to point at something unloaded, which is the whole point of it.
	 */
	inline bool NamedObjectResolved(
		const FProperty* Property,
		const TSharedPtr<FJsonValue>& Supplied,
		const void* ValueAddr,
		FString& OutError)
	{
		if (!Supplied.IsValid() || !ValueAddr)
		{
			return true;
		}
		if (Property->IsA<FSoftObjectProperty>() || Property->IsA<FSoftClassProperty>())
		{
			return true;
		}
		const FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(Property);
		if (!AsObject)
		{
			return true;
		}

		FString Path;
		if (!Supplied->TryGetString(Path) || Path.IsEmpty() || Path == TEXT("None"))
		{
			return true; // caller asked for null, or passed a non-path form
		}
		if (AsObject->GetObjectPropertyValue(ValueAddr) != nullptr)
		{
			return true;
		}

		const UClass* Expected = AsObject->PropertyClass;
		OutError = FString::Printf(
			TEXT("'%s': nothing found at '%s'%s. Check the path - an asset reference usually looks like ")
			TEXT("/Game/Folder/Asset.Asset, and a Blueprint class needs the _C suffix."),
			*Property->GetName(), *Path,
			Expected ? *FString::Printf(TEXT(" (expected a %s)"), *Expected->GetName()) : TEXT(""));
		return false;
	}

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

		// "subsystem:AssetEditorSubsystem" (or a full class path after the colon)
		// resolves the live subsystem instance - editor, engine, game-instance,
		// world or local-player - whose object paths are otherwise unguessable.
		if (ObjectPath.StartsWith(TEXT("subsystem:")))
		{
			const FString ClassSpec = ObjectPath.RightChop(10);
			UClass* Class = ClassSpec.StartsWith(TEXT("/"))
				? StaticLoadClass(UObject::StaticClass(), nullptr, *ClassSpec)
				: FindFirstObject<UClass>(*ClassSpec, EFindFirstObjectOptions::None);
			if (!Class)
			{
				OutError = FString::Printf(TEXT("subsystem class not found: %s"), *ClassSpec);
				return nullptr;
			}

			UObject* Subsystem = nullptr;
			if (Class->IsChildOf(UEditorSubsystem::StaticClass()))
			{
				Subsystem = GEditor ? GEditor->GetEditorSubsystemBase(Class) : nullptr;
			}
			else if (Class->IsChildOf(UEngineSubsystem::StaticClass()))
			{
				Subsystem = GEngine ? GEngine->GetEngineSubsystemBase(Class) : nullptr;
			}
			else if (Class->IsChildOf(UGameInstanceSubsystem::StaticClass()))
			{
				Subsystem = (World && World->GetGameInstance())
					? World->GetGameInstance()->GetSubsystemBase(Class) : nullptr;
			}
			else if (Class->IsChildOf(UWorldSubsystem::StaticClass()))
			{
				Subsystem = World ? World->GetSubsystemBase(Class) : nullptr;
			}
			else if (Class->IsChildOf(ULocalPlayerSubsystem::StaticClass()))
			{
				ULocalPlayer* Player = World ? World->GetFirstLocalPlayerFromController() : nullptr;
				Subsystem = Player ? Player->GetSubsystemBase(Class) : nullptr;
			}
			else
			{
				OutError = FString::Printf(TEXT("%s is not a subsystem class"), *Class->GetName());
				return nullptr;
			}

			if (!Subsystem)
			{
				OutError = FString::Printf(
					TEXT("no live instance of %s (game-instance/world/player subsystems need a running world - pass world:'pie')"),
					*Class->GetName());
			}
			return Subsystem;
		}

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
