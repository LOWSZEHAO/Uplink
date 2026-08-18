// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Plumbing every bp_* tool leans on: asset and graph lookup, the pin-type
// spec parser, and the compile report.

#include "Blueprint/UplinkBlueprintCommon.h"
#include "UplinkToolUtil.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace UplinkToolUtil;

namespace UplinkBlueprint
{
	UBlueprint* LoadBlueprint(const FUplinkToolContext& Ctx, FString& OutError)
	{
		const FString Path = GetString(Ctx.Params, TEXT("blueprint"));
		if (Path.IsEmpty())
		{
			OutError = TEXT("'blueprint' (asset path, e.g. /Game/BP_Thing) is required");
			return nullptr;
		}
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path);
		if (!Blueprint)
		{
			OutError = FString::Printf(TEXT("blueprint not found: %s"), *Path);
		}
		return Blueprint;
	}

	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName, FString& OutError)
	{
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		if (GraphName.IsEmpty())
		{
			if (Blueprint->UbergraphPages.Num() > 0)
			{
				return Blueprint->UbergraphPages[0];
			}
			OutError = TEXT("blueprint has no event graph; pass 'graph'");
			return nullptr;
		}
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
		TArray<FString> Names;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph)
			{
				Names.Add(Graph->GetName());
			}
		}
		OutError = FString::Printf(TEXT("graph '%s' not found. Available: %s"), *GraphName, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	bool MakePinType(const FString& TypeString, FEdGraphPinType& Out, FString& OutError)
	{
		Out = FEdGraphPinType();

		FString Inner = TypeString;
		if (Inner.StartsWith(TEXT("array:")))
		{
			Out.ContainerType = EPinContainerType::Array;
			Inner = Inner.RightChop(6);
		}
		else if (Inner.StartsWith(TEXT("set:")))
		{
			Out.ContainerType = EPinContainerType::Set;
			Inner = Inner.RightChop(4);
		}
		else if (Inner.StartsWith(TEXT("map:")))
		{
			// map:<key>:<value> - the value type rides in the terminal category.
			Out.ContainerType = EPinContainerType::Map;
			const FString Spec = Inner.RightChop(4);
			FString KeyPart, ValuePart;
			if (!Spec.Split(TEXT(":"), &KeyPart, &ValuePart)
				|| KeyPart.IsEmpty() || ValuePart.IsEmpty())
			{
				OutError = TEXT("map needs both halves: map:<key>:<value>, e.g. map:name:float");
				return false;
			}
			// Object/struct specs contain their own colon (object:/Script/...),
			// so re-join everything after the first split point for the key when
			// the key itself is a prefixed form.
			if ((KeyPart == TEXT("object") || KeyPart == TEXT("class") || KeyPart == TEXT("struct")
				|| KeyPart == TEXT("enum") || KeyPart == TEXT("soft_object") || KeyPart == TEXT("soft_class")))
			{
				// key is prefixed: find the value after the key's own payload.
				// Simplest honest rule: prefixed keys are not supported - keys in
				// Blueprint maps are almost always name/string/int anyway.
				OutError = TEXT("map keys must be simple types (bool,int,int64,float,string,name,text,byte). Prefixed key types are not supported.");
				return false;
			}
			FEdGraphPinType ValueType;
			if (!MakePinType(ValuePart, ValueType, OutError))
			{
				return false;
			}
			if (ValueType.ContainerType != EPinContainerType::None)
			{
				OutError = TEXT("a map value cannot itself be a container");
				return false;
			}
			Out.PinValueType = FEdGraphTerminalType::FromPinType(ValueType);
			Inner = KeyPart;
		}

		if (Inner == TEXT("bool")) { Out.PinCategory = UEdGraphSchema_K2::PC_Boolean; }
		else if (Inner == TEXT("int")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int; }
		else if (Inner == TEXT("int64")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int64; }
		else if (Inner == TEXT("float") || Inner == TEXT("double"))
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Real;
			Out.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		else if (Inner == TEXT("string")) { Out.PinCategory = UEdGraphSchema_K2::PC_String; }
		else if (Inner == TEXT("name")) { Out.PinCategory = UEdGraphSchema_K2::PC_Name; }
		else if (Inner == TEXT("text")) { Out.PinCategory = UEdGraphSchema_K2::PC_Text; }
		else if (Inner == TEXT("byte")) { Out.PinCategory = UEdGraphSchema_K2::PC_Byte; }
		else if (Inner == TEXT("vector"))
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		}
		else if (Inner == TEXT("rotator"))
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		}
		else if (Inner == TEXT("transform"))
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		}
		else if (Inner.StartsWith(TEXT("object:")) || Inner.StartsWith(TEXT("class:")))
		{
			const bool bClassPin = Inner.StartsWith(TEXT("class:"));
			const FString ClassPath = Inner.RightChop(bClassPin ? 6 : 7);
			UClass* Class = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
			if (!Class)
			{
				OutError = FString::Printf(TEXT("class not found: %s"), *ClassPath);
				return false;
			}
			Out.PinCategory = bClassPin ? UEdGraphSchema_K2::PC_Class : UEdGraphSchema_K2::PC_Object;
			Out.PinSubCategoryObject = Class;
		}
		else if (Inner.StartsWith(TEXT("struct:")))
		{
			const FString StructPath = Inner.RightChop(7);
			UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
			if (!Struct)
			{
				OutError = FString::Printf(TEXT("struct not found: %s"), *StructPath);
				return false;
			}
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = Struct;
		}
		else if (Inner.StartsWith(TEXT("enum:")))
		{
			const FString EnumPath = Inner.RightChop(5);
			UEnum* Enum = LoadObject<UEnum>(nullptr, *EnumPath);
			if (!Enum)
			{
				OutError = FString::Printf(TEXT("enum not found: %s (e.g. /Script/Engine.ECollisionChannel or /Game/E_State.E_State)"), *EnumPath);
				return false;
			}
			// Byte-backed, the way the editor types an enum variable.
			Out.PinCategory = UEdGraphSchema_K2::PC_Byte;
			Out.PinSubCategoryObject = Enum;
		}
		else if (Inner.StartsWith(TEXT("soft_object:")) || Inner.StartsWith(TEXT("soft_class:")))
		{
			const bool bClassPin = Inner.StartsWith(TEXT("soft_class:"));
			const FString ClassPath = Inner.RightChop(bClassPin ? 11 : 12);
			UClass* Class = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
			if (!Class)
			{
				OutError = FString::Printf(TEXT("class not found: %s"), *ClassPath);
				return false;
			}
			Out.PinCategory = bClassPin ? UEdGraphSchema_K2::PC_SoftClass : UEdGraphSchema_K2::PC_SoftObject;
			Out.PinSubCategoryObject = Class;
		}
		else
		{
			OutError = FString::Printf(TEXT("unknown type '%s' (bool,int,int64,float,string,name,text,byte,vector,rotator,transform,object:<class>,class:<class>,soft_object:<class>,soft_class:<class>,struct:<path>,enum:<path>,array:<inner>,set:<inner>,map:<key>:<value>)"), *TypeString);
			return false;
		}
		return true;
	}

	FUplinkToolResult CompileAndReport(UBlueprint* Blueprint)
	{
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

		TArray<TSharedPtr<FJsonValue>> Messages;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("severity"),
				Message->GetSeverity() == EMessageSeverity::Error ? TEXT("error") :
				Message->GetSeverity() == EMessageSeverity::Warning ? TEXT("warning") : TEXT("info"));
			Row->SetStringField(TEXT("text"), Message->ToText().ToString());
			Messages.Add(MakeShared<FJsonValueObject>(Row));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Data->SetNumberField(TEXT("errors"), Results.NumErrors);
		Data->SetNumberField(TEXT("warnings"), Results.NumWarnings);
		Data->SetArrayField(TEXT("messages"), Messages);

		FUplinkToolResult Out = FUplinkToolResult::Ok(Data,
			Results.NumErrors == 0 ? TEXT("compiled") : TEXT("compile FAILED"));
		Out.bError = Results.NumErrors > 0;
		return Out;
	}
}
