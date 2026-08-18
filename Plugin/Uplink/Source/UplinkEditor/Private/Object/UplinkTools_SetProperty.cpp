// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// set_property: write any UPROPERTY, by dotted path, from JSON.

#include "Object/UplinkObjectTools.h"

#include "Object/UplinkObjectPath.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"
#include "UplinkValueConverter.h"

#include "UObject/UnrealType.h"

using namespace UplinkToolUtil;

void UplinkObject::RegisterSetProperty(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("set_property"),
		TEXT("Write any UPROPERTY of an object from a JSON value (numbers, strings, bools, structs as objects, arrays). 'property' accepts a dotted path to reach struct members, e.g. 'MyStruct.Inner.Value', matching get_property. In the editor world this also runs PostEditChangeProperty so the editor reacts like a Details-panel edit. A named object reference that resolves to nothing is refused rather than written as null."),
		TEXT(R"json({"type":"object","properties":{"object_path":{"type":"string"},"actor":{"type":"string"},"component":{"type":"string"},"property":{"type":"string"},"value":{"description":"New value as JSON"},"world":{"type":"string","enum":["editor","pie"]}},"required":["property","value"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
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
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			// The object the value actually landed on, which is the component
			// rather than the actor whenever the path crossed a reference.
			Data->SetStringField(TEXT("object"), OwningObject->GetPathName());
			Data->SetField(TEXT("value"), UplinkValue::PropertyToJson(Property, ValueAddr));

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
