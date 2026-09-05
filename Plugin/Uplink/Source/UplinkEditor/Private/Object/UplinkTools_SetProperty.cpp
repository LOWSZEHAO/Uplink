// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// set_property: write any UPROPERTY, by dotted path, from JSON.

#include "Object/UplinkObjectTools.h"

#include "Object/UplinkObjectPath.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"
#include "UplinkValueConverter.h"

#include "UObject/UnrealType.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using namespace UplinkToolUtil;

namespace
{
	/**
	 * Did the caller's value reach the property?
	 *
	 * Asked of the property, not of JSON. Comparing the request against a
	 * re-serialisation of the property fails on spelling alone: an enum sent
	 * as 2, or as EComponentMobility::Static, reads back as "Movable" and
	 * "Static", and an object inside an array reads back in export form
	 * (/Script/Engine.Material'/Game/M_X.M_X'). All three of those writes land
	 * correctly, and all three used to be reported as writes that did not
	 * survive - which invites undoing work that was right.
	 *
	 * So the request is applied a second time, to a throwaway copy of what is
	 * actually there, and the two are compared with FProperty::Identical.
	 * Whatever spelling the writer accepts, this accepts. Starting the copy
	 * from the landed value rather than from a default-constructed one is what
	 * keeps a partial struct write honest: {"X":1} on a vector is a request
	 * about X, and Y and Z must not be read as having been asked for.
	 *
	 * A write that was reverted still fails, which is the whole point of the
	 * check: re-applying the request to the reverted value produces the value
	 * that was asked for, and that differs from what is there.
	 */
	bool ValueReachedProperty(
		const TSharedPtr<FJsonValue>& Wanted, FProperty* Property, const void* LandedAddr)
	{
		if (!Wanted.IsValid() || !Property || !LandedAddr)
		{
			return false;
		}

		// Properties hold constructed types - FString, TArray, structs with
		// destructors - so the scratch value is initialised and destroyed in
		// pairs rather than treated as raw bytes.
		void* Scratch = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
		Property->InitializeValue(Scratch);
		Property->CopyCompleteValue(Scratch, LandedAddr);

		FString ConversionError;
		const bool bApplied = UplinkValue::JsonToProperty(Wanted, Property, Scratch, ConversionError);
		const bool bSame = bApplied && Property->Identical(Scratch, LandedAddr);

		Property->DestroyValue(Scratch);
		FMemory::Free(Scratch);
		return bSame;
	}

	/** One line of JSON, for putting a value inside a sentence. */
	FString Describe(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return TEXT("nothing");
		}
		if (Value->Type == EJson::String)
		{
			return FString::Printf(TEXT("\"%s\""), *Value->AsString());
		}
		if (Value->Type == EJson::Boolean)
		{
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		}
		double Number = 0.0;
		if (Value->TryGetNumber(Number))
		{
			return FString::SanitizeFloat(Number);
		}
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		if (Value->Type == EJson::Object)
		{
			FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer);
		}
		else if (Value->Type == EJson::Array)
		{
			FJsonSerializer::Serialize(Value->AsArray(), Writer);
		}
		return Text.IsEmpty() ? TEXT("?") : Text;
	}
}

void UplinkObject::RegisterSetProperty(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("set_property"),
		TEXT("Write any UPROPERTY of an object from a JSON value (numbers, strings, bools, structs as objects, arrays). 'property' accepts a dotted path to reach struct members, e.g. 'MyStruct.Inner.Value', matching get_property. In the editor world this also runs PostEditChangeProperty so the editor reacts like a Details-panel edit. A named object reference that resolves to nothing is refused rather than written as null. A property the engine has deprecated is refused too, with its deprecation message - it would take the write and read straight back while nothing acts on it - pass force:true to write one anyway."),
		TEXT(R"json({"type":"object","properties":{"object_path":{"type":"string"},"actor":{"type":"string"},"component":{"type":"string"},"property":{"type":"string"},"value":{"description":"New value as JSON"},"force":{"type":"boolean","default":false,"description":"Write a deprecated property anyway"},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["property","value"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			if (!Error.IsEmpty())
			{
				// ResolveWorld names the world ids that do exist. ResolveObject
				// overwrites Error with a generic line, so an explicitly named
				// world that does not resolve has to be reported here.
				return FUplinkToolResult::Error(Error);
			}
			UObject* Object = ResolveObject(Ctx.Params, World, Error);
			if (!Object)
			{
				return FUplinkToolResult::Error(Error);
			}

			// Same dotted-path walk get_property uses, so a value that can be
			// read can also be written rather than only half the pair working.
			void* ValueAddr = nullptr;
			UObject* OwningObject = Object;
			FProperty* MemberProperty = nullptr;
			FProperty* Property = ResolvePropertyPath(
				Object, GetString(Ctx.Params, TEXT("property")), ValueAddr, Error,
				&OwningObject, &MemberProperty);
			if (!Property)
			{
				return FUplinkToolResult::Error(Error);
			}

			const TSharedPtr<FJsonValue> Value = Ctx.Params->TryGetField(FStringView(TEXT("value")));
			if (!Value.IsValid())
			{
				return FUplinkToolResult::Error(TEXT("'value' is required"));
			}

			// A retired property takes the write and hands it straight back on
			// a read, which is exactly how a wrong value survives: every check
			// a caller can make says the edit worked, while the engine has long
			// since stopped reading the field. Refused rather than warned,
			// because a warning attached to a success is the thing that gets
			// skimmed past - force is there for when writing it really is what
			// was meant.
			FString DeprecationMessage;
			bool bForce = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("force")), bForce);
			if (!bForce && IsDeprecatedProperty(Property, DeprecationMessage))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' on %s is deprecated, so writing it changes nothing the engine reads.%s%s ")
					TEXT("Pass force:true to write it anyway."),
					*GetString(Ctx.Params, TEXT("property")), *OwningObject->GetClass()->GetName(),
					DeprecationMessage.IsEmpty() ? TEXT("") : TEXT(" "), *DeprecationMessage));
			}

			const bool bEditorObject = !Ctx.IsPieWorld();
#if WITH_EDITOR
			if (bEditorObject)
			{
				// The owner, not the target: a path like RootComponent.RelativeLocation.X
				// writes into the component, and a transaction only restores what
				// was told to Modify().
				OwningObject->Modify();
			}
#endif
			if (!UplinkValue::JsonToProperty(Value, Property, ValueAddr, Error))
			{
				return FUplinkToolResult::Error(Error);
			}
#if WITH_EDITOR
			if (bEditorObject)
			{
				// Named after the outermost member, not the leaf. USceneComponent
				// gates its transform update on the member name, so an event that
				// says only "X" moves nothing: the value changes and the viewport
				// does not follow.
				FPropertyChangedEvent ChangeEvent(Property, EPropertyChangeType::ValueSet);
				if (MemberProperty)
				{
					ChangeEvent.SetActiveMemberProperty(MemberProperty);
				}
				OwningObject->PostEditChangeProperty(ChangeEvent);
				OwningObject->MarkPackageDirty();
			}
#endif
			// Read back through a FRESH resolve rather than the address written
			// to. PostEditChangeProperty can rerun an actor's construction
			// scripts, which rebuilds components - so the address the write used
			// may no longer belong to anything, and reading it would be reading
			// freed memory to report a number that is not there.
			const FString PropertyPath = GetString(Ctx.Params, TEXT("property"));
			void* ReadAddr = nullptr;
			FString ReadError;
			FProperty* ReadProperty = ResolvePropertyPath(Object, PropertyPath, ReadAddr, ReadError);
			const TSharedPtr<FJsonValue> Landed = (ReadProperty && ReadAddr)
				? UplinkValue::PropertyToJson(ReadProperty, ReadAddr)
				: nullptr;

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			// The object the value actually landed on, which is the component
			// rather than the actor whenever the path crossed a reference.
			Data->SetStringField(TEXT("object"), OwningObject->GetPathName());
			Data->SetStringField(TEXT("property"), PropertyPath);
			if (Landed.IsValid())
			{
				Data->SetField(TEXT("value"), Landed);
			}

			// A write that did not survive must not report success. It happens
			// for a real reason: a Blueprint variable that is not instance
			// editable is not meant to be overridden per instance, so rerunning
			// the construction scripts puts the class default back - and the
			// write, the change event and the dirty flag all succeed on the way
			// past. The silence is worse than the reset, because the next thing
			// anybody does is trust the value.
			const bool bLanded = ValueReachedProperty(Value, ReadProperty, ReadAddr);
			if (!bLanded)
			{
				const bool bBlueprintVariable = Property->HasAnyPropertyFlags(CPF_BlueprintVisible);
				const bool bNotInstanceEditable =
					!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance);
				const FString Why = (bBlueprintVariable && bNotInstanceEditable)
					? TEXT(" This Blueprint variable is not instance editable, so the actor's construction scripts put the class default back. Tick 'Instance Editable' on the variable, or write the class default on the Blueprint instead of the placed actor.")
					: FString();
				Data->SetBoolField(TEXT("written"), false);
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("the write did not survive: '%s' on %s reads back as %s.%s"),
					*PropertyPath, *OwningObject->GetName(), *Describe(Landed), *Why));
			}
			Data->SetBoolField(TEXT("written"), true);

			// Writing a CDO or a component template succeeds and changes nothing
			// anyone is looking at, because instances already in the level keep
			// their own copy. Say so rather than let it read as a no-op edit.
			if (OwningObject->HasAnyFlags(RF_ArchetypeObject | RF_ClassDefaultObject))
			{
				Data->SetBoolField(TEXT("archetype"), true);
				return FUplinkToolResult::Ok(Data,
					TEXT("written to a template - actors already placed in a level keep their own value, so respawn them to see it"));
			}
			return FUplinkToolResult::Ok(Data);
		});
}
