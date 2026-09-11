// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Widget tools: widget_tree (query a Widget Blueprint's hierarchy), widget_add
// (construct a widget into it) and widget_modify (remove one, or move it to
// another panel). Event hookup for widgets is bp_modify's component_bound_event
// node kind, and layout is set_property against the paths widget_tree reports -
// a widget and the slot that positions it are both ordinary named objects.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprintEditorUtils.h"
#include "WidgetBlueprint.h"

using namespace UplinkToolUtil;

namespace
{
	UWidgetBlueprint* LoadWidgetBlueprint(const FUplinkToolContext& Ctx, FString& OutError)
	{
		const FString Path = GetString(Ctx.Params, TEXT("blueprint"));
		UWidgetBlueprint* WidgetBlueprint = LoadObject<UWidgetBlueprint>(nullptr, *Path);
		if (!WidgetBlueprint)
		{
			OutError = FString::Printf(TEXT("widget blueprint not found: %s"), *Path);
		}
		return WidgetBlueprint;
	}
}

void UplinkTools::RegisterWidget(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("widget_tree"),
		TEXT("List a Widget Blueprint's widget hierarchy: name, class, parent, and whether each widget is a variable (only variables can have events bound via bp_modify component_bound_event). Each row also carries 'path' and, for anything inside a panel, 'slot_path' and 'slot_class' - those are real object paths, so set_property writes a widget's own properties through the first and its layout through the second: a canvas child's position is 'LayoutData.Offsets.Left' on the slot, a box child's is 'Padding' or 'Size.Value'. That is how layout is authored here; there is no separate layout tool. The reply carries 'total' and 'truncated' so a capped list is never mistaken for the whole tree."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string","description":"Widget Blueprint asset path"},"max":{"type":"number","default":200}},"required":["blueprint"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprint(Ctx, Error);
			if (!WidgetBlueprint)
			{
				return FUplinkToolResult::Error(Error);
			}

			// A real HUD or menu runs to hundreds of widgets, so cap the list
			// and report the true count rather than returning all of it.
			const int32 Max = FMath::Clamp(
				static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 200.0)), 1, 2000);
			int32 Total = 0;

			TArray<TSharedPtr<FJsonValue>> Rows;
			if (WidgetBlueprint->WidgetTree)
			{
				const UWidget* Root = WidgetBlueprint->WidgetTree->RootWidget;
				WidgetBlueprint->WidgetTree->ForEachWidget([&Rows, &Total, Max, Root](UWidget* Widget)
				{
					if (!Widget)
					{
						return;
					}
					++Total;
					if (Rows.Num() >= Max)
					{
						return;
					}
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Widget->GetName());
					Row->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
					Row->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);

					// A widget in a tree is an ordinary named object, so
					// set_property already reaches it - but only if you can write
					// the path, and the ':WidgetTree.' in the middle of it is not
					// something anyone guesses. Handing it back turns authoring
					// layout into one more call instead of a research problem.
					Row->SetStringField(TEXT("path"), Widget->GetPathName());
					if (const UPanelSlot* Slot = Widget->Slot)
					{
						// The layout lives on the slot, not the widget, and which
						// properties exist depends on the panel: a canvas slot has
						// LayoutData and ZOrder, a box slot has Padding and Size.
						// The class is reported so the caller knows which.
						Row->SetStringField(TEXT("slot_path"), Slot->GetPathName());
						Row->SetStringField(TEXT("slot_class"), Slot->GetClass()->GetPathName());
					}

					if (Widget == Root)
					{
						Row->SetBoolField(TEXT("is_root"), true);
					}
					else if (const UPanelWidget* Parent = Widget->GetParent())
					{
						Row->SetStringField(TEXT("parent"), Parent->GetName());
					}
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				});
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("blueprint"), WidgetBlueprint->GetPathName());
			Data->SetArrayField(TEXT("widgets"), Rows);
			Data->SetNumberField(TEXT("total"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Rows.Num());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("widget_add"),
		TEXT("Construct a widget into a Widget Blueprint's tree (as a variable, so its events are immediately bindable). Becomes the root if the tree is empty; otherwise pass a panel widget as 'parent' (or omit to attach to the root panel)."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"},"class":{"type":"string","description":"Widget class, e.g. /Script/UMG.Button or /Script/UMG.TextBlock"},"name":{"type":"string"},"parent":{"type":"string","description":"Name of an existing panel widget to add into"}},"required":["blueprint","class","name"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprint(Ctx, Error);
			if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
			{
				return FUplinkToolResult::Error(Error.IsEmpty() ? TEXT("widget blueprint has no widget tree") : Error);
			}

			UClass* WidgetClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *GetString(Ctx.Params, TEXT("class")));
			if (!WidgetClass)
			{
				return FUplinkToolResult::Error(TEXT("widget class not found (e.g. /Script/UMG.Button)"));
			}

			const FName WidgetName(*GetString(Ctx.Params, TEXT("name")));
			if (WidgetBlueprint->WidgetTree->FindWidget(WidgetName))
			{
				return FUplinkToolResult::Error(TEXT("a widget with that name already exists"));
			}

			UWidget* NewWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, WidgetName);
			if (!NewWidget)
			{
				return FUplinkToolResult::Error(TEXT("ConstructWidget failed"));
			}
			NewWidget->bIsVariable = true;

			// Every source widget must have an entry here or the compiler fires
			// ensureAlwaysMsgf("Widget [%s] was added but did not get a GUID")
			// and patches one in for us - once per widget, every compile, with a
			// crash report each time. The designer registers this when it drops
			// a widget; constructing one directly has to do it too.
			WidgetBlueprint->WidgetVariableNameToGuidMap.Add(WidgetName, FGuid::NewGuid());

			const FString ParentName = GetString(Ctx.Params, TEXT("parent"));
			if (!ParentName.IsEmpty())
			{
				UPanelWidget* Parent = Cast<UPanelWidget>(WidgetBlueprint->WidgetTree->FindWidget(FName(*ParentName)));
				if (!Parent)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("parent '%s' not found or is not a panel widget"), *ParentName));
				}
				// AddChild returns null when the panel is full - a Button,
				// Border or SizeBox holds exactly one child. Reporting success
				// there leaves an orphan in the tree that only surfaces later as
				// a compiler complaint nothing appears to explain.
				if (!Parent->AddChild(NewWidget))
				{
					WidgetBlueprint->WidgetTree->RemoveWidget(NewWidget);
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' (%s) cannot take another child - it holds %d and its slot type allows no more. ")
						TEXT("Single-child panels (Button, Border, SizeBox, ScaleBox...) need a layout panel inside them first."),
						*ParentName, *Parent->GetClass()->GetName(), Parent->GetChildrenCount()));
				}
			}
			else if (!WidgetBlueprint->WidgetTree->RootWidget)
			{
				WidgetBlueprint->WidgetTree->RootWidget = NewWidget;
			}
			else if (UPanelWidget* RootPanel = Cast<UPanelWidget>(WidgetBlueprint->WidgetTree->RootWidget))
			{
				// Same refusal as the explicit-parent branch above: a root that
				// is a Button or Border is a panel and already holds its child,
				// so AddChild returns null and the widget stays in the tree
				// parented to nothing.
				if (!RootPanel->AddChild(NewWidget))
				{
					WidgetBlueprint->WidgetTree->RemoveWidget(NewWidget);
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("the root widget '%s' (%s) cannot take another child - it holds %d and its slot type allows no more. ")
						TEXT("Single-child panels (Button, Border, SizeBox, ScaleBox...) need a layout panel inside them first."),
						*RootPanel->GetName(), *RootPanel->GetClass()->GetName(), RootPanel->GetChildrenCount()));
				}
			}
			else
			{
				return FUplinkToolResult::Error(TEXT("the root widget is not a panel; pass 'parent' explicitly"));
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("widget"), NewWidget->GetName());
			Data->SetStringField(TEXT("class"), WidgetClass->GetPathName());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("widget_modify"),
		TEXT("Change a Widget Blueprint's tree: 'remove' takes a widget (and everything under it) out, 'reparent' moves one into a different panel. Removing is refused while a graph node still reads or writes the widget, and those nodes are named: the engine's delete takes them with it, so forcing does not leave a broken graph - it leaves one that has quietly lost logic, and it still compiles clean afterwards. force:true does it anyway. Layout is not here: a widget and its slot are ordinary objects, so set_property against the 'path' and 'slot_path' that widget_tree reports is how position, size and padding are written."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string","description":"Widget Blueprint asset path"},"op":{"type":"string","enum":["remove","reparent"]},"widget":{"type":"string","description":"Name of the widget to act on"},"parent":{"type":"string","description":"reparent: name of the panel widget to move it into"},"force":{"type":"boolean","default":false,"description":"remove: delete even though graph nodes still reference the widget"}},"required":["blueprint","op","widget"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprint(Ctx, Error);
			if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
			{
				return FUplinkToolResult::Error(Error.IsEmpty() ? TEXT("widget blueprint has no widget tree") : Error);
			}

			const FString WidgetName = GetString(Ctx.Params, TEXT("widget"));
			UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName));
			if (!Widget)
			{
				TArray<FString> Names;
				WidgetBlueprint->WidgetTree->ForEachWidget([&Names](UWidget* Each)
				{
					if (Each)
					{
						Names.Add(Each->GetName());
					}
				});
				const FString Nearest = NearestName(WidgetName, Names);
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("no widget named '%s' in this tree.%s"),
					*WidgetName,
					Nearest.IsEmpty()
						? TEXT(" widget_tree lists the ones that are there.")
						: *FString::Printf(TEXT(" Did you mean '%s'?"), *Nearest)));
			}

			const FString Op = GetString(Ctx.Params, TEXT("op"));
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("blueprint"), WidgetBlueprint->GetPathName());
			Data->SetStringField(TEXT("widget"), Widget->GetName());
			Data->SetStringField(TEXT("op"), Op);

			if (Op == TEXT("remove"))
			{
				// The engine's own delete asks a person about this with a modal,
				// and over HTTP there is nobody to answer it. Answering it
				// silently is worse than refusing, because of what the engine
				// then does: DeleteWidgets removes the referencing NODES along
				// with the widget. Nothing dangles and the Blueprint compiles
				// clean, so there is no warning anywhere and no way to notice
				// afterwards that a piece of the graph left with it.
				const FName VariableName = Widget->GetFName();
				TArray<FString> Users;
				TArray<UK2Node_Variable*> VariableNodes;
				FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Variable>(WidgetBlueprint, VariableNodes);
				for (const UK2Node_Variable* Node : VariableNodes)
				{
					if (Node && Node->VariableReference.GetMemberName() == VariableName)
					{
						Users.AddUnique(FString::Printf(TEXT("%s in %s"),
							*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
							Node->GetGraph() ? *Node->GetGraph()->GetName() : TEXT("a graph")));
					}
				}

				bool bForce = false;
				Ctx.Params->TryGetBoolField(FStringView(TEXT("force")), bForce);
				if (Users.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> Json;
					for (const FString& User : Users)
					{
						Json.Add(MakeShared<FJsonValueString>(User));
					}
					Data->SetArrayField(TEXT("referencing_nodes"), Json);
					if (!bForce)
					{
						FUplinkToolResult Refusal = FUplinkToolResult::Error(FString::Printf(
							TEXT("%d graph node(s) still reference '%s' - they are named in 'referencing_nodes'. The engine deletes those nodes along with the widget, and the Blueprint compiles clean afterwards, so forcing this loses that logic silently rather than leaving anything to find. Repoint them first, or pass force:true."),
							Users.Num(), *WidgetName));
						Refusal.Data = Data;
						return Refusal;
					}
				}

				const bool bWasRoot = WidgetBlueprint->WidgetTree->RootWidget == Widget;
				FWidgetBlueprintEditorUtils::DeleteWidgets(
					WidgetBlueprint, { Widget },
					FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);
				WidgetBlueprint->WidgetVariableNameToGuidMap.Remove(VariableName);

				// Read back rather than report the request: DeleteWidgets is void.
				const bool bGone = WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName)) == nullptr;
				Data->SetBoolField(TEXT("removed"), bGone);
				if (!bGone)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' is still in the tree after the delete"), *WidgetName));
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				return FUplinkToolResult::Ok(Data, FString::Printf(
					TEXT("removed %s%s%s"), *WidgetName,
					bWasRoot ? TEXT(" - it was the root, so the tree is now empty") : TEXT(""),
					Users.Num() > 0
						? *FString::Printf(TEXT(" - and the %d graph node(s) in 'referencing_nodes' went with it"), Users.Num())
						: TEXT("")));
			}

			if (Op == TEXT("reparent"))
			{
				const FString ParentName = GetString(Ctx.Params, TEXT("parent"));
				UPanelWidget* NewParent = Cast<UPanelWidget>(
					WidgetBlueprint->WidgetTree->FindWidget(FName(*ParentName)));
				if (!NewParent)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("parent '%s' not found or is not a panel widget"), *ParentName));
				}
				if (NewParent == Widget)
				{
					return FUplinkToolResult::Error(TEXT("a widget cannot be its own parent"));
				}

				// A panel moved inside its own descendant leaves a ring the tree
				// walk never escapes, and nothing in the engine checks for it -
				// the designer hangs on the next open.
				for (const UWidget* Walk = NewParent; Walk; Walk = Walk->GetParent())
				{
					if (Walk == Widget)
					{
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("'%s' is inside '%s', so moving '%s' into it would make the tree a ring"),
							*ParentName, *WidgetName, *WidgetName));
					}
				}

				UPanelWidget* OldParent = Widget->GetParent();
				if (OldParent == NewParent)
				{
					Data->SetStringField(TEXT("parent"), NewParent->GetName());
					return FUplinkToolResult::Ok(Data, FString::Printf(
						TEXT("%s is already in %s"), *WidgetName, *ParentName));
				}

				// Detach first so the old slot is released, then attach - and put
				// it back if the new panel will not take it, because a widget
				// parented to nothing is in the tree and in no layout, which the
				// designer shows as simply missing.
				Widget->RemoveFromParent();
				if (!NewParent->AddChild(Widget))
				{
					if (OldParent)
					{
						OldParent->AddChild(Widget);
					}
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' (%s) cannot take another child - it holds %d and its slot type allows no more. ")
						TEXT("Single-child panels (Button, Border, SizeBox, ScaleBox...) need a layout panel inside them first."),
						*ParentName, *NewParent->GetClass()->GetName(), NewParent->GetChildrenCount()));
				}

				if (WidgetBlueprint->WidgetTree->RootWidget == Widget)
				{
					// It was the root and is now somebody's child, so the tree
					// needs a root that is not also a descendant of itself.
					WidgetBlueprint->WidgetTree->RootWidget = nullptr;
					for (UWidget* Walk = NewParent; Walk; Walk = Walk->GetParent())
					{
						if (!Walk->GetParent())
						{
							WidgetBlueprint->WidgetTree->RootWidget = Walk;
						}
					}
				}

				Data->SetStringField(TEXT("parent"), NewParent->GetName());
				if (Widget->Slot)
				{
					// The slot is a different class under a different panel, so
					// the path the caller was holding is stale - handing back the
					// new one saves a second widget_tree.
					Data->SetStringField(TEXT("slot_path"), Widget->Slot->GetPathName());
					Data->SetStringField(TEXT("slot_class"), Widget->Slot->GetClass()->GetPathName());
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				return FUplinkToolResult::Ok(Data, FString::Printf(
					TEXT("moved %s into %s - its layout is on a new %s, so anything set on the old slot is gone"),
					*WidgetName, *ParentName,
					Widget->Slot ? *Widget->Slot->GetClass()->GetName() : TEXT("slot")));
			}

			return FUplinkToolResult::Error(FString::Printf(
				TEXT("unknown op '%s' - one of remove, reparent"), *Op));
		});

}
