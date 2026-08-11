// Copyright (c) 2026 Low Sze Hao. MIT License.
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
	FProperty* FindPropertyChecked(UObject* Object, const FString& PropertyName, FString& OutError)
	{
		FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *PropertyName);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("property '%s' not found on %s"),
				*PropertyName, *Object->GetClass()->GetName());
		}
		return Property;
	}
}

void UplinkTools::RegisterObject(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("get_property"),
		TEXT("Read any UPROPERTY of an object as JSON. Target by 'object_path', or 'actor' (name/label) plus optional 'component'."),
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

			FProperty* Property = FindPropertyChecked(Object, GetString(Ctx.Params, TEXT("property")), Error);
			if (!Property)
			{
				return FUplinkToolResult::Error(Error);
			}

			const TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(
				Property, Property->ContainerPtrToValuePtr<void>(Object));

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("object"), Object->GetPathName());
			Data->SetStringField(TEXT("type"), Property->GetCPPType());
			Data->SetField(TEXT("value"), Value);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("set_property"),
		TEXT("Write any UPROPERTY of an object from a JSON value (numbers, strings, bools, structs as objects, arrays). In the editor world this also runs PostEditChangeProperty so the editor reacts like a Details-panel edit."),
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

			FProperty* Property = FindPropertyChecked(Object, GetString(Ctx.Params, TEXT("property")), Error);
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
				Object->Modify();
			}
#endif
			if (!FJsonObjectConverter::JsonValueToUProperty(
				Value, Property, Property->ContainerPtrToValuePtr<void>(Object)))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("could not convert JSON to %s (%s)"), *Property->GetName(), *Property->GetCPPType()));
			}
#if WITH_EDITOR
			if (bEditorObject)
			{
				FPropertyChangedEvent ChangeEvent(Property);
				Object->PostEditChangeProperty(ChangeEvent);
				Object->MarkPackageDirty();
			}
#endif
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("object"), Object->GetPathName());
			Data->SetField(TEXT("value"), FJsonObjectConverter::UPropertyToJsonValue(
				Property, Property->ContainerPtrToValuePtr<void>(Object)));
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("call_function"),
		TEXT("Call any UFUNCTION (BlueprintCallable, BlueprintPure, or plain UFUNCTION) on an object by reflection. 'args' maps parameter names to JSON values; the response contains the return value and all out-parameters. This exposes the entire Blueprint-callable API of a project through one tool."),
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

			FText FailReason;
			if (!FJsonObjectConverter::JsonObjectToUStruct(
				Args.ToSharedRef(), Function, ParamFrame.GetStructMemory(),
				CPF_Parm, CPF_ReturnParm, /*bStrictMode=*/false, &FailReason))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("could not map 'args' onto %s parameters: %s"), *FunctionName, *FailReason.ToString()));
			}

			Object->ProcessEvent(Function, ParamFrame.GetStructMemory());

			// Export the same frame: out-params + the literal 'ReturnValue'.
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			FJsonObjectConverter::UStructToJsonObject(Function, ParamFrame.GetStructMemory(), Out, CPF_Parm, 0);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("object"), Object->GetPathName());
			Data->SetStringField(TEXT("function"), FunctionName);
			Data->SetObjectField(TEXT("result"), Out);
			return FUplinkToolResult::Ok(Data);
		});
}
