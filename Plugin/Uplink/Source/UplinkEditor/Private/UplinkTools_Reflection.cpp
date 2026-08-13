// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Discovery tools: class_info, find_functions. These make the engine's whole
// reflected surface findable, so call_function / get_property / watch_events
// can reach systems that have no dedicated tool (materials, animation,
// editor scripting libraries, ...).

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

using namespace UplinkToolUtil;

namespace
{
	bool IsRealClass(const UClass* Class)
	{
		// Skip compile artifacts and dead classes that pollute discovery.
		const FString Name = Class->GetName();
		return !Class->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated)
			&& !Name.StartsWith(TEXT("SKEL_"))
			&& !Name.StartsWith(TEXT("REINST_"))
			&& !Name.StartsWith(TEXT("TRASHCLASS_"))
			&& !Name.StartsWith(TEXT("HOTRELOADED_"));
	}

	FString DescribeFunctionFlags(const UFunction* Function)
	{
		TArray<FString> Flags;
		if (Function->HasAnyFunctionFlags(FUNC_Static)) { Flags.Add(TEXT("static")); }
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintCallable)) { Flags.Add(TEXT("callable")); }
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure)) { Flags.Add(TEXT("pure")); }
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent)) { Flags.Add(TEXT("event")); }
		return FString::Join(Flags, TEXT(","));
	}

	FString BuildSignature(const UFunction* Function)
	{
		FString Params;
		FString Return = TEXT("void");
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}
			if (It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				Return = It->GetCPPType();
				continue;
			}
			if (!Params.IsEmpty())
			{
				Params += TEXT(", ");
			}
			if (It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReferenceParm))
			{
				Params += TEXT("out ");
			}
			Params += FString::Printf(TEXT("%s %s"), *It->GetCPPType(), *It->GetName());
		}
		return FString::Printf(TEXT("%s(%s) -> %s"), *Function->GetName(), *Params, *Return);
	}

	/** How call_function should address this function: the CDO path for statics. */
	void AddCallHint(const TSharedRef<FJsonObject>& Row, const UClass* Class, const UFunction* Function)
	{
		if (Function->HasAnyFunctionFlags(FUNC_Static))
		{
			Row->SetStringField(TEXT("call_via_object_path"), Class->GetDefaultObject()->GetPathName());
		}
	}
}

void UplinkTools::RegisterReflection(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("class_info"),
		TEXT("Reflect a class: its properties (usable with get_property/set_property), functions with full signatures (usable with call_function), and multicast delegates (usable with watch_events). This is how you discover what an object can do."),
		TEXT(R"json({"type":"object","properties":{"class":{"type":"string","description":"Class path, e.g. /Script/Engine.Actor or /Game/BP_X.BP_X_C"},"contains":{"type":"string","description":"Substring filter applied to member names"},"include_inherited":{"type":"boolean","default":true},"max":{"type":"number","default":150,"description":"Cap per member kind"}},"required":["class"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			UClass* Class = StaticLoadClass(UObject::StaticClass(), nullptr, *GetString(Ctx.Params, TEXT("class")));
			if (!Class)
			{
				return FUplinkToolResult::Error(TEXT("class not found (native: /Script/Module.Class, blueprint: /Game/Path/BP.BP_C)"));
			}

			const FString Filter = GetString(Ctx.Params, TEXT("contains"));
			bool bInherited = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("include_inherited")), bInherited);
			const EFieldIteratorFlags::SuperClassFlags SuperFlags =
				bInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 150)), 1, 500);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("class"), Class->GetPathName());
			Data->SetStringField(TEXT("parent"), Class->GetSuperClass() ? Class->GetSuperClass()->GetPathName() : TEXT(""));

			TArray<TSharedPtr<FJsonValue>> Properties;
			TArray<TSharedPtr<FJsonValue>> Delegates;
			for (TFieldIterator<FProperty> It(Class, SuperFlags); It; ++It)
			{
				if (!Filter.IsEmpty() && !It->GetName().Contains(Filter))
				{
					continue;
				}
				if (const FMulticastDelegateProperty* Delegate = CastField<FMulticastDelegateProperty>(*It))
				{
					if (Delegates.Num() >= Max)
					{
						continue;
					}
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Delegate->GetName());
					Row->SetStringField(TEXT("signature"),
						Delegate->SignatureFunction ? BuildSignature(Delegate->SignatureFunction) : TEXT(""));
					Delegates.Add(MakeShared<FJsonValueObject>(Row));
					continue;
				}
				if (Properties.Num() >= Max)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), It->GetName());
				Row->SetStringField(TEXT("type"), It->GetCPPType());
				Properties.Add(MakeShared<FJsonValueObject>(Row));
			}

			TArray<TSharedPtr<FJsonValue>> Functions;
			for (TFieldIterator<UFunction> It(Class, SuperFlags); It && Functions.Num() < Max; ++It)
			{
				if (!Filter.IsEmpty() && !It->GetName().Contains(Filter))
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("signature"), BuildSignature(*It));
				Row->SetStringField(TEXT("flags"), DescribeFunctionFlags(*It));
				AddCallHint(Row, Class, *It);
				Functions.Add(MakeShared<FJsonValueObject>(Row));
			}

			Data->SetArrayField(TEXT("properties"), Properties);
			Data->SetArrayField(TEXT("functions"), Functions);
			Data->SetArrayField(TEXT("delegates"), Delegates);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("find_functions"),
		TEXT("Search every loaded class in the engine and project for functions whose name matches a keyword (e.g. 'MaterialExpression', 'Notify', 'Montage'). Returns full signatures plus, for static library functions, the object_path to pass to call_function - this is how you find operations Uplink has no dedicated tool for."),
		TEXT(R"json({"type":"object","properties":{"query":{"type":"string","description":"Substring of the function name"},"class_contains":{"type":"string","description":"Optional substring filter on the owning class name"},"callable_only":{"type":"boolean","default":true,"description":"Only BlueprintCallable/Pure functions"},"max":{"type":"number","default":60}},"required":["query"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Query = GetString(Ctx.Params, TEXT("query"));
			if (Query.Len() < 3)
			{
				return FUplinkToolResult::Error(TEXT("'query' must be at least 3 characters"));
			}
			const FString ClassFilter = GetString(Ctx.Params, TEXT("class_contains"));
			bool bCallableOnly = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("callable_only")), bCallableOnly);
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 60)), 1, 200);

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Total = 0;
			for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
			{
				const UClass* Class = *ClassIt;
				if (!IsRealClass(Class))
				{
					continue;
				}
				if (!ClassFilter.IsEmpty() && !Class->GetName().Contains(ClassFilter))
				{
					continue;
				}
				// ExcludeSuper so each function is reported once, on its owner.
				for (TFieldIterator<UFunction> FunctionIt(const_cast<UClass*>(Class), EFieldIteratorFlags::ExcludeSuper); FunctionIt; ++FunctionIt)
				{
					const UFunction* Function = *FunctionIt;
					if (!Function->GetName().Contains(Query))
					{
						continue;
					}
					if (bCallableOnly && !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
					{
						continue;
					}
					++Total;
					if (Rows.Num() >= Max)
					{
						continue;
					}
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("class"), Class->GetPathName());
					Row->SetStringField(TEXT("signature"), BuildSignature(Function));
					Row->SetStringField(TEXT("flags"), DescribeFunctionFlags(Function));
					AddCallHint(Row, Class, Function);
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("functions"), Rows);
			Data->SetNumberField(TEXT("total_matching"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Rows.Num());
			return FUplinkToolResult::Ok(Data);
		});
}
