// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// add_node - one branch per node kind - and set_node_property, which reaches
// into a placed node's own settings.

#include "Blueprint/UplinkBlueprintCommon.h"
#include "UplinkToolUtil.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "JsonObjectConverter.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_GetSubsystem.h"
#include "EditorSubsystem.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "InputAction.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "K2Node_Timeline.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_Select.h"
#include "K2Node_Self.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"

using namespace UplinkToolUtil;

namespace UplinkBlueprint
{
	/**
	 * Custom event parameters, added once the node is in the graph so that
	 * AllocateDefaultPins has already run.
	 *
	 * They are OUTPUT pins: the data flows out of the event into the graph.
	 * UK2Node_CustomEvent::CanCreateUserDefinedPin refuses EGPD_Input outright
	 * ("Cannot add input pins to custom event node!"), so a caller asking for
	 * an input direction is an error rather than something to quietly correct.
	 *
	 * Returns an empty string on success, otherwise the reason.
	 */
	FString AddCustomEventParams(UK2Node_CustomEvent* Node, const TSharedPtr<FJsonObject>& OpParams)
	{
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (!OpParams->TryGetArrayField(FStringView(TEXT("inputs")), Inputs))
		{
			return FString();
		}

		for (const TSharedPtr<FJsonValue>& Value : *Inputs)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Value->TryGetObject(Obj) || !Obj->IsValid())
			{
				continue;
			}
			const FString PinName = GetString(*Obj, TEXT("name"));
			if (PinName.IsEmpty())
			{
				return TEXT("every entry in 'inputs' needs a 'name'");
			}

			FString Error;
			FEdGraphPinType PinType;
			if (!MakePinType(GetString(*Obj, TEXT("type"), TEXT("float")), PinType, Error))
			{
				return FString::Printf(TEXT("input '%s': %s"), *PinName, *Error);
			}
			bool bByRef = false;
			(*Obj)->TryGetBoolField(FStringView(TEXT("by_ref")), bByRef);
			PinType.bIsReference = bByRef;

			// Ask the node itself, so a type it will not take reports the
			// engine's own reason instead of becoming a pin that never appears.
			FText Refusal;
			if (!Node->CanCreateUserDefinedPin(PinType, EGPD_Output, Refusal))
			{
				return FString::Printf(TEXT("input '%s': %s"), *PinName, *Refusal.ToString());
			}
			if (!Node->CreateUserDefinedPin(FName(*PinName), PinType, EGPD_Output))
			{
				// CreateUserDefinedPin hands back whatever the virtual
				// CreatePinFromUserDefinition returns. A null means the entry
				// went into UserDefinedPins with no pin on the node to show
				// for it - recorded, invisible, and compiled into nothing.
				return FString::Printf(TEXT("input '%s': the node did not create the pin"), *PinName);
			}
		}

		// What the editor does after the same edit, so pin order and state
		// match a parameter added by hand in the details panel.
		Node->ReconstructNode();
		return FString();
	}

	FUplinkToolResult ApplyAddNodeOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TMap<FString, FString>& NodeRefs, TSharedRef<FJsonObject> Data)
	{
		FString Error;

		UEdGraph* Graph = FindGraph(Blueprint, GetString(OpParams, TEXT("graph")), Error);
		if (!Graph)
		{
			return FUplinkToolResult::Error(Error);
		}
		const FString Kind = GetString(OpParams, TEXT("kind"));
		UEdGraphNode* NewNode = nullptr;
		UEdGraphNode* ResultNode = nullptr;

		if (Kind == TEXT("call_function"))
		{
			const FString ClassPath = GetString(OpParams, TEXT("class"), TEXT("self"));
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
			UFunction* Function = TargetClass->FindFunctionByName(FName(*GetString(OpParams, TEXT("function"))));
			if (!Function)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("function '%s' not found on %s"), *GetString(OpParams, TEXT("function")), *TargetClass->GetName()));
			}
			// The node class matters, not just the function. Array_Add and
			// friends declare a typed TArray parameter, and only
			// UK2Node_CallArrayFunction turns that into a wildcard that
			// propagates the connected type - on a plain call node the pin is
			// hard int32, the schema rejects everything else, and nothing
			// says why. Same story for the DataTable row pin. This mirrors
			// how the palette picks (BlueprintFunctionNodeSpawner).
			TSubclassOf<UK2Node_CallFunction> NodeClass = UK2Node_CallFunction::StaticClass();
			if (Function->HasMetaData(FBlueprintMetadata::MD_DataTablePin))
			{
				NodeClass = UK2Node_CallDataTableFunction::StaticClass();
			}
			else if (Function->HasMetaData(FBlueprintMetadata::MD_ArrayParam))
			{
				NodeClass = UK2Node_CallArrayFunction::StaticClass();
			}
			UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph, NodeClass);
			Node->SetFromFunction(Function);
			NewNode = Node;
		}
		else if (Kind == TEXT("custom_event"))
		{
			UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
			Node->CustomFunctionName = FName(*GetString(OpParams, TEXT("name"), TEXT("CustomEvent")));
			NewNode = Node;
		}
		else if (Kind == TEXT("get_subsystem"))
		{
			const FString ClassPath = GetString(OpParams, TEXT("class"));
			UClass* SubsystemClass = ClassPath.IsEmpty()
				? nullptr
				: StaticLoadClass(USubsystem::StaticClass(), nullptr, *ClassPath);
			if (!SubsystemClass)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("subsystem class not found: '%s' - pass a concrete subsystem class, ")
					TEXT("e.g. /Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem"), *ClassPath));
			}

			// Which of the four node classes to build. This mirrors the
			// dispatch the base node's own ExpandNode does, because the node
			// and the family have to agree: a class the chosen node does not
			// handle reaches the compiler as "Node @@ must have a class
			// specified", which names neither the real cause nor the fix.
			const TCHAR* NodeClassPath = nullptr;
			if (SubsystemClass->IsChildOf(UEngineSubsystem::StaticClass()))
			{
				NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_GetEngineSubsystem");
			}
			else if (SubsystemClass->IsChildOf(UEditorSubsystem::StaticClass()))
			{
				NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_GetEditorSubsystem");
			}
			else
			{
				// The base node covers the rest. AudioEngineSubsystem lives in
				// AudioMixer and is resolved by path rather than linked, so
				// this file does not take a module dependency for one branch.
				const UClass* AudioBase = FindObject<UClass>(nullptr, TEXT("/Script/AudioMixer.AudioEngineSubsystem"));
				const bool bBaseHandlesIt =
					SubsystemClass->IsChildOf(UGameInstanceSubsystem::StaticClass())
					|| SubsystemClass->IsChildOf(UWorldSubsystem::StaticClass())
					|| SubsystemClass->IsChildOf(ULocalPlayerSubsystem::StaticClass())
					|| (AudioBase && SubsystemClass->IsChildOf(AudioBase));
				if (!bBaseHandlesIt)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' is not a subsystem this node can get. It must derive from one of: ")
						TEXT("GameInstanceSubsystem, WorldSubsystem, LocalPlayerSubsystem, AudioEngineSubsystem, ")
						TEXT("EngineSubsystem or EditorSubsystem."), *SubsystemClass->GetPathName()));
				}
				NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_GetSubsystem");
			}

			// Found by path, not by StaticClass(): only the base is declared
			// MinimalAPI, so the three derived node classes have no exported
			// symbol to link against from outside BlueprintGraph.
			UClass* NodeClass = FindObject<UClass>(nullptr, NodeClassPath);
			if (!NodeClass)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("node class %s is not loaded"), NodeClassPath));
			}

			UK2Node_GetSubsystem* Node = NewObject<UK2Node_GetSubsystem>(Graph, NodeClass);
			// Before the node is placed: AllocateDefaultPins reads the class
			// to type the return pin, and adds a loose Class input pin when it
			// is unset. Setting it afterwards leaves a node that returns the
			// abstract USubsystem and carries a pin nobody asked for.
			Node->Initialize(SubsystemClass);
			NewNode = Node;
		}
		else if (Kind == TEXT("enhanced_input"))
		{
			// The action has to be resolved before the node is placed:
			// AllocateDefaultPins reads InputAction to type the ActionValue pin
			// and to create the action pin at all, and FinalizeNewNode is what
			// calls it. Setting the action afterwards leaves a node whose pins
			// describe no action.
			const FString ActionPath = GetString(OpParams, TEXT("action"));
			const UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
			if (!Action)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("input action not found: '%s' - pass an Input Action asset path, e.g. /Game/Input/Actions/IA_Jump"),
					*ActionPath));
			}

			// One event node per action per graph. A second one is a compile
			// error, and the editor's own spawner reuses rather than stacking,
			// so match that instead of producing a graph that will not build.
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				UK2Node_EnhancedInputAction* Existing = Cast<UK2Node_EnhancedInputAction>(GraphNode);
				if (Existing && Existing->InputAction == Action)
				{
					Existing->Modify();
					Existing->SetEnabledState(ENodeEnabledState::Enabled);
					Data->SetObjectField(TEXT("node"), NodeToJson(Existing));
					Data->SetBoolField(TEXT("reused"), true);
					ResultNode = Existing;
					break;
				}
			}
			if (!ResultNode)
			{
				UK2Node_EnhancedInputAction* Node = NewObject<UK2Node_EnhancedInputAction>(Graph);
				Node->InputAction = Action;
				NewNode = Node;
			}
		}
		else if (Kind == TEXT("event"))
		{
			// New actor blueprints carry disabled ghost placeholders for the
			// common events; reuse (and enable) a matching one instead of
			// stacking a duplicate on top of it.
			const FName EventName(*GetString(OpParams, TEXT("name")));
			// Without this, a missing or misspelt 'name' built a nameless event
			// node that compiles to nothing and reads as broken in the editor.
			if (EventName.IsNone())
			{
				return FUplinkToolResult::Error(
					TEXT("add_node kind 'event' needs 'name' (the function being overridden, ")
					TEXT("e.g. ReceiveBeginPlay). For a new event of your own, use kind 'custom_event'."));
			}
			{
				UClass* SearchClass = Blueprint->SkeletonGeneratedClass
					? Blueprint->SkeletonGeneratedClass.Get()
					: Blueprint->ParentClass.Get();
				if (SearchClass && !SearchClass->FindFunctionByName(EventName))
				{
					TArray<FString> Candidates;
					for (TFieldIterator<UFunction> It(SearchClass, EFieldIteratorFlags::IncludeSuper);
						It && Candidates.Num() < 40; ++It)
					{
						if (UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(*It))
						{
							Candidates.AddUnique(It->GetName());
						}
					}
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no event '%s' on %s. Available: %s"),
						*EventName.ToString(), *SearchClass->GetName(), *FString::Join(Candidates, TEXT(", "))));
				}
			}
			UK2Node_Event* Existing = nullptr;
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				UK2Node_Event* Event = Cast<UK2Node_Event>(GraphNode);
				if (Event && Event->bOverrideFunction && Event->EventReference.GetMemberName() == EventName)
				{
					Existing = Event;
					break;
				}
			}
			if (Existing)
			{
				Existing->Modify();
				Existing->SetEnabledState(ENodeEnabledState::Enabled);
				Data->SetObjectField(TEXT("node"), NodeToJson(Existing));
				Data->SetBoolField(TEXT("reused"), true);
				ResultNode = Existing;
			}
			else
			{
				// The declaring class must be used here rather than ParentClass.
				// Interface events are not reachable through the normal
				// parent-class lookup: FindFunctionByName above accepts them,
				// because UClass searches implemented interfaces, while the
				// reference resolves along the UStruct super chain, which never
				// contains one. Anchoring to ParentClass therefore produced a
				// node that reported success and could never resolve.
				UClass* DeclaringClass = Blueprint->ParentClass;
				if (DeclaringClass && !FindUField<UFunction>(DeclaringClass, EventName))
				{
					for (const FBPInterfaceDescription& Implemented : Blueprint->ImplementedInterfaces)
					{
						if (Implemented.Interface && FindUField<UFunction>(Implemented.Interface, EventName))
						{
							DeclaringClass = Implemented.Interface;
							break;
						}
					}
				}

				UK2Node_Event* Node = NewObject<UK2Node_Event>(Graph);
				Node->EventReference.SetExternalMember(EventName, DeclaringClass);
				Node->bOverrideFunction = true;
				NewNode = Node;
			}
		}
		else if (Kind == TEXT("component_bound_event"))
		{
			// Bind a component's (or widget's) delegate as an event node -
			// the graph equivalent of clicking + on OnClicked/OnSystemFinished.
			UClass* OwnerClass = Blueprint->SkeletonGeneratedClass
				? Blueprint->SkeletonGeneratedClass.Get()
				: (Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->ParentClass.Get());
			FObjectProperty* ComponentProperty =
				FindFProperty<FObjectProperty>(OwnerClass, *GetString(OpParams, TEXT("component")));
			if (!ComponentProperty)
			{
				TArray<FString> Names;
				for (TFieldIterator<FObjectProperty> It(OwnerClass); It && Names.Num() < 40; ++It)
				{
					Names.Add(It->GetName());
				}
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("component property '%s' not found (widgets must have Is Variable set). Object properties: %s"),
					*GetString(OpParams, TEXT("component")), *FString::Join(Names, TEXT(", "))));
			}
			FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(
				ComponentProperty->PropertyClass, *GetString(OpParams, TEXT("event")));
			if (!DelegateProperty)
			{
				TArray<FString> Names;
				for (TFieldIterator<FMulticastDelegateProperty> It(ComponentProperty->PropertyClass); It; ++It)
				{
					Names.Add(It->GetName());
				}
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("event '%s' not found on %s. Delegates: %s"),
					*GetString(OpParams, TEXT("event")), *ComponentProperty->PropertyClass->GetName(),
					*FString::Join(Names, TEXT(", "))));
			}
			// One binding per component+event, like the editor's + button. A
			// second node compiles clean and fires the handler twice at
			// runtime, which is a miserable thing to debug from inside a game.
			if (const UK2Node_ComponentBoundEvent* Existing = FKismetEditorUtilities::FindBoundEventForComponent(
				Blueprint, DelegateProperty->GetFName(), ComponentProperty->GetFName()))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' on '%s' is already bound (node %s). Wire onto the existing node instead of stacking a second binding."),
					*DelegateProperty->GetName(), *ComponentProperty->GetName(),
					*Existing->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
			}
			UK2Node_ComponentBoundEvent* Node = NewObject<UK2Node_ComponentBoundEvent>(Graph);
			Node->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
			NewNode = Node;
		}
		else if (Kind == TEXT("variable_get") || Kind == TEXT("variable_set"))
		{
			const FName VarName(*GetString(OpParams, TEXT("name")));
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
		else if (Kind == TEXT("call_parent"))
		{
			const FName FuncName(*GetString(OpParams, TEXT("function")));
			UClass* SelfClass = Blueprint->SkeletonGeneratedClass
				? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->ParentClass.Get();
			UFunction* FnInSelf = SelfClass ? SelfClass->FindFunctionByName(FuncName) : nullptr;
			if (!FnInSelf)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("function '%s' not found on %s or its parents"),
					*FuncName.ToString(), SelfClass ? *SelfClass->GetName() : TEXT("null")));
			}
			// If this blueprint owns the override, step up to the super's
			// version; if the function only exists on a parent, that IS the
			// parent implementation already.
			UFunction* ParentFn = (FnInSelf->GetOwnerClass() == SelfClass)
				? UEdGraphSchema_K2::GetCallableParentFunction(FnInSelf)
				: FnInSelf;
			if (!ParentFn)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' has no parent implementation to call"), *FuncName.ToString()));
			}
			UK2Node_CallParentFunction* Node = NewObject<UK2Node_CallParentFunction>(Graph);
			Node->SetFromFunction(ParentFn);
			NewNode = Node;
		}
		// --- event dispatchers --------------------------------------------
		else if (Kind == TEXT("bind_dispatcher") || Kind == TEXT("unbind_dispatcher")
			|| Kind == TEXT("call_dispatcher") || Kind == TEXT("clear_dispatcher"))
		{
			const FString DispatcherName = GetString(OpParams, TEXT("name"));
			// A dispatcher on another object needs its class; on this
			// blueprint, self-context stores no class at all, which is what
			// lets the node survive a rename or a duplicate of the asset.
			const FString TargetSpec = GetString(OpParams, TEXT("target"));
			UClass* OwnerClass = nullptr;
			bool bSelfContext = true;
			if (TargetSpec.IsEmpty())
			{
				OwnerClass = Blueprint->SkeletonGeneratedClass
					? Blueprint->SkeletonGeneratedClass.Get()
					: (Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->ParentClass.Get());
			}
			else
			{
				bSelfContext = false;
				OwnerClass = StaticLoadClass(UObject::StaticClass(), nullptr, *TargetSpec);
				if (!OwnerClass)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("target class not found: '%s'. A Blueprint's class needs the _C suffix, e.g. /Game/BP_Door.BP_Door_C"),
						*TargetSpec));
				}
			}

			FMulticastDelegateProperty* DelegateProperty =
				FindFProperty<FMulticastDelegateProperty>(OwnerClass, *DispatcherName);
			if (!DelegateProperty)
			{
				TArray<FString> Names;
				for (TFieldIterator<FMulticastDelegateProperty> It(OwnerClass); It; ++It)
				{
					Names.Add(It->GetName());
				}
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("no dispatcher '%s' on %s. Dispatchers: %s"),
					*DispatcherName, *OwnerClass->GetName(),
					Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
			}

			UK2Node_BaseMCDelegate* Node = nullptr;
			if (Kind == TEXT("bind_dispatcher")) { Node = NewObject<UK2Node_AddDelegate>(Graph); }
			else if (Kind == TEXT("unbind_dispatcher")) { Node = NewObject<UK2Node_RemoveDelegate>(Graph); }
			else if (Kind == TEXT("call_dispatcher")) { Node = NewObject<UK2Node_CallDelegate>(Graph); }
			else { Node = NewObject<UK2Node_ClearDelegate>(Graph); }
			Node->SetFromProperty(DelegateProperty, bSelfContext, DelegateProperty->GetOwnerClass());
			NewNode = Node;
		}
		// --- flow control -------------------------------------------------
		else if (Kind == TEXT("branch"))
		{
			NewNode = NewObject<UK2Node_IfThenElse>(Graph);
		}
		else if (Kind == TEXT("sequence"))
		{
			NewNode = NewObject<UK2Node_ExecutionSequence>(Graph);
		}
		else if (Kind == TEXT("cast"))
		{
			UClass* TargetType = StaticLoadClass(UObject::StaticClass(), nullptr, *GetString(OpParams, TEXT("class")));
			if (!TargetType)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("cast needs 'class' to name the type to cast TO, e.g. /Script/Engine.PlayerController or /Game/BP_Door.BP_Door_C (got '%s')"),
					*GetString(OpParams, TEXT("class"))));
			}
			UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(Graph);
			Node->TargetType = TargetType;
			bool bPure = false;
			OpParams->TryGetBoolField(FStringView(TEXT("pure")), bPure);
			Node->SetPurity(bPure);
			NewNode = Node;
		}
		else if (Kind == TEXT("switch_enum"))
		{
			UEnum* Enum = LoadObject<UEnum>(nullptr, *GetString(OpParams, TEXT("enum")));
			if (!Enum)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("switch_enum needs 'enum', e.g. /Script/Engine.ECollisionChannel or /Game/E_State.E_State (got '%s')"),
					*GetString(OpParams, TEXT("enum"))));
			}
			UK2Node_SwitchEnum* Node = NewObject<UK2Node_SwitchEnum>(Graph);
			Node->SetEnum(Enum);
			NewNode = Node;
		}
		else if (Kind == TEXT("switch_int"))
		{
			NewNode = NewObject<UK2Node_SwitchInteger>(Graph);
		}
		else if (Kind == TEXT("switch_string"))
		{
			NewNode = NewObject<UK2Node_SwitchString>(Graph);
		}
		else if (Kind == TEXT("macro"))
		{
			// Loops and the rest of the standard library are macro instances,
			// not node classes - ForEachLoop, ForLoop, WhileLoop, DoOnce, Gate,
			// FlipFlop and friends all live in one engine Blueprint.
			const FString Library = GetString(OpParams, TEXT("library"),
				TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
			UBlueprint* MacroLibrary = LoadObject<UBlueprint>(nullptr, *Library);
			if (!MacroLibrary)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("macro library not found: %s"), *Library));
			}
			const FString MacroName = GetString(OpParams, TEXT("name"));
			UEdGraph* MacroGraph = nullptr;
			TArray<FString> Available;
			for (UEdGraph* Candidate : MacroLibrary->MacroGraphs)
			{
				if (!Candidate)
				{
					continue;
				}
				Available.Add(Candidate->GetName());
				if (Candidate->GetName().Equals(MacroName, ESearchCase::IgnoreCase))
				{
					MacroGraph = Candidate;
				}
			}
			if (!MacroGraph)
			{
				Available.Sort();
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("macro '%s' not found in %s. Available: %s"),
					*MacroName, *Library, *FString::Join(Available, TEXT(", "))));
			}
			UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
			Node->SetMacroGraph(MacroGraph);
			NewNode = Node;
		}
		// --- data ---------------------------------------------------------
		else if (Kind == TEXT("make_struct") || Kind == TEXT("break_struct"))
		{
			UScriptStruct* StructType = LoadObject<UScriptStruct>(nullptr, *GetString(OpParams, TEXT("struct")));
			if (!StructType)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("%s needs 'struct', e.g. /Script/CoreUObject.Vector or /Game/S_Row.S_Row (got '%s')"),
					*Kind, *GetString(OpParams, TEXT("struct"))));
			}
			// A struct that ships a native make or break function cannot use the
			// generic node, and the engine only says so at compile: FVector
			// errors with "the structure Make Vector is not a BlueprintType",
			// FRotator and friends warn. Both arrive long after the call that
			// caused them and neither names the thing to use instead, so this
			// refuses up front and points at the real function.
			const bool bMake = Kind == TEXT("make_struct");
			// The same metadata the engine's own CanBeMade/CanBeBroken test, and
			// its value is the function path - so the check and the suggestion
			// come from one place. Tested directly rather than through those
			// helpers because CanBeBroken is not exported from BlueprintGraph
			// (CanBeMade is), so it cannot be linked from here.
			const FName MetaKey = bMake
				? FBlueprintMetadata::MD_NativeMakeFunction
				: FBlueprintMetadata::MD_NativeBreakFunction;
			if (StructType->HasMetaData(MetaKey))
			{
				const FString Native = StructType->GetMetaData(MetaKey);
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("%s cannot be used on %s - it has a native %s function, and the generic node is rejected at compile.%s"),
					*Kind, *StructType->GetName(), bMake ? TEXT("make") : TEXT("break"),
					Native.IsEmpty()
						? TEXT(" Use add_node call_function with the struct's own Make/Break function instead.")
						: *FString::Printf(
							TEXT(" Use add_node kind 'call_function' with function '%s' instead."),
							*FPackageName::ObjectPathToObjectName(Native))));
			}
			if (bMake)
			{
				UK2Node_MakeStruct* Node = NewObject<UK2Node_MakeStruct>(Graph);
				Node->StructType = StructType;
				NewNode = Node;
			}
			else
			{
				UK2Node_BreakStruct* Node = NewObject<UK2Node_BreakStruct>(Graph);
				Node->StructType = StructType;
				NewNode = Node;
			}
		}
		else if (Kind == TEXT("make_array"))
		{
			NewNode = NewObject<UK2Node_MakeArray>(Graph);
		}
		else if (Kind == TEXT("select"))
		{
			NewNode = NewObject<UK2Node_Select>(Graph);
		}
		else if (Kind == TEXT("self"))
		{
			NewNode = NewObject<UK2Node_Self>(Graph);
		}
		else if (Kind == TEXT("function_result"))
		{
			// The return node of a function graph. Only meaningful there, and
			// a graph may only have one, so hand back the existing one rather
			// than making a second that silently wins or loses.
			for (UEdGraphNode* Existing : Graph->Nodes)
			{
				if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Existing))
				{
					ResultNode = Result;
					Data->SetObjectField(TEXT("node"), NodeToJson(Result));
					break;
				}
			}
			if (!ResultNode)
			{
				NewNode = NewObject<UK2Node_FunctionResult>(Graph);
			}
		}
		else if (Kind == TEXT("timeline"))
		{
			// A Timeline is not one object. It is a UTimelineTemplate owned by the
			// Blueprint, a curve per track, and a UK2Node_Timeline in the graph
			// that finds the template by variable name. Authoring one means
			// building all three in the right order.
			if (!FBlueprintEditorUtils::DoesSupportTimelines(Blueprint))
			{
				return FUplinkToolResult::Error(TEXT(
					"timelines only exist on actor-based Blueprints with an event graph - "
					"DoesSupportTimelines refuses anything else, and AddNewTimeline would return null"));
			}

			// AddNewTimeline does check(GeneratedClass) - a hard assert, not a
			// refusal - so a Blueprint that has never been compiled would take
			// the editor down rather than report a problem.
			if (!Blueprint->GeneratedClass)
			{
				FKismetEditorUtilities::CompileBlueprint(Blueprint);
				if (!Blueprint->GeneratedClass)
				{
					return FUplinkToolResult::Error(TEXT(
						"this Blueprint has no generated class even after a compile, and adding a timeline to one asserts"));
				}
			}

			FName TimelineVarName(*GetString(OpParams, TEXT("name")));
			if (TimelineVarName.IsNone())
			{
				TimelineVarName = FBlueprintEditorUtils::FindUniqueTimelineName(Blueprint);
			}
			UTimelineTemplate* Template = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineVarName);
			if (!Template)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("could not add a timeline called '%s' - a timeline or variable of that name already exists"),
					*TimelineVarName.ToString()));
			}

			// Length, looping and autoplay live on the TEMPLATE. The node carries
			// copies of all three, but they are Transient caches refreshed from
			// here on every AllocateDefaultPins - writing them on the node is a
			// silent no-op that is then silently overwritten.
			Template->Modify();
			Template->TimelineLength = static_cast<float>(GetNumber(OpParams, TEXT("length"), 2.0));
			Template->LengthMode = TL_TimelineLength;
			// Read into a bool first: these are one-bit fields and cannot be
			// bound to a bool& out-parameter.
			bool bFlag = false;
			if (OpParams->TryGetBoolField(FStringView(TEXT("loop")), bFlag)) { Template->bLoop = bFlag; }
			if (OpParams->TryGetBoolField(FStringView(TEXT("autoplay")), bFlag)) { Template->bAutoPlay = bFlag; }

			// One float track, which is what gives the node an output pin to read
			// the curve from.
			const FString TrackNameStr = GetString(OpParams, TEXT("track"), TEXT("Alpha"));
			const FName TrackName(*TrackNameStr);
			static const TSet<FName> ReservedPins = {
				TEXT("Play"), TEXT("PlayFromStart"), TEXT("Stop"), TEXT("Reverse"),
				TEXT("ReverseFromEnd"), TEXT("SetNewTime"), TEXT("NewTime"),
				TEXT("Update"), TEXT("Finished"), TEXT("Direction") };
			if (ReservedPins.Contains(TrackName))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' is one of the timeline node's own pin names, so a track cannot use it. ")
					TEXT("Reserved: Play, PlayFromStart, Stop, Reverse, ReverseFromEnd, SetNewTime, NewTime, Update, Finished, Direction."),
					*TrackNameStr));
			}

			FTTFloatTrack Track;
			// SetTrackName, not assignment: TrackName is private, and the setter
			// is what derives the PropertyName the compiler later resolves the
			// output pin against.
			Track.SetTrackName(TrackName, Template);
			Track.CurveFloat = NewObject<UCurveFloat>(Blueprint->GeneratedClass, NAME_None, RF_Public);

			const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
			if (OpParams->TryGetArrayField(FStringView(TEXT("keys")), Keys) && Keys->Num() > 0)
			{
				for (const TSharedPtr<FJsonValue>& Entry : *Keys)
				{
					const TSharedPtr<FJsonObject>* Key = nullptr;
					if (Entry.IsValid() && Entry->TryGetObject(Key) && Key->IsValid())
					{
						Track.CurveFloat->FloatCurve.AddKey(
							static_cast<float>(GetNumber(*Key, TEXT("time"), 0.0)),
							static_cast<float>(GetNumber(*Key, TEXT("value"), 0.0)));
					}
				}
			}
			else
			{
				// A curve with no keys reads zero forever, so the timeline would
				// run and drive nothing.
				Track.CurveFloat->FloatCurve.AddKey(0.0f, 0.0f);
				Track.CurveFloat->FloatCurve.AddKey(Template->TimelineLength, 1.0f);
			}

			const int32 TypedIndex = Template->FloatTracks.Num();
			Template->FloatTracks.Add(Track);

			// The step with no error attached to getting it wrong.
			// AllocateDefaultPins walks TrackDisplayOrder, not FloatTracks, so a
			// track that is never given a display entry compiles into a real
			// property with a real runtime binding and simply has no pin.
			Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, TypedIndex));

			UK2Node_Timeline* Node = NewObject<UK2Node_Timeline>(Graph);
			// Before the pins are allocated, which FinalizeNewNode does next: the
			// node reads the template through this name to know what pins to make.
			Node->TimelineName = TimelineVarName;
			NewNode = Node;
		}
		else
		{
			return FUplinkToolResult::Error(TEXT(
				"unknown 'kind'. Events and calls: call_function, custom_event {name, inputs?: [{name, type, by_ref?}] - event parameters, which appear as OUTPUT pins}, event, component_bound_event, enhanced_input {action} - an Enhanced Input event node for an Input Action asset · get_subsystem {class} - Get Subsystem for a concrete subsystem class, typed to it. "
				"Data: variable_get, variable_set, make_struct, break_struct, make_array, select, self. "
				"Flow: branch, sequence, cast, switch_enum, switch_int, switch_string, macro (ForEachLoop, ForLoop, WhileLoop, DoOnce, Gate, FlipFlop...), function_result. "
				"Dispatchers: bind_dispatcher, unbind_dispatcher, call_dispatcher, clear_dispatcher {name, target?}. Parent: call_parent {function}. "
				"Animation: timeline {name?, length?, loop?, autoplay?, track?, keys?}."));
		}

		if (NewNode)
		{
			FinalizeNewNode(Graph, NewNode, OpParams);
			if (UK2Node_CustomEvent* EventNode = Cast<UK2Node_CustomEvent>(NewNode))
			{
				const FString ParamError = AddCustomEventParams(EventNode, OpParams);
				if (!ParamError.IsEmpty())
				{
					// A failed op is reported, not rolled back, so take the
					// half-built event back out rather than leaving it in the
					// graph for the caller to find later.
					Graph->RemoveNode(EventNode);
					return FUplinkToolResult::Error(ParamError);
				}
			}
			Data->SetObjectField(TEXT("node"), NodeToJson(NewNode));
			ResultNode = NewNode;
		}
		const FString Ref = GetString(OpParams, TEXT("ref"));
		if (!Ref.IsEmpty() && ResultNode)
		{
			NodeRefs.Add(Ref, ResultNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}
		return FUplinkToolResult::Ok();
	}

	FUplinkToolResult ApplyNodePropertyOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data)
	{
		FString Error;

		// Set a property ON A GRAPH NODE (addressed by guid), not on an asset.
		// Supports dotted paths so struct members are reachable - anim graph
		// nodes keep their real settings inside a 'Node' struct, e.g.
		// "Node.OnMotionMatchingStateUpdated.FunctionName".
		UEdGraph* Graph = FindGraph(Blueprint, GetString(OpParams, TEXT("graph")), Error);
		if (!Graph)
		{
			return FUplinkToolResult::Error(Error);
		}
		const FString NodeRef = GetString(OpParams, TEXT("node"));
		// Compare parsed guids: bp_query emits DigitsWithHyphens while
		// FGuid::ToString() defaults to Digits, so a string compare misses.
		FGuid WantedGuid;
		const bool bParsedGuid = FGuid::Parse(NodeRef, WantedGuid);
		UEdGraphNode* Target = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			if (bParsedGuid ? (Node->NodeGuid == WantedGuid)
				: (Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) == NodeRef))
			{
				Target = Node;
				break;
			}
		}
		if (!Target)
		{
			return FUplinkToolResult::Error(FString::Printf(
				TEXT("node '%s' not found in graph '%s' (use bp_query to list node guids)"), *NodeRef, *Graph->GetName()));
		}

		const FString PropPath = GetString(OpParams, TEXT("property"));
		TArray<FString> Segments;
		PropPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return FUplinkToolResult::Error(TEXT("'property' is required"));
		}

		// Walk the path: every segment but the last must be a struct.
		void* Container = Target;
		UStruct* Owner = Target->GetClass();
		FProperty* Leaf = nullptr;
		for (int32 i = 0; i < Segments.Num(); ++i)
		{
			FProperty* Found = Owner->FindPropertyByName(FName(*Segments[i]));
			if (!Found)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("property '%s' not found on %s (path '%s')"),
					*Segments[i], *Owner->GetName(), *PropPath));
			}
			if (i == Segments.Num() - 1)
			{
				Leaf = Found;
				Container = Found->ContainerPtrToValuePtr<void>(Container);
				break;
			}
			FStructProperty* AsStruct = CastField<FStructProperty>(Found);
			if (!AsStruct)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' is not a struct, so '%s' cannot be reached"), *Segments[i], *PropPath));
			}
			Container = AsStruct->ContainerPtrToValuePtr<void>(Container);
			Owner = AsStruct->Struct;
		}

		const TSharedPtr<FJsonValue> Value = OpParams->TryGetField(FStringView(TEXT("value")));
		if (!Value.IsValid())
		{
			return FUplinkToolResult::Error(TEXT("'value' is required"));
		}
		Target->Modify();
		if (!FJsonObjectConverter::JsonValueToUProperty(Value, Leaf, Container))
		{
			return FUplinkToolResult::Error(FString::Printf(
				TEXT("could not convert value for %s (%s)"), *PropPath, *Leaf->GetCPPType()));
		}

		// Let the node rebuild its pins/state from the new value.
		bool bReconstruct = true;
		OpParams->TryGetBoolField(FStringView(TEXT("reconstruct")), bReconstruct);
		if (bReconstruct)
		{
			Target->ReconstructNode();
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Data->SetStringField(TEXT("node"), Target->GetClass()->GetName());
		Data->SetStringField(TEXT("property"), PropPath);
		Data->SetField(TEXT("value"), FJsonObjectConverter::UPropertyToJsonValue(Leaf, Container));
		return FUplinkToolResult::Ok();
	}
}
