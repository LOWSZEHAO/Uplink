// Copyright (c) 2026 Low Sze Hao. MIT License.
// Blueprint tools: bp_create, bp_query, bp_modify, bp_compile.

#include "UplinkTools.h"
#include "UplinkCompat.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "JsonObjectConverter.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

using namespace UplinkToolUtil;

namespace
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

	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidString, FString& OutError)
	{
		FGuid Guid;
		if (!FGuid::Parse(GuidString, Guid))
		{
			OutError = FString::Printf(TEXT("'%s' is not a node guid (get guids from bp_query)"), *GuidString);
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == Guid)
			{
				return Node;
			}
		}
		OutError = FString::Printf(TEXT("node %s not found in graph %s"), *GuidString, *Graph->GetName());
		return nullptr;
	}

	UEdGraphPin* FindPinLoose(UEdGraphNode* Node, const FString& PinName, FString& OutError)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		TArray<FString> Names;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin)
			{
				Names.Add(Pin->PinName.ToString());
			}
		}
		OutError = FString::Printf(TEXT("pin '%s' not found. Pins: %s (exec pins are 'execute' and 'then')"),
			*PinName, *FString::Join(Names, TEXT(", ")));
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
		else
		{
			OutError = FString::Printf(TEXT("unknown type '%s' (bool,int,int64,float,string,name,text,byte,vector,rotator,transform,object:<class>,class:<class>,struct:<path>,array:<inner>)"), *TypeString);
			return false;
		}
		return true;
	}

	void FinalizeNewNode(UEdGraph* Graph, UEdGraphNode* Node, const FUplinkToolContext& Ctx)
	{
		Graph->Modify();
		Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		Node->NodePosX = static_cast<int32>(GetNumber(Ctx.Params, TEXT("x"), 0));
		Node->NodePosY = static_cast<int32>(GetNumber(Ctx.Params, TEXT("y"), 0));
	}

	TSharedRef<FJsonObject> NodeToJson(const UEdGraphNode* Node)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Out->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Out->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Out->SetNumberField(TEXT("x"), Node->NodePosX);
		Out->SetNumberField(TEXT("y"), Node->NodePosY);

		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden)
			{
				continue;
			}
			TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
			PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
			PinJson->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			if (!Pin->DefaultValue.IsEmpty())
			{
				PinJson->SetStringField(TEXT("default"), Pin->DefaultValue);
			}
			if (Pin->LinkedTo.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Links;
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode())
					{
						Links.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s:%s"),
							*Linked->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
							*Linked->PinName.ToString())));
					}
				}
				PinJson->SetArrayField(TEXT("links"), Links);
			}
			Pins.Add(MakeShared<FJsonValueObject>(PinJson));
		}
		Out->SetArrayField(TEXT("pins"), Pins);
		return Out;
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

void UplinkTools::RegisterBlueprint(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("bp_create"),
		TEXT("Create a new Blueprint asset (in memory, marked dirty; save from the editor or with console_command 'SaveDirtyPackages'). parent_class e.g. /Script/Engine.Actor."),
		TEXT(R"json({"type":"object","properties":{"path":{"type":"string","description":"Asset path, e.g. /Game/Tests/BP_Probe"},"parent_class":{"type":"string","default":"/Script/Engine.Actor"}},"required":["path"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Path = GetString(Ctx.Params, TEXT("path"));
			if (!Path.StartsWith(TEXT("/Game/")))
			{
				return FUplinkToolResult::Error(TEXT("'path' must start with /Game/"));
			}
			if (LoadObject<UBlueprint>(nullptr, *Path))
			{
				return FUplinkToolResult::Error(TEXT("an asset already exists at that path"));
			}

			UClass* Parent = StaticLoadClass(UObject::StaticClass(), nullptr,
				*GetString(Ctx.Params, TEXT("parent_class"), TEXT("/Script/Engine.Actor")));
			if (!Parent)
			{
				return FUplinkToolResult::Error(TEXT("parent_class not found"));
			}

			const FString AssetName = FPackageName::GetShortName(Path);
			UPackage* Package = CreatePackage(*Path);
			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
				Parent, Package, FName(*AssetName), BPTYPE_Normal,
				UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			if (!Blueprint)
			{
				return FUplinkToolResult::Error(TEXT("CreateBlueprint failed"));
			}
			FAssetRegistryModule::AssetCreated(Blueprint);
			Package->MarkPackageDirty();

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
			Data->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("bp_query"),
		TEXT("Inspect a Blueprint: parent class, variables, and graphs with their nodes, pins and connections (node guids are the handles bp_modify uses)."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"},"graph":{"type":"string","description":"Only this graph (default: all)"},"max_nodes":{"type":"number","default":100}},"required":["blueprint"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UBlueprint* Blueprint = LoadBlueprint(Ctx, Error);
			if (!Blueprint)
			{
				return FUplinkToolResult::Error(Error);
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
			Data->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
			Data->SetStringField(TEXT("status"),
				Blueprint->Status == BS_Error ? TEXT("error") :
				Blueprint->Status == BS_UpToDate ? TEXT("up_to_date") : TEXT("dirty"));

			TArray<TSharedPtr<FJsonValue>> Variables;
			for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Variable.VarName.ToString());
				Row->SetStringField(TEXT("type"), UEdGraphSchema_K2::TypeToText(Variable.VarType).ToString());
				if (!Variable.DefaultValue.IsEmpty())
				{
					Row->SetStringField(TEXT("default"), Variable.DefaultValue);
				}
				Variables.Add(MakeShared<FJsonValueObject>(Row));
			}
			Data->SetArrayField(TEXT("variables"), Variables);

			if (Blueprint->SimpleConstructionScript)
			{
				TArray<TSharedPtr<FJsonValue>> Components;
				TFunction<void(USCS_Node*, const FString&)> AddNodeRow =
					[&Components, &AddNodeRow](USCS_Node* Node, const FString& ParentName)
				{
					if (!Node)
					{
						return;
					}
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
					Row->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT(""));
					if (!ParentName.IsEmpty())
					{
						Row->SetStringField(TEXT("parent"), ParentName);
					}
					Components.Add(MakeShared<FJsonValueObject>(Row));
					for (USCS_Node* Child : Node->GetChildNodes())
					{
						AddNodeRow(Child, Node->GetVariableName().ToString());
					}
				};
				for (USCS_Node* Root : Blueprint->SimpleConstructionScript->GetRootNodes())
				{
					AddNodeRow(Root, FString());
				}
				Data->SetArrayField(TEXT("components"), Components);
			}

			const FString GraphFilter = GetString(Ctx.Params, TEXT("graph"));
			const int32 MaxNodes = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max_nodes"), 100)), 1, 500);
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);

			TArray<TSharedPtr<FJsonValue>> Graphs;
			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph)
				{
					continue;
				}
				if (!GraphFilter.IsEmpty() && !Graph->GetName().Equals(GraphFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
				GraphJson->SetStringField(TEXT("name"), Graph->GetName());
				TArray<TSharedPtr<FJsonValue>> Nodes;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Nodes.Num() >= MaxNodes)
					{
						break;
					}
					if (Node)
					{
						Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
					}
				}
				GraphJson->SetArrayField(TEXT("nodes"), Nodes);
				GraphJson->SetBoolField(TEXT("truncated"), Graph->Nodes.Num() > Nodes.Num());
				Graphs.Add(MakeShared<FJsonValueObject>(GraphJson));
			}
			Data->SetArrayField(TEXT("graphs"), Graphs);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("bp_modify"),
		TEXT("Edit a Blueprint. op: add_variable {name,type,default?} | remove_variable {name} | add_node {kind: call_function {class,function} | custom_event {name} | event {name - e.g. ReceiveBeginPlay/ReceiveTick} | component_bound_event {component, event - e.g. component 'MyButton' event 'OnClicked', or 'NiagaraComp' + 'OnSystemFinished'} | variable_get/variable_set {name}, graph?, x?, y?} | connect {graph?, from_node, from_pin, to_node, to_pin} | break_links {graph?, node, pin} | delete_node {graph?, node} | set_pin_default {graph?, node, pin, value}. Node handles are guids from bp_query or the add_node response. Set compile=true to compile after the edit."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"},"op":{"type":"string","enum":["add_variable","remove_variable","add_node","connect","break_links","delete_node","set_pin_default"]},"name":{"type":"string"},"type":{"type":"string"},"default":{"type":"string"},"kind":{"type":"string","enum":["call_function","custom_event","event","component_bound_event","variable_get","variable_set"]},"class":{"type":"string","description":"add_node call_function: class path or 'self'"},"function":{"type":"string"},"component":{"type":"string","description":"component_bound_event: component/widget variable name"},"event":{"type":"string","description":"component_bound_event: delegate name on the component's class"},"graph":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"from_node":{"type":"string"},"from_pin":{"type":"string"},"to_node":{"type":"string"},"to_pin":{"type":"string"},"node":{"type":"string"},"pin":{"type":"string"},"value":{"type":"string"},"compile":{"type":"boolean"}},"required":["blueprint","op"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UBlueprint* Blueprint = LoadBlueprint(Ctx, Error);
			if (!Blueprint)
			{
				return FUplinkToolResult::Error(Error);
			}
			const FString Op = GetString(Ctx.Params, TEXT("op"));
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

			if (Op == TEXT("add_variable"))
			{
				FEdGraphPinType PinType;
				if (!MakePinType(GetString(Ctx.Params, TEXT("type"), TEXT("float")), PinType, Error))
				{
					return FUplinkToolResult::Error(Error);
				}
				const FName VarName(*GetString(Ctx.Params, TEXT("name")));
				if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType, GetString(Ctx.Params, TEXT("default"))))
				{
					return FUplinkToolResult::Error(TEXT("AddMemberVariable failed (name collision?)"));
				}
				Data->SetStringField(TEXT("variable"), VarName.ToString());
			}
			else if (Op == TEXT("remove_variable"))
			{
				FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*GetString(Ctx.Params, TEXT("name"))));
			}
			else if (Op == TEXT("add_node"))
			{
				UEdGraph* Graph = FindGraph(Blueprint, GetString(Ctx.Params, TEXT("graph")), Error);
				if (!Graph)
				{
					return FUplinkToolResult::Error(Error);
				}
				const FString Kind = GetString(Ctx.Params, TEXT("kind"));
				UEdGraphNode* NewNode = nullptr;

				if (Kind == TEXT("call_function"))
				{
					const FString ClassPath = GetString(Ctx.Params, TEXT("class"), TEXT("self"));
					UClass* TargetClass = nullptr;
					if (ClassPath == TEXT("self"))
					{
						TargetClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->ParentClass.Get();
					}
					else
					{
						TargetClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
					}
					if (!TargetClass)
					{
						return FUplinkToolResult::Error(TEXT("class not found for call_function"));
					}
					UFunction* Function = TargetClass->FindFunctionByName(FName(*GetString(Ctx.Params, TEXT("function"))));
					if (!Function)
					{
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("function '%s' not found on %s"), *GetString(Ctx.Params, TEXT("function")), *TargetClass->GetName()));
					}
					UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
					Node->SetFromFunction(Function);
					NewNode = Node;
				}
				else if (Kind == TEXT("custom_event"))
				{
					UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
					Node->CustomFunctionName = FName(*GetString(Ctx.Params, TEXT("name"), TEXT("CustomEvent")));
					NewNode = Node;
				}
				else if (Kind == TEXT("event"))
				{
					UK2Node_Event* Node = NewObject<UK2Node_Event>(Graph);
					Node->EventReference.SetExternalMember(FName(*GetString(Ctx.Params, TEXT("name"))), Blueprint->ParentClass);
					Node->bOverrideFunction = true;
					NewNode = Node;
				}
				else if (Kind == TEXT("component_bound_event"))
				{
					// Bind a component's (or widget's) delegate as an event node -
					// the graph equivalent of clicking + on OnClicked/OnSystemFinished.
					UClass* OwnerClass = Blueprint->SkeletonGeneratedClass
						? Blueprint->SkeletonGeneratedClass.Get()
						: (Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->ParentClass.Get());
					FObjectProperty* ComponentProperty =
						FindFProperty<FObjectProperty>(OwnerClass, *GetString(Ctx.Params, TEXT("component")));
					if (!ComponentProperty)
					{
						TArray<FString> Names;
						for (TFieldIterator<FObjectProperty> It(OwnerClass); It && Names.Num() < 40; ++It)
						{
							Names.Add(It->GetName());
						}
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("component property '%s' not found (widgets must have Is Variable set). Object properties: %s"),
							*GetString(Ctx.Params, TEXT("component")), *FString::Join(Names, TEXT(", "))));
					}
					FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(
						ComponentProperty->PropertyClass, *GetString(Ctx.Params, TEXT("event")));
					if (!DelegateProperty)
					{
						TArray<FString> Names;
						for (TFieldIterator<FMulticastDelegateProperty> It(ComponentProperty->PropertyClass); It; ++It)
						{
							Names.Add(It->GetName());
						}
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("event '%s' not found on %s. Delegates: %s"),
							*GetString(Ctx.Params, TEXT("event")), *ComponentProperty->PropertyClass->GetName(),
							*FString::Join(Names, TEXT(", "))));
					}
					UK2Node_ComponentBoundEvent* Node = NewObject<UK2Node_ComponentBoundEvent>(Graph);
					Node->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
					NewNode = Node;
				}
				else if (Kind == TEXT("variable_get") || Kind == TEXT("variable_set"))
				{
					const FName VarName(*GetString(Ctx.Params, TEXT("name")));
					if (Kind == TEXT("variable_get"))
					{
						UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Graph);
						Node->VariableReference.SetSelfMember(VarName);
						NewNode = Node;
					}
					else
					{
						UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Graph);
						Node->VariableReference.SetSelfMember(VarName);
						NewNode = Node;
					}
				}
				else
				{
					return FUplinkToolResult::Error(TEXT("kind must be call_function, custom_event, event, variable_get, or variable_set"));
				}

				FinalizeNewNode(Graph, NewNode, Ctx);
				Data->SetObjectField(TEXT("node"), NodeToJson(NewNode));
			}
			else if (Op == TEXT("connect"))
			{
				UEdGraph* Graph = FindGraph(Blueprint, GetString(Ctx.Params, TEXT("graph")), Error);
				if (!Graph)
				{
					return FUplinkToolResult::Error(Error);
				}
				UEdGraphNode* FromNode = FindNodeByGuid(Graph, GetString(Ctx.Params, TEXT("from_node")), Error);
				UEdGraphNode* ToNode = FromNode ? FindNodeByGuid(Graph, GetString(Ctx.Params, TEXT("to_node")), Error) : nullptr;
				if (!FromNode || !ToNode)
				{
					return FUplinkToolResult::Error(Error);
				}
				UEdGraphPin* FromPin = FindPinLoose(FromNode, GetString(Ctx.Params, TEXT("from_pin")), Error);
				UEdGraphPin* ToPin = FromPin ? FindPinLoose(ToNode, GetString(Ctx.Params, TEXT("to_pin")), Error) : nullptr;
				if (!FromPin || !ToPin)
				{
					return FUplinkToolResult::Error(Error);
				}
				const UEdGraphSchema* Schema = Graph->GetSchema();
				if (!Schema->TryCreateConnection(FromPin, ToPin))
				{
					const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
					return FUplinkToolResult::Error(FString::Printf(TEXT("connection rejected: %s"), *Response.Message.ToString()));
				}
			}
			else if (Op == TEXT("break_links") || Op == TEXT("delete_node") || Op == TEXT("set_pin_default"))
			{
				UEdGraph* Graph = FindGraph(Blueprint, GetString(Ctx.Params, TEXT("graph")), Error);
				if (!Graph)
				{
					return FUplinkToolResult::Error(Error);
				}
				UEdGraphNode* Node = FindNodeByGuid(Graph, GetString(Ctx.Params, TEXT("node")), Error);
				if (!Node)
				{
					return FUplinkToolResult::Error(Error);
				}

				if (Op == TEXT("delete_node"))
				{
					FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
				}
				else
				{
					UEdGraphPin* Pin = FindPinLoose(Node, GetString(Ctx.Params, TEXT("pin")), Error);
					if (!Pin)
					{
						return FUplinkToolResult::Error(Error);
					}
					if (Op == TEXT("break_links"))
					{
						Graph->GetSchema()->BreakPinLinks(*Pin, /*bSendsNodeNotification=*/true);
					}
					else // set_pin_default
					{
						const FString Value = GetString(Ctx.Params, TEXT("value"));
						if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
						{
							UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Value);
							if (!Object)
							{
								return FUplinkToolResult::Error(FString::Printf(TEXT("object not found: %s"), *Value));
							}
							Graph->GetSchema()->TrySetDefaultObject(*Pin, Object);
						}
						else
						{
							Graph->GetSchema()->TrySetDefaultValue(*Pin, Value);
						}
					}
				}
			}
			else
			{
				return FUplinkToolResult::Error(TEXT("unknown op (add_variable, remove_variable, add_node, connect, break_links, delete_node, set_pin_default)"));
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

			bool bCompile = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("compile")), bCompile);
			if (bCompile)
			{
				FUplinkToolResult CompileResult = CompileAndReport(Blueprint);
				if (CompileResult.Data.IsValid())
				{
					Data->SetObjectField(TEXT("compile"), CompileResult.Data);
				}
				FUplinkToolResult Out = FUplinkToolResult::Ok(Data, CompileResult.Message);
				Out.bError = CompileResult.bError;
				return Out;
			}
			return FUplinkToolResult::Ok(Data, TEXT("modified (not compiled)"));
		});

	Registry.RegisterQuick(
		TEXT("bp_add_component"),
		TEXT("Add a component to a Blueprint's construction script (like the editor's Add Component button). 'class': component class - short name ('StaticMeshComponent', 'BoxComponent') or full path. 'parent': attach under this existing component (default: the scene root for scene components). Conveniences on the template: 'location'/'rotation'/'scale' (relative), 'static_mesh' (asset path), 'collision_profile' (e.g. 'OverlapOnlyPawn', 'BlockAll'), plus 'properties' as a generic name->JSON map. The component becomes a Blueprint variable of the same name (usable by bp_modify's component_bound_event after a compile - set compile:true)."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"},"class":{"type":"string"},"name":{"type":"string"},"parent":{"type":"string"},"location":{"type":"object"},"rotation":{"type":"object"},"scale":{"type":"object"},"static_mesh":{"type":"string"},"collision_profile":{"type":"string"},"properties":{"type":"object"},"compile":{"type":"boolean","default":false}},"required":["blueprint","class","name"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UBlueprint* Blueprint = LoadBlueprint(Ctx, Error);
			if (!Blueprint)
			{
				return FUplinkToolResult::Error(Error);
			}
			if (!Blueprint->SimpleConstructionScript)
			{
				return FUplinkToolResult::Error(TEXT("blueprint has no construction script (not an actor blueprint?)"));
			}
			USimpleConstructionScript* Scs = Blueprint->SimpleConstructionScript;

			// Resolve the component class: full path, or short name under /Script/Engine.
			FString ClassSpec = GetString(Ctx.Params, TEXT("class"));
			UClass* Class = nullptr;
			if (ClassSpec.StartsWith(TEXT("/")))
			{
				Class = StaticLoadClass(UActorComponent::StaticClass(), nullptr, *ClassSpec);
			}
			else
			{
				Class = StaticLoadClass(UActorComponent::StaticClass(), nullptr,
					*FString::Printf(TEXT("/Script/Engine.%s"), *ClassSpec));
			}
			if (!Class)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("component class not found: %s (use a short engine name like 'StaticMeshComponent' or a full path like /Script/UMG.WidgetComponent)"), *ClassSpec));
			}
			if (Class->HasAnyClassFlags(CLASS_Abstract))
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("%s is abstract"), *Class->GetName()));
			}

			const FString Name = GetString(Ctx.Params, TEXT("name"));
			if (Name.IsEmpty() || Scs->FindSCSNode(FName(*Name)))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("component name '%s' is empty or already used in this blueprint"), *Name));
			}

			const bool bScene = Class->IsChildOf(USceneComponent::StaticClass());
			const FString ParentName = GetString(Ctx.Params, TEXT("parent"));
			USCS_Node* ParentNode = nullptr;
			if (!ParentName.IsEmpty())
			{
				if (!bScene)
				{
					return FUplinkToolResult::Error(TEXT("'parent' only applies to scene components"));
				}
				ParentNode = Scs->FindSCSNode(FName(*ParentName));
				if (!ParentNode)
				{
					TArray<FString> Names;
					for (const USCS_Node* Existing : Scs->GetAllNodes())
					{
						if (Existing)
						{
							Names.Add(Existing->GetVariableName().ToString());
						}
					}
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("parent component '%s' not found. Components: %s"),
						*ParentName, *FString::Join(Names, TEXT(", "))));
				}
			}

			Blueprint->Modify();
			USCS_Node* Node = Scs->CreateNode(Class, FName(*Name));
			if (!Node)
			{
				return FUplinkToolResult::Error(TEXT("CreateNode failed"));
			}
			if (ParentNode)
			{
				ParentNode->AddChildNode(Node);
			}
			else if (bScene && Scs->GetDefaultSceneRootNode())
			{
				Scs->GetDefaultSceneRootNode()->AddChildNode(Node);
			}
			else
			{
				Scs->AddNode(Node);
			}

			// Apply template setup. Errors past this point report but keep the node.
			TArray<FString> Applied;
			TArray<FString> Failed;
			UActorComponent* Template = Node->ComponentTemplate;

			FVector Location, Scale;
			FRotator Rotation;
			if (USceneComponent* SceneTemplate = Cast<USceneComponent>(Template))
			{
				if (TryGetVector(Ctx.Params, TEXT("location"), Location))
				{
					SceneTemplate->SetRelativeLocation_Direct(Location);
					Applied.Add(TEXT("location"));
				}
				if (TryGetRotator(Ctx.Params, TEXT("rotation"), Rotation))
				{
					SceneTemplate->SetRelativeRotation_Direct(Rotation);
					Applied.Add(TEXT("rotation"));
				}
				if (TryGetVector(Ctx.Params, TEXT("scale"), Scale))
				{
					SceneTemplate->SetRelativeScale3D_Direct(Scale);
					Applied.Add(TEXT("scale"));
				}
			}

			const FString MeshPath = GetString(Ctx.Params, TEXT("static_mesh"));
			if (!MeshPath.IsEmpty())
			{
				UStaticMeshComponent* MeshTemplate = Cast<UStaticMeshComponent>(Template);
				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
				if (MeshTemplate && Mesh)
				{
					MeshTemplate->SetStaticMesh(Mesh);
					Applied.Add(TEXT("static_mesh"));
				}
				else
				{
					Failed.Add(FString::Printf(TEXT("static_mesh: %s"),
						Mesh ? TEXT("component is not a StaticMeshComponent") : TEXT("mesh asset not found")));
				}
			}

			const FString Profile = GetString(Ctx.Params, TEXT("collision_profile"));
			if (!Profile.IsEmpty())
			{
				if (UPrimitiveComponent* PrimitiveTemplate = Cast<UPrimitiveComponent>(Template))
				{
					PrimitiveTemplate->SetCollisionProfileName(FName(*Profile));
					Applied.Add(TEXT("collision_profile"));
				}
				else
				{
					Failed.Add(TEXT("collision_profile: component is not a PrimitiveComponent"));
				}
			}

			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (Ctx.Params->TryGetObjectField(FStringView(TEXT("properties")), Properties) && Properties->IsValid())
			{
				for (const auto& Pair : (*Properties)->Values)
				{
					const FString PropertyName = UplinkCompat::JsonKeyToString(Pair.Key);
					FProperty* Property = FindFProperty<FProperty>(Template->GetClass(), *PropertyName);
					if (!Property)
					{
						Failed.Add(FString::Printf(TEXT("%s: no such property on %s"), *PropertyName, *Class->GetName()));
						continue;
					}
					if (FJsonObjectConverter::JsonValueToUProperty(
						Pair.Value, Property, Property->ContainerPtrToValuePtr<void>(Template)))
					{
						Applied.Add(PropertyName);
					}
					else
					{
						Failed.Add(FString::Printf(TEXT("%s: JSON conversion failed"), *PropertyName));
					}
				}
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
			Data->SetStringField(TEXT("component"), Name);
			Data->SetStringField(TEXT("class"), Class->GetName());
			Data->SetStringField(TEXT("parent"), ParentNode
				? ParentNode->GetVariableName().ToString()
				: (bScene && Scs->GetDefaultSceneRootNode() && Node != Scs->GetDefaultSceneRootNode()
					? Scs->GetDefaultSceneRootNode()->GetVariableName().ToString() : TEXT("")));
			if (Applied.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FString& Entry : Applied) { Values.Add(MakeShared<FJsonValueString>(Entry)); }
				Data->SetArrayField(TEXT("applied"), Values);
			}
			if (Failed.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FString& Entry : Failed) { Values.Add(MakeShared<FJsonValueString>(Entry)); }
				Data->SetArrayField(TEXT("failed"), Values);
			}

			bool bCompile = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("compile")), bCompile);
			if (bCompile)
			{
				FUplinkToolResult CompileResult = CompileAndReport(Blueprint);
				if (CompileResult.Data.IsValid())
				{
					Data->SetObjectField(TEXT("compile"), CompileResult.Data);
				}
				FUplinkToolResult Out = FUplinkToolResult::Ok(Data, CompileResult.Message);
				Out.bError = CompileResult.bError;
				return Out;
			}
			return FUplinkToolResult::Ok(Data, TEXT("component added (not compiled)"));
		});

	Registry.RegisterQuick(
		TEXT("bp_compile"),
		TEXT("Compile a Blueprint and return its errors and warnings."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"}},"required":["blueprint"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UBlueprint* Blueprint = LoadBlueprint(Ctx, Error);
			if (!Blueprint)
			{
				return FUplinkToolResult::Error(Error);
			}
			return CompileAndReport(Blueprint);
		});
}
