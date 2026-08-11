// Copyright (c) 2026 Low Sze Hao. MIT License.
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
		TEXT("List a Widget Blueprint's widget hierarchy: name, class, parent, and whether each widget is a variable (only variables can have events bound via bp_modify component_bound_event)."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string","description":"Widget Blueprint asset path"}},"required":["blueprint"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprint(Ctx, Error);
			if (!WidgetBlueprint)
			{
				return FUplinkToolResult::Error(Error);
			}

			TArray<TSharedPtr<FJsonValue>> Rows;
			if (WidgetBlueprint->WidgetTree)
			{
				const UWidget* Root = WidgetBlueprint->WidgetTree->RootWidget;
				WidgetBlueprint->WidgetTree->ForEachWidget([&Rows, Root](UWidget* Widget)
				{
					if (!Widget)
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

			const FString ParentName = GetString(Ctx.Params, TEXT("parent"));
			if (!ParentName.IsEmpty())
			{
				UPanelWidget* Parent = Cast<UPanelWidget>(WidgetBlueprint->WidgetTree->FindWidget(FName(*ParentName)));
				if (!Parent)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("parent '%s' not found or is not a panel widget"), *ParentName));
				}
				Parent->AddChild(NewWidget);
			}
			else if (!WidgetBlueprint->WidgetTree->RootWidget)
			{
				WidgetBlueprint->WidgetTree->RootWidget = NewWidget;
			}
			else if (UPanelWidget* RootPanel = Cast<UPanelWidget>(WidgetBlueprint->WidgetTree->RootWidget))
			{
				RootPanel->AddChild(NewWidget);
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
