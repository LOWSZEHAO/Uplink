// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Widget tools: widget_tree (query a Widget Blueprint's hierarchy) and
// widget_add (construct a widget into the tree). Event hookup for widgets is
// bp_modify's component_bound_event node kind.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
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
		TEXT("List a Widget Blueprint's widget hierarchy: name, class, parent, and whether each widget is a variable (only variables can have events bound via bp_modify component_bound_event). The reply carries 'total' and 'truncated' so a capped list is never mistaken for the whole tree."),
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
}
