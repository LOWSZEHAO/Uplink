// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Reflection tools: get_property, set_property, call_function.

#include "UplinkTools.h"
#include "UplinkCompat.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "JsonObjectConverter.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

using namespace UplinkToolUtil;

namespace
{
	/**
	 * Read a property as JSON, but report an object reference as its path
	 * instead of serialising the object's contents.
	 *
	 * The converter expands an instanced object property inline, walking into
	 * everything it owns. A UMG widget owns its children and each child points
	 * back at its parent, so that walk never terminates: reading one child
	 * widget overflowed the stack and took the whole editor down. A path is
	 * also the more useful answer, since it can be passed straight back in as
	 * the target of another call.
	 */
	TSharedPtr<FJsonValue> ObjectReferenceOrJson(FProperty* Property, const void* ValueAddr)
	{
		if (const FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Referenced = AsObject->GetObjectPropertyValue(ValueAddr);
			return MakeShared<FJsonValueString>(Referenced ? Referenced->GetPathName() : FString());
		}
		return FJsonObjectConverter::UPropertyToJsonValue(Property, ValueAddr);
	}

	/**
	 * Resolve a dotted property path such as "MyStruct.Inner.Value", returning the
	 * leaf property and the address of its value. Needed because some engine
	 * structs refuse to serialise as a whole (FPoseSearchBlueprintResult exports
	 * as an empty object), but their individual members read back fine.
	 */
	FProperty* ResolvePropertyPath(
		UObject* Object,
		const FString& Path,
		void*& OutContainer,
		FString& OutError,
		UObject** OutOwningObject = nullptr,
		FProperty** OutMemberProperty = nullptr)
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

		// The object that actually holds the value, which is NOT the target
		// whenever the path crosses an object reference, and the outermost
		// property on it. Both matter after the write: Modify() has to record
		// the owner or the edit is not in the transaction and Ctrl+Z will not
		// bring it back, and PostEditChangeProperty has to name the member or
		// handlers keyed on it - every USceneComponent transform field - never
		// run, so the viewport does not move.
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
}

void UplinkTools::RegisterObject(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("get_property"),
		TEXT("Read any UPROPERTY of an object as JSON. Target by 'object_path' (also accepts 'subsystem:<Class>' for live subsystem instances), or 'actor' (name/label) plus optional 'component'. 'property' accepts a dotted path to reach struct members, e.g. 'MyStruct.Inner.Value' - useful because a few engine structs will not serialise as a whole but their members read fine."),
		TEXT(R"json({"type":"object","properties":{"object_path":{"type":"string"},"actor":{"type":"string"},"component":{"type":"string"},"property":{"type":"string"},"world":{"type":"string","enum":["editor","pie"]}},"required":["property"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			UObject* Object = ResolveObject(Ctx.Params, World, Error);
			if (!Object)
			{
				return FUplinkToolResult::Error(Error);
			}

			void* ValueAddr = nullptr;
			FProperty* Property = ResolvePropertyPath(Object, GetString(Ctx.Params, TEXT("property")), ValueAddr, Error);
			if (!Property)
			{
				return FUplinkToolResult::Error(Error);
			}

			const TSharedPtr<FJsonValue> Value = ObjectReferenceOrJson(Property, ValueAddr);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("object"), Object->GetPathName());
			Data->SetStringField(TEXT("type"), Property->GetCPPType());
			Data->SetField(TEXT("value"), Value);
			return FUplinkToolResult::Ok(Data);
		});

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
			if (!FJsonObjectConverter::JsonValueToUProperty(Value, Property, ValueAddr))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("could not convert JSON to %s (%s)"), *Property->GetName(), *Property->GetCPPType()));
			}

			// A path that resolves to nothing would otherwise be written as
			// null and reported as a success - this is how a material ended up
			// with no parent and rendered black with nothing in the logs.
			if (FString ObjectError; !NamedObjectResolved(Property, Value, ValueAddr, ObjectError))
			{
				return FUplinkToolResult::Error(ObjectError);
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
			Data->SetField(TEXT("value"), ObjectReferenceOrJson(Property, ValueAddr));

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

	Registry.RegisterQuick(
		TEXT("call_function"),
		TEXT("Call any UFUNCTION (BlueprintCallable, BlueprintPure, or plain UFUNCTION) on an object by reflection. 'args' maps parameter names to JSON values; the response contains the return value and all out-parameters. This exposes the entire Blueprint-callable API of a project through one tool. 'object_path' also accepts 'subsystem:<Class>' to reach live subsystem instances (e.g. subsystem:AssetEditorSubsystem to open asset editors)."),
		TEXT(R"json({"type":"object","properties":{"object_path":{"type":"string"},"actor":{"type":"string"},"component":{"type":"string"},"function":{"type":"string"},"args":{"type":"object","description":"Parameter name -> JSON value"},"world":{"type":"string","enum":["editor","pie"]}},"required":["function"]})json"),
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

			const FString FunctionName = GetString(Ctx.Params, TEXT("function"));
			UFunction* Function = Object->FindFunction(FName(*FunctionName));
			if (!Function)
			{
				// List callable candidates so the model can self-correct.
				TArray<FString> Names;
				for (TFieldIterator<UFunction> It(Object->GetClass()); It && Names.Num() < 40; ++It)
				{
					Names.Add(It->GetName());
				}
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("function '%s' not found on %s. Some available functions: %s"),
					*FunctionName, *Object->GetClass()->GetName(), *FString::Join(Names, TEXT(", "))));
			}

			// Build the parameter frame from JSON (skip the return value on import).
			FStructOnScope ParamFrame(Function);
			TSharedPtr<FJsonObject> Args;
			const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
			if (Ctx.Params->TryGetObjectField(FStringView(TEXT("args")), ArgsPtr) && ArgsPtr->IsValid())
			{
				Args = *ArgsPtr;
			}
			else
			{
				Args = MakeShared<FJsonObject>();
			}

			// Calling an instance method on a class-default object runs with a
			// bogus 'this' and can take the editor down (crashed it once by
			// calling a subsystem's OpenEditorForAssets through its CDO).
			// Statics are safe - that is how the scripting libraries are used.
			if (Object->HasAnyFlags(RF_ClassDefaultObject) && !Function->HasAnyFunctionFlags(FUNC_Static))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' is an instance method and '%s' is a class-default object; calling it would run without a valid instance and can crash the editor. ")
					TEXT("Use a real instance (spawn/find the actor, or a subsystem instance) - only static library functions may be called through a Default__ path."),
					*FunctionName, *Object->GetName()));
			}

			// Reject arg names that match no parameter — otherwise a typo'd
			// arg silently becomes a zero-default (found during live testing).
			TArray<FString> ParamNames;
			for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
			{
				if (ParamIt->HasAnyPropertyFlags(CPF_Parm) && !ParamIt->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					ParamNames.Add(ParamIt->GetName());
				}
			}
			for (const auto& ArgPair : Args->Values)
			{
				const FString ArgName = UplinkCompat::JsonKeyToString(ArgPair.Key);
				const bool bKnown = ParamNames.ContainsByPredicate([&ArgName](const FString& ParamName)
				{
					return ParamName.Equals(ArgName, ESearchCase::IgnoreCase);
				});
				if (!bKnown)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("unknown arg '%s' for %s. Expected parameters: %s"),
						*ArgName, *FunctionName, *FString::Join(ParamNames, TEXT(", "))));
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
				for (const auto& ArgPair : Args->Values)
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
					ParamIt->ContainerPtrToValuePtr<void>(ParamFrame.GetStructMemory()),
					nullptr, PPF_None);
			}

			FText FailReason;
			if (!FJsonObjectConverter::JsonObjectToUStruct(
				Args.ToSharedRef(), Function, ParamFrame.GetStructMemory(),
				CPF_Parm, CPF_ReturnParm, /*bStrictMode=*/false, &FailReason))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("could not map 'args' onto %s parameters: %s"), *FunctionName, *FailReason.ToString()));
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
				const TSharedPtr<FJsonValue>* Supplied = nullptr;
				for (const auto& ArgPair : Args->Values)
				{
					if (UplinkCompat::JsonKeyToString(ArgPair.Key).Equals(ParamIt->GetName(), ESearchCase::IgnoreCase))
					{
						Supplied = &ArgPair.Value;
						break;
					}
				}
				if (!Supplied)
				{
					continue;
				}
				FString ObjectError;
				if (!NamedObjectResolved(*ParamIt, *Supplied,
					ParamIt->ContainerPtrToValuePtr<void>(ParamFrame.GetStructMemory()), ObjectError))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("arg %s"), *ObjectError));
				}
			}

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
			{
				const UWorld* ObjectWorld = Object->GetWorld();
				const bool bGameWorld = ObjectWorld && ObjectWorld->IsGameWorld();
				TGuardValue<bool> EditorExecutionGuard(
					GAllowActorScriptExecutionInEditor, bGameWorld ? GAllowActorScriptExecutionInEditor : true);
				Object->ProcessEvent(Function, ParamFrame.GetStructMemory());
			}

			// Export the same frame: out-params + the literal 'ReturnValue'.
			// CheckFlags must be 0, not CPF_Parm: the frame only ever contains
			// parameters, but the filter also recurses into struct parameters and
			// strips their members (which are not themselves parameters), so a
			// struct out-param would come back as an empty object.
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			FJsonObjectConverter::UStructToJsonObject(Function, ParamFrame.GetStructMemory(), Out, 0, 0);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("object"), Object->GetPathName());
			Data->SetStringField(TEXT("function"), FunctionName);
			Data->SetObjectField(TEXT("result"), Out);
			return FUplinkToolResult::Ok(Data);
		});
}
