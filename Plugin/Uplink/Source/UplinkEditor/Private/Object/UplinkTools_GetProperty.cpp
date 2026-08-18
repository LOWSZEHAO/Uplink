// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// get_property: read any UPROPERTY, by dotted path, as JSON.

#include "Object/UplinkObjectTools.h"

#include "Object/UplinkObjectPath.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"
#include "UplinkValueConverter.h"

#include "UObject/UnrealType.h"

using namespace UplinkToolUtil;

void UplinkObject::RegisterGetProperty(FUplinkToolRegistry& Registry)
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

			const TSharedPtr<FJsonValue> Value = UplinkValue::PropertyToJson(Property, ValueAddr);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("object"), Object->GetPathName());
			Data->SetStringField(TEXT("type"), Property->GetCPPType());
			Data->SetField(TEXT("value"), Value);
			return FUplinkToolResult::Ok(Data);
		});
}
