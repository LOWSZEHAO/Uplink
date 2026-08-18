// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// The surface the bp_* tool files hand each other. One file per subject in
// this directory; UplinkTools_Blueprint.cpp only calls the Register* entries.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UplinkTool.h"

class FUplinkToolRegistry;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;

struct FEdGraphPinType;

/**
 * A named namespace rather than a file-local one, because these cross
 * translation units and the module builds with unity - a file-local helper
 * here would share a scope with every other tool file's helpers.
 */
namespace UplinkBlueprint
{
	// Lookup and shared plumbing - UplinkBlueprintCommon.cpp.
	UBlueprint* LoadBlueprint(const FUplinkToolContext& Ctx, FString& OutError);
	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName, FString& OutError);
	bool MakePinType(const FString& TypeString, FEdGraphPinType& Out, FString& OutError);
	FUplinkToolResult CompileAndReport(UBlueprint* Blueprint);

	// Placement and auto-layout - UplinkBlueprintLayout.cpp.
	void FinalizeNewNode(UEdGraph* Graph, UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Op);
	int32 ArrangeGraph(UEdGraph* Graph);

	// The node/pin shape bp_query reports and the add_node ops echo back -
	// UplinkBlueprintQuery.cpp.
	TSharedRef<FJsonObject> NodeToJson(const UEdGraphNode* Node);

	/**
	 * One bp_modify op family each. OpParams carries the fields of bp_modify's
	 * single-op form, and NodeRefs maps a batch's 'ref' names to node guids so
	 * later ops can say '@ref'. Ok() means applied; a batch stops on the first
	 * error, so a handler must not report success it did not achieve.
	 */
	FUplinkToolResult ApplyVariableOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data);
	FUplinkToolResult ApplyAddDispatcherOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data);
	FUplinkToolResult ApplyInterfaceOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data);
	FUplinkToolResult ApplyOverrideFunctionOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TMap<FString, FString>& NodeRefs, TSharedRef<FJsonObject> Data);
	FUplinkToolResult ApplyFunctionOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data);
	FUplinkToolResult ApplyAddNodeOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TMap<FString, FString>& NodeRefs, TSharedRef<FJsonObject> Data);
	FUplinkToolResult ApplyNodePropertyOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data);
	FUplinkToolResult ApplyArrangeOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		TSharedRef<FJsonObject> Data);
	FUplinkToolResult ApplyConnectOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		const TMap<FString, FString>& NodeRefs);
	FUplinkToolResult ApplyPinOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& OpParams,
		const TMap<FString, FString>& NodeRefs);

	// Called in this order, which is the order the tools have always been
	// advertised in.
	void RegisterCreate(FUplinkToolRegistry& Registry);
	void RegisterQuery(FUplinkToolRegistry& Registry);
	void RegisterModify(FUplinkToolRegistry& Registry);
	void RegisterAddComponent(FUplinkToolRegistry& Registry);
	void RegisterCompile(FUplinkToolRegistry& Registry);
}
