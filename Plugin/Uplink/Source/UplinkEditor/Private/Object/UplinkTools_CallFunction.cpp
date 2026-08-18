// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// call_function: invoke any UFUNCTION by reflection, with JSON arguments.

#include "Object/UplinkObjectTools.h"

#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"
#include "UplinkValueConverter.h"

#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

using namespace UplinkToolUtil;

void UplinkObject::RegisterCallFunction(FUplinkToolRegistry& Registry)
{
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

			FStructOnScope ParamFrame(Function);
			TSharedPtr<FJsonObject> Args;
			const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
			if (Ctx.Params->TryGetObjectField(FStringView(TEXT("args")), ArgsPtr) && ArgsPtr->IsValid())
			{
				Args = *ArgsPtr;
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

			if (!UplinkValue::JsonToFunctionParams(
				Function, Args, ParamFrame.GetStructMemory(), FunctionName, Error))
			{
				return FUplinkToolResult::Error(Error);
			}

			UplinkValue::InvokeFunction(Object, Function, ParamFrame.GetStructMemory());

			const TSharedRef<FJsonObject> Out =
				UplinkValue::FunctionResultToJson(Function, ParamFrame.GetStructMemory());

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("object"), Object->GetPathName());
			Data->SetStringField(TEXT("function"), FunctionName);
			Data->SetObjectField(TEXT("result"), Out);
			return FUplinkToolResult::Ok(Data);
		});
}
