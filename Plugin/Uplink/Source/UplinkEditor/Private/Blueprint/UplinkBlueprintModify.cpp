// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// bp_modify: the op dispatcher, the batch loop that feeds it, and bp_compile.

#include "Blueprint/UplinkBlueprintCommon.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Package.h"

using namespace UplinkToolUtil;

namespace UplinkBlueprint
{
	/**
	 * One graph edit. Op fields match bp_modify's single-op form; NodeRefs maps
	 * batch 'ref' names to node guids so later ops can say '@ref'.
	 */
	static FUplinkToolResult ApplyGraphOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TMap<FString, FString>& NodeRefs, TSharedRef<FJsonObject> Data)
	{
		const FString Op = GetString(OpParams, TEXT("op"));

		if (Op == TEXT("add_variable") || Op == TEXT("set_variable") || Op == TEXT("remove_variable"))
		{
			return ApplyVariableOp(Blueprint, OpParams, Data);
		}
		if (Op == TEXT("add_dispatcher"))
		{
			return ApplyAddDispatcherOp(Blueprint, OpParams, Data);
		}
		if (Op == TEXT("implement_interface") || Op == TEXT("remove_interface"))
		{
			return ApplyInterfaceOp(Blueprint, OpParams, Data);
		}
		if (Op == TEXT("override_function"))
		{
			return ApplyOverrideFunctionOp(Blueprint, OpParams, NodeRefs, Data);
		}
		if (Op == TEXT("add_function") || Op == TEXT("remove_function"))
		{
			return ApplyFunctionOp(Blueprint, OpParams, Data);
		}
		if (Op == TEXT("add_node"))
		{
			return ApplyAddNodeOp(Blueprint, OpParams, NodeRefs, Data);
		}
		if (Op == TEXT("set_node_property"))
		{
			return ApplyNodePropertyOp(Blueprint, OpParams, Data);
		}
		if (Op == TEXT("arrange"))
		{
			return ApplyArrangeOp(Blueprint, OpParams, Data);
		}
		if (Op == TEXT("connect"))
		{
			return ApplyConnectOp(Blueprint, OpParams, NodeRefs);
		}
		if (Op == TEXT("break_links") || Op == TEXT("delete_node") || Op == TEXT("set_pin_default"))
		{
			return ApplyPinOp(Blueprint, OpParams, NodeRefs);
		}
		return FUplinkToolResult::Error(TEXT("unknown op (add_variable, set_variable, remove_variable, add_dispatcher, implement_interface, remove_interface, override_function, add_function, remove_function, add_node, arrange, connect, break_links, delete_node, set_pin_default, set_node_property)"));
	}

	void RegisterModify(FUplinkToolRegistry& Registry)
	{
		Registry.RegisterQuick(
			TEXT("bp_modify"),
			TEXT("Edit a Blueprint - one op, or a whole batch in a single call via 'ops': [{op:..., ...}, ...]. In a batch, give add_node ops a 'ref' name and later ops can address that node as '@ref' (from_node/to_node/node) - an entire event graph builds in ONE request. Ops: add_variable {name,type,default?,instance_editable?,expose_on_spawn?,read_only?,replicated?,category?,tooltip?} | set_variable {name, same options} - configure an existing variable | remove_variable {name} | add_dispatcher {name, inputs?} - declare an event dispatcher | implement_interface {interface} / remove_interface {interface, preserve_functions?} | override_function {name, as_function?} - override a parent or interface function | add_function {name, inputs?, outputs?, pure?, thread_safe?, category?} | add_node {kind: call_function {class,function} | custom_event {name} | event {name - e.g. ReceiveBeginPlay/ReceiveTick; reuses a matching ghost/existing event node} | component_bound_event {component, event - e.g. component 'MyButton' event 'OnClicked', or 'NiagaraComp' + 'OnSystemFinished'} | variable_get/variable_set {name} | branch | sequence | cast {class, pure?} | switch_enum {enum} | switch_int | switch_string | macro {name: ForEachLoop/ForLoop/WhileLoop/DoOnce/Gate/FlipFlop..., library?} | make_struct/break_struct {struct} | make_array | select | self | function_result | bind_dispatcher/unbind_dispatcher/call_dispatcher/clear_dispatcher {name, target?} - pins are 'Delegate' (shown as Event) and 'self' (shown as Target), graph?, x?, y?, ref?} | arrange {graph?} - auto-layout: dependency columns, exec chains as straight horizontal lanes, data nodes below, reroute knots at turns; finish a batch with it | connect {graph?, from_node, from_pin, to_node, to_pin} | break_links {graph?, node, pin} | delete_node {graph?, node} | set_pin_default {graph?, node, pin, value}. New nodes never overlap existing ones (positions are nudged to free space; omit x/y for auto-placement). Node handles are guids from bp_query, the add_node response, or '@ref'. A failed batch op stops the batch (earlier ops stay applied). Set compile=true to compile at the end."),
			TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"},"op":{"type":"string","enum":["add_variable","set_variable","remove_variable","add_dispatcher","implement_interface","remove_interface","override_function","add_function","remove_function","add_node","arrange","connect","break_links","delete_node","set_pin_default","set_node_property"]},"ops":{"type":"array","items":{"type":"object"},"description":"Batch mode: sequence of op objects (same fields as single-op form, plus 'ref' on add_node)"},"name":{"type":"string"},"type":{"type":"string"},"default":{"type":"string"},"instance_editable":{"type":"boolean","description":"add/set_variable: editable on placed instances"},"expose_on_spawn":{"type":"boolean","description":"add/set_variable: show as a SpawnActor pin"},"read_only":{"type":"boolean","description":"add/set_variable: blueprint-read-only"},"replicated":{"type":"boolean","description":"add/set_variable: replicate this property"},"tooltip":{"type":"string","description":"add/set_variable"},"kind":{"type":"string","enum":["call_function","custom_event","event","component_bound_event","variable_get","variable_set","branch","sequence","cast","switch_enum","switch_int","switch_string","macro","make_struct","break_struct","make_array","select","self","function_result","call_parent","bind_dispatcher","unbind_dispatcher","call_dispatcher","clear_dispatcher","timeline","enhanced_input","get_subsystem"]},"class":{"type":"string","description":"add_node call_function: class path or 'self'; add_node cast: the class to cast TO; add_node get_subsystem: the subsystem class to get"},"struct":{"type":"string","description":"make_struct/break_struct: script struct path, e.g. /Script/CoreUObject.Vector"},"enum":{"type":"string","description":"switch_enum: enum path, e.g. /Game/E_State.E_State"},"action":{"type":"string","description":"enhanced_input: Input Action asset path, e.g. /Game/Input/Actions/IA_Jump"},"library":{"type":"string","description":"macro: Blueprint holding the macro (default the engine StandardMacros)"},"target":{"type":"string","description":"dispatcher kinds: class owning the dispatcher, e.g. /Game/BP_Door.BP_Door_C. Omit for one on this blueprint"},"interface":{"type":"string","description":"implement_interface/remove_interface: /Script/Module.IThing or /Game/BPI_Thing"},"preserve_functions":{"type":"boolean","description":"remove_interface: keep the implementations as normal functions"},"as_function":{"type":"boolean","description":"override_function: force the function-graph form instead of an event node"},"function":{"type":"string"},"component":{"type":"string","description":"component_bound_event: component/widget variable name"},"event":{"type":"string","description":"component_bound_event: delegate name on the component's class"},"ref":{"type":"string","description":"add_node: name this node for '@ref' handles in later batch ops"},"graph":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"from_node":{"type":"string"},"from_pin":{"type":"string"},"to_node":{"type":"string"},"to_pin":{"type":"string"},"node":{"type":"string"},"pin":{"type":"string"},"value":{"type":"string"},"compile":{"type":"boolean"},"thread_safe":{"type":"boolean","description":"add_function: required for anim-graph node functions, which cannot run on the game thread"},"pure":{"type":"boolean","description":"add_function: no exec pins"},"category":{"type":"string","description":"add_function / add_variable / set_variable: category"},"inputs":{"type":"array","items":{"type":"object"},"description":"add_function and add_node custom_event: [{name, type, by_ref?, const?}] parameters. On a custom event they become OUTPUT pins - by_ref+const are needed to match prototype-validated signatures such as anim node bindings"},"outputs":{"type":"array","items":{"type":"object"},"description":"add_function: [{name, type}] return values - creates the Result node, wired to the entry"},"property":{"type":"string","description":"set_node_property: property on the node, dotted paths allowed"},"reconstruct":{"type":"boolean","description":"set_node_property: rebuild the node's pins afterwards (default true)"},"length":{"type":"number","description":"timeline: seconds"},"loop":{"type":"boolean","description":"timeline: loop"},"autoplay":{"type":"boolean","description":"timeline: play on BeginPlay without a Play wire"},"track":{"type":"string","description":"timeline: float track name, which becomes the output pin (default Alpha)"},"keys":{"type":"array","description":"timeline: curve keyframes [{time,value}]; default ramps 0-1 over the length","items":{"type":"object","properties":{"time":{"type":"number"},"value":{"type":"number"}}}},"save":{"type":"boolean","default":false,"description":"Write the blueprint to disk afterwards. Edits are in memory until saved, so an editor restart discards them. Skipped if compile:true reported errors."}},"required":["blueprint"]})json"),
			/*bReadOnly=*/false,
			[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
			{
				FString Error;
				UBlueprint* Blueprint = LoadBlueprint(Ctx, Error);
				if (!Blueprint)
				{
					return FUplinkToolResult::Error(Error);
				}
				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				TMap<FString, FString> NodeRefs;

				const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
				if (Ctx.Params->TryGetArrayField(FStringView(TEXT("ops")), Ops))
				{
					TArray<TSharedPtr<FJsonValue>> Results;
					for (int32 Index = 0; Index < Ops->Num(); ++Index)
					{
						const TSharedPtr<FJsonObject>* OpObject = nullptr;
						if (!(*Ops)[Index].IsValid() || !(*Ops)[Index]->TryGetObject(OpObject) || !OpObject->IsValid())
						{
							return FUplinkToolResult::Error(FString::Printf(TEXT("ops[%d] is not an object"), Index));
						}
						TSharedRef<FJsonObject> OpData = MakeShared<FJsonObject>();
						const FUplinkToolResult OpResult = ApplyGraphOp(Blueprint, *OpObject, NodeRefs, OpData);
						if (OpResult.bError)
						{
							// Earlier ops have already been applied, so hand back
							// what succeeded - otherwise the caller cannot tell how
							// far the batch got and has to re-derive it.
							Data->SetArrayField(TEXT("results"), Results);
							Data->SetNumberField(TEXT("failedAt"), Index);
							Data->SetNumberField(TEXT("applied"), Results.Num());
							FUplinkToolResult Out = FUplinkToolResult::Ok(Data, FString::Printf(
								TEXT("ops[%d] (%s) failed: %s - the %d op(s) before it were applied, blueprint not compiled"),
								Index, *GetString(*OpObject, TEXT("op")), *OpResult.Message, Results.Num()));
							Out.bError = true;
							return Out;
						}
						Results.Add(MakeShared<FJsonValueObject>(OpData));
					}
					Data->SetArrayField(TEXT("results"), Results);
				}
				else
				{
					const FUplinkToolResult OpResult = ApplyGraphOp(Blueprint, Ctx.Params, NodeRefs, Data);
					if (OpResult.bError)
					{
						return OpResult;
					}
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				// Graph edits live only in memory until the package is written, so
				// an editor restart silently discards them. 'save' makes a run of
				// edits durable without a second round trip.
				bool bSave = false;
				Ctx.Params->TryGetBoolField(FStringView(TEXT("save")), bSave);
				auto SaveIfAsked = [Blueprint, bSave, &Data]()
				{
					if (!bSave)
					{
						return;
					}
					UPackage* Package = Blueprint->GetOutermost();
					TArray<UPackage*> ToSave{ Package };
					Data->SetBoolField(TEXT("saved"), SavePackagesUnattended(ToSave, /*bCheckDirty=*/false));
				};

				bool bCompile = false;
				Ctx.Params->TryGetBoolField(FStringView(TEXT("compile")), bCompile);
				if (bCompile)
				{
					FUplinkToolResult CompileResult = CompileAndReport(Blueprint);
					if (CompileResult.Data.IsValid())
					{
						Data->SetObjectField(TEXT("compile"), CompileResult.Data);
					}
					// Only persist a blueprint that compiled - saving a broken one
					// makes the breakage survive the restart too.
					if (!CompileResult.bError)
					{
						SaveIfAsked();
					}
					FUplinkToolResult Out = FUplinkToolResult::Ok(Data, CompileResult.Message);
					Out.bError = CompileResult.bError;
					return Out;
				}
				SaveIfAsked();
				return FUplinkToolResult::Ok(Data, TEXT("modified (not compiled)"));
			});
	}

	void RegisterCompile(FUplinkToolRegistry& Registry)
	{
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
}
