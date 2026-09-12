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
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/NamedSlot.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "K2Node_Variable.h"
#include "Kismet2/Kismet2NameValidators.h"
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

	/**
	 * Is this a name the Blueprint compiler will accept for a widget?
	 *
	 * Every widget that is a variable becomes a property on the generated
	 * class, so a widget named Tick collides with UUserWidget::Tick and the
	 * compile dies with "Tried to create a property Tick in scope SKEL_..._C,
	 * but another object already exists there" - a message that names a
	 * skeleton class and never mentions the widget. Worse, nothing recompiles
	 * when a widget is added, so the failure surfaces on some later edit and
	 * looks like that edit caused it.
	 *
	 * FKismetNameValidator is what the designer's own rename box uses. It
	 * unions every inherited property, every inherited and generated function,
	 * the other widgets, the Blueprint's variables, its graphs, its timelines
	 * and its animations - a set no hand-written list of reserved words would
	 * keep up with.
	 *
	 * The exception is deliberate: a meta=(BindWidget) property on the parent
	 * class is a name the widget is SUPPOSED to take. The validator counts it
	 * as taken, because it is a property; the compiler then binds to it instead
	 * of generating a second one. So that case is allowed through first.
	 */
	/**
	 * Attach to a panel at a position rather than always at the end.
	 *
	 * AddChild appends, which is fine until order is the thing being authored -
	 * a row reading [icon][label] and one reading [label][icon] differ by
	 * nothing else. InsertChildAt takes the index and returns the slot; both it
	 * and GetChildIndex are identical in 5.7 and 5.8.
	 *
	 * An index past the end appends rather than failing: asking for position 9
	 * of 3 children means "last", and refusing it would only make callers count
	 * first.
	 */
	UPanelSlot* AttachAt(UPanelWidget* Parent, UWidget* Widget, int32 Index)
	{
		if (Index < 0 || Index >= Parent->GetChildrenCount())
		{
			return Parent->AddChild(Widget);
		}
		return Parent->InsertChildAt(Index, Widget);
	}

	/**
	 * The named slots a Widget Blueprint's own tree can fill.
	 *
	 * Two sources, and neither alone is the answer. UWidgetTree::GetSlotNames
	 * returns only slots that already HAVE content - its body is a GetKeys over
	 * NamedSlotBindings - so an empty inherited slot, which is exactly the one
	 * a caller wants to fill, is missing from it. The inherited list comes from
	 * the parent generated class instead.
	 *
	 * UWidgetBlueprint::GetInheritedAvailableNamedSlots dereferences
	 * GeneratedClass with no null check of its own, and a Widget Blueprint that
	 * has never been compiled has none - so the guard here is not defensive
	 * habit, it is the difference between a refusal and taking the editor down.
	 */
	TArray<FName> AvailableTreeSlots(const UWidgetBlueprint* WidgetBlueprint)
	{
		TArray<FName> Names;
		if (WidgetBlueprint->GeneratedClass)
		{
			Names = WidgetBlueprint->GetInheritedAvailableNamedSlots();
		}
		if (WidgetBlueprint->WidgetTree)
		{
			TArray<FName> Filled;
			WidgetBlueprint->WidgetTree->GetSlotNames(Filled);
			for (const FName& Each : Filled)
			{
				Names.AddUnique(Each);
			}
		}
		return Names;
	}

	bool WidgetNameIsUsable(
		const UWidgetBlueprint* WidgetBlueprint,
		const UClass* WidgetClass,
		const FName& Name,
		FString& OutError)
	{
		const UClass* ParentClass = WidgetBlueprint->ParentClass;
		if (ParentClass && WidgetClass)
		{
			if (const FObjectPropertyBase* Bind =
					CastField<FObjectPropertyBase>(ParentClass->FindPropertyByName(Name)))
			{
				if (FWidgetBlueprintEditorUtils::IsBindWidgetProperty(Bind)
					&& WidgetClass->IsChildOf(Bind->PropertyClass))
				{
					return true;
				}
			}
		}

		FKismetNameValidator Validator(WidgetBlueprint);
		const EValidatorResult Result = Validator.IsValid(Name);
		if (Result == EValidatorResult::Ok)
		{
			return true;
		}

		OutError = FString::Printf(
			TEXT("'%s' is not a usable widget name: %s A widget that is a variable becomes a property on the generated class, so the compile fails later with a message naming the skeleton class rather than the widget."),
			*Name.ToString(),
			*INameValidatorInterface::GetErrorString(Name.ToString(), Result));

		if (Result == EValidatorResult::AlreadyInUse && ParentClass)
		{
			if (const FProperty* Clash = ParentClass->FindPropertyByName(Name))
			{
				OutError += FString::Printf(
					TEXT(" %s::%s already exists."), *Clash->GetOwnerVariant().GetName(), *Name.ToString());
			}
			else if (const UFunction* ClashFn = ParentClass->FindFunctionByName(Name))
			{
				OutError += FString::Printf(
					TEXT(" %s::%s() already exists."), *ClashFn->GetOuterUClass()->GetName(), *Name.ToString());
			}
		}
		return false;
	}
}

void UplinkTools::RegisterWidget(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("widget_tree"),
		TEXT("List a Widget Blueprint's widget hierarchy: name, class, parent, and whether each widget is a variable (only variables can have events bound via bp_modify component_bound_event). Each row also carries 'path' and, for anything inside a panel, 'slot_path' and 'slot_class' - those are real object paths, so set_property writes a widget's own properties through the first and its layout through the second: a canvas child's position is 'LayoutData.Offsets.Left' on the slot, a box child's is 'Padding' or 'Size.Value'. That is how layout is authored here; there is no separate layout tool. 'named_slots' lists every named slot in play: ones inherited from the parent class (empty 'host', fillable here), ones a UserWidget in the tree exposes (under that widget's name), each with its 'content' when filled - and separately any NamedSlot widget this blueprint DECLARES for its own consumers, marked with 'declared_by'. An empty slot is listed rather than omitted, which is the whole point: it is the one a caller can fill. The reply carries 'total' and 'truncated' so a capped list is never mistaken for the whole tree."),
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

			// Named slots are holes this Blueprint leaves for whoever uses it,
			// and an EMPTY one is the interesting case - it is the thing a
			// caller can fill and the thing that is otherwise invisible. The
			// engine's own listing walks WidgetTree->NamedSlotBindings, which
			// only holds slots that already have content, so it can never
			// report one as available.
			TArray<TSharedPtr<FJsonValue>> Slots;
			auto AddSlots = [&Slots](INamedSlotInterface* Host, const FString& HostName)
			{
				if (!Host)
				{
					return;
				}
				TArray<FName> Names;
				Host->GetSlotNames(Names);
				for (const FName& Name : Names)
				{
					const UWidget* Content = Host->GetContentForSlot(Name);
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("slot"), Name.ToString());
					Row->SetStringField(TEXT("host"), HostName);
					if (Content)
					{
						Row->SetStringField(TEXT("content"), Content->GetName());
					}
					Slots.Add(MakeShared<FJsonValueObject>(Row));
				}
			};

			if (WidgetBlueprint->WidgetTree)
			{
				// The tree hosts the slots inherited from the parent class.
				for (const FName& Name : AvailableTreeSlots(WidgetBlueprint))
				{
					const UWidget* Content = WidgetBlueprint->WidgetTree->GetContentForSlot(Name);
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("slot"), Name.ToString());
					Row->SetStringField(TEXT("host"), FString());
					Row->SetBoolField(TEXT("inherited"), true);
					if (Content)
					{
						Row->SetStringField(TEXT("content"), Content->GetName());
					}
					Slots.Add(MakeShared<FJsonValueObject>(Row));
				}
				WidgetBlueprint->WidgetTree->ForEachWidget([&AddSlots, &Slots, WidgetBlueprint](UWidget* Widget)
				{
					if (!Widget)
					{
						return;
					}
					AddSlots(Cast<INamedSlotInterface>(Widget), Widget->GetName());

					// A NamedSlot widget is the other kind of entry entirely: it
					// does not implement INamedSlotInterface and hosts nothing.
					// It DECLARES a slot that whoever instantiates this
					// blueprint can fill - so it belongs in the list, marked as
					// what it is, or a caller reads an empty list and concludes
					// the blueprint offers no slots when offering one is the
					// whole reason the widget is there.
					if (Widget->IsA<UNamedSlot>())
					{
						TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
						Row->SetStringField(TEXT("slot"), Widget->GetName());
						Row->SetStringField(TEXT("declared_by"), WidgetBlueprint->GetName());
						Slots.Add(MakeShared<FJsonValueObject>(Row));
					}
				});
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("blueprint"), WidgetBlueprint->GetPathName());
			Data->SetArrayField(TEXT("widgets"), Rows);
			Data->SetNumberField(TEXT("total"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Rows.Num());
			if (Slots.Num() > 0)
			{
				Data->SetArrayField(TEXT("named_slots"), Slots);
			}
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("widget_add"),
		TEXT("Construct a widget into a Widget Blueprint's tree (as a variable, so its events are immediately bindable). Becomes the root if the tree is empty; otherwise pass a panel widget as 'parent' (or omit to attach to the root panel), with 'index' to place it among that panel's existing children rather than last. The name is checked against everything the Blueprint compiler reserves - inherited properties and functions, the other widgets, variables, graphs, animations - because a widget that is a variable becomes a property, so naming one Tick or Slot compiles until some later edit and then fails naming a skeleton class rather than the widget. A meta=(BindWidget) name on the parent class is allowed through, since taking it is the point."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string"},"class":{"type":"string","description":"Widget class, e.g. /Script/UMG.Button or /Script/UMG.TextBlock"},"name":{"type":"string"},"parent":{"type":"string","description":"Name of an existing panel widget to add into"},"index":{"type":"number","description":"Position among the parent's children; appended when omitted"}},"required":["blueprint","class","name"]})json"),
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

			FString NameError;
			if (!WidgetNameIsUsable(WidgetBlueprint, WidgetClass, WidgetName, NameError))
			{
				return FUplinkToolResult::Error(NameError);
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

			// -1 means "not given", which AttachAt reads as append.
			const int32 InsertIndex = Ctx.Params->HasField(TEXT("index"))
				? static_cast<int32>(GetNumber(Ctx.Params, TEXT("index"), -1.0))
				: -1;

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
				if (!AttachAt(Parent, NewWidget, InsertIndex))
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
				if (!AttachAt(RootPanel, NewWidget, InsertIndex))
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
		TEXT("Change a Widget Blueprint's tree: 'remove' takes a widget (and everything under it) out, 'reparent' moves one into a different panel - or, with the same panel and an 'index', reorders it in place. The slot's layout is carried across the move for every field the old and new slot classes share. Removing is refused while a graph node still reads or writes the widget, and those nodes are named: the engine's delete takes them with it, so forcing does not leave a broken graph - it leaves one that has quietly lost logic, and it still compiles clean afterwards. force:true does it anyway. 'set_slot' puts a widget into a named slot and 'clear_slot' empties one - name the declaring widget as 'host', or omit it for a slot inherited from the parent class; widget_tree's 'named_slots' lists what is there. 'bind_property' is the designer's Bind dropdown - the property stops being a stored value and is re-read from a pure function every frame; it is refused for a property with no matching <Name>Delegate on its class, for a widget that is not a variable, and for an impure function, each of which the compiler would otherwise report against the widget rather than the cause. Only a full compile copies bindings onto the generated class, so compile before checking one. Layout is not here: a widget and its slot are ordinary objects, so set_property against the 'path' and 'slot_path' that widget_tree reports is how position, size and padding are written."),
		TEXT(R"json({"type":"object","properties":{"blueprint":{"type":"string","description":"Widget Blueprint asset path"},"op":{"type":"string","enum":["remove","reparent","set_slot","clear_slot","bind_property","unbind_property"]},"widget":{"type":"string","description":"Name of the widget to act on"},"parent":{"type":"string","description":"reparent: name of the panel widget to move it into"},"index":{"type":"number","description":"reparent: position among that panel's children. Same panel + index reorders in place; omitted appends"},"force":{"type":"boolean","default":false,"description":"remove: delete even though graph nodes still reference the widget"},"slot":{"type":"string","description":"set_slot/clear_slot: the named slot"},"host":{"type":"string","description":"set_slot/clear_slot: widget declaring the slot. Omit for a slot inherited from the parent class"},"property":{"type":"string","description":"bind_property/unbind_property: the widget property, e.g. Text - not TextDelegate"},"function":{"type":"string","description":"bind_property: pure, no parameters, returns the property's type"}},"required":["blueprint","op"]})json"),
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

			// clear_slot names a slot, not a widget - everything else needs one.
			const bool bNeedsWidget = GetString(Ctx.Params, TEXT("op")) != TEXT("clear_slot");
			if (WidgetName.IsEmpty() && bNeedsWidget)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' needs 'widget'"), *GetString(Ctx.Params, TEXT("op"))));
			}
			if (!Widget && bNeedsWidget)
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
			if (Widget)
			{
				Data->SetStringField(TEXT("widget"), Widget->GetName());
			}
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

				const int32 InsertIndex = Ctx.Params->HasField(TEXT("index"))
					? static_cast<int32>(GetNumber(Ctx.Params, TEXT("index"), -1.0))
					: -1;

				UPanelWidget* OldParent = Widget->GetParent();
				if (OldParent == NewParent && InsertIndex < 0)
				{
					Data->SetStringField(TEXT("parent"), NewParent->GetName());
					return FUplinkToolResult::Ok(Data, FString::Printf(
						TEXT("%s is already in %s - pass 'index' to move it within that panel"),
						*WidgetName, *ParentName));
				}

				// No adjustment for the removal that is about to happen. The
				// index means the position in the FINAL list, and detaching
				// first then inserting at that index already produces it: from
				// [Label, Icon], moving Label to 1 leaves [Icon], and inserting
				// at 1 gives [Icon, Label]. Compensating for the shift moves it
				// one place short.
				const int32 TargetIndex = InsertIndex;

				// The layout lives on the slot, and the slot is destroyed with
				// the old parenting. The engine carries it across as text, which
				// works even between different slot classes - the fields that
				// exist in both survive, the rest fall away. Without this every
				// reorder silently resets padding and alignment.
				TMap<FName, FString> SlotProperties;
				if (Widget->Slot)
				{
					FWidgetBlueprintEditorUtils::ExportPropertiesToText(Widget->Slot, SlotProperties);
				}

				// Detach first so the old slot is released, then attach - and put
				// it back if the new panel will not take it, because a widget
				// parented to nothing is in the tree and in no layout, which the
				// designer shows as simply missing.
				Widget->RemoveFromParent();
				if (!AttachAt(NewParent, Widget, TargetIndex))
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

				if (Widget->Slot && SlotProperties.Num() > 0)
				{
					FWidgetBlueprintEditorUtils::ImportPropertiesFromText(Widget->Slot, SlotProperties);
				}

				Data->SetStringField(TEXT("parent"), NewParent->GetName());
				Data->SetNumberField(TEXT("index"), NewParent->GetChildIndex(Widget));
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
					TEXT("moved %s into %s at index %d - its layout is now a %s, carried across from the old slot where the two share a field"),
					*WidgetName, *ParentName, NewParent->GetChildIndex(Widget),
					Widget->Slot ? *Widget->Slot->GetClass()->GetName() : TEXT("slot")));
			}

			if (Op == TEXT("set_slot") || Op == TEXT("clear_slot"))
			{
				// A named slot is a hole a Widget Blueprint leaves for whoever
				// uses it to fill. The host is either a widget in this tree that
				// declares one, or the tree itself for a slot inherited from the
				// parent class - hence two ways to resolve it.
				const FString SlotName = GetString(Ctx.Params, TEXT("slot"));
				const FString HostName = GetString(Ctx.Params, TEXT("host"));

				INamedSlotInterface* Host = nullptr;
				FString HostLabel;
				if (HostName.IsEmpty())
				{
					Host = WidgetBlueprint->WidgetTree;
					HostLabel = TEXT("the widget tree");
				}
				else if (UWidget* HostWidget = WidgetBlueprint->WidgetTree->FindWidget(FName(*HostName)))
				{
					Host = Cast<INamedSlotInterface>(HostWidget);
					HostLabel = HostWidget->GetName();
					if (!Host)
					{
						return FUplinkToolResult::Error(HostWidget->IsA<UNamedSlot>()
							? FString::Printf(
								TEXT("'%s' is a NamedSlot, which DECLARES a slot rather than hosting one - it is the hole this blueprint offers to whoever instantiates it, and it gets filled from there, not from here. Inside this blueprint, put content under it the ordinary way with widget_add parent:'%s'."),
								*HostName, *HostName)
							: FString::Printf(
								TEXT("'%s' is a %s, which hosts no named slots. widget_tree's 'named_slots' lists the ones that are there."),
								*HostName, *HostWidget->GetClass()->GetName()));
					}
				}
				else
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no widget named '%s' in this tree"), *HostName));
				}

				TArray<FName> Declared;
				if (HostName.IsEmpty())
				{
					Declared = AvailableTreeSlots(WidgetBlueprint);
				}
				else
				{
					Host->GetSlotNames(Declared);
				}
				if (!Declared.Contains(FName(*SlotName)))
				{
					TArray<FString> Names;
					for (const FName& Each : Declared)
					{
						Names.Add(Each.ToString());
					}
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("%s has no named slot '%s'.%s"),
						*HostLabel, *SlotName,
						Names.Num() > 0
							? *FString::Printf(TEXT(" It has: %s"), *FString::Join(Names, TEXT(", ")))
							: TEXT(" It has none. A blueprint's own tree can only fill slots INHERITED from its parent class - a NamedSlot widget placed here declares a slot for this blueprint's consumers instead, and is filled by them.")));
				}

				Data->SetStringField(TEXT("slot"), SlotName);
				Data->SetStringField(TEXT("host"), HostLabel);
				WidgetBlueprint->Modify();

				if (Op == TEXT("clear_slot"))
				{
					Host->SetContentForSlot(FName(*SlotName), nullptr);
					const bool bCleared = Host->GetContentForSlot(FName(*SlotName)) == nullptr;
					Data->SetBoolField(TEXT("filled"), !bCleared);
					if (!bCleared)
					{
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("'%s' still has content after clearing it"), *SlotName));
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
					return FUplinkToolResult::Ok(Data, FString::Printf(
						TEXT("cleared %s on %s - the widget that was in it is still in the tree, unparented"),
						*SlotName, *HostLabel));
				}

				UWidget* Content = WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName));
				if (!Content)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no widget named '%s' to put in the slot"), *WidgetName));
				}
				if (Content == Cast<UWidget>(Host))
				{
					return FUplinkToolResult::Error(TEXT("a widget cannot be the content of its own slot"));
				}

				// A widget that is still a panel's child would end up in two
				// places at once - drawn by the panel and by the slot - so it is
				// detached first, the same way the engine's own path does it.
				if (Content->Slot && Content->Slot->Parent)
				{
					Content->Slot->Parent->RemoveChild(Content);
				}
				Host->SetContentForSlot(FName(*SlotName), Content);

				// SetContentForSlot fails silently when the host does not
				// actually own the slot, so the answer is read back rather than
				// assumed.
				UWidget* Landed = Host->GetContentForSlot(FName(*SlotName));
				Data->SetBoolField(TEXT("filled"), Landed != nullptr);
				if (Landed != Content)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("%s did not take '%s' into slot '%s' - it holds %s instead"),
						*HostLabel, *WidgetName, *SlotName,
						Landed ? *Landed->GetName() : TEXT("nothing")));
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				return FUplinkToolResult::Ok(Data, FString::Printf(
					TEXT("%s is now the content of %s on %s"), *WidgetName, *SlotName, *HostLabel));
			}

			if (Op == TEXT("bind_property") || Op == TEXT("unbind_property"))
			{
				// A property binding is the designer's "Bind" dropdown: the
				// property stops being a stored value and is re-read from a
				// function every frame. It is stored as one entry in the
				// Blueprint's Bindings array, and only a FULL compile copies
				// those onto the generated class - MarkBlueprintAsStructurallyModified
				// is skeleton-only and would leave the binding inert.
				const FName PropertyName(*GetString(Ctx.Params, TEXT("property")));
				if (PropertyName.IsNone())
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("'%s' needs 'property'"), *Op));
				}

				Data->SetStringField(TEXT("property"), PropertyName.ToString());
				WidgetBlueprint->Modify();

				if (Op == TEXT("unbind_property"))
				{
					const int32 Removed = WidgetBlueprint->Bindings.RemoveAll(
						[&Widget, &PropertyName](const FDelegateEditorBinding& Each)
						{
							return Each.ObjectName == Widget->GetName() && Each.PropertyName == PropertyName;
						});
					Data->SetNumberField(TEXT("removed"), Removed);
					if (Removed == 0)
					{
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("'%s' on %s was not bound"), *PropertyName.ToString(), *WidgetName));
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
					return FUplinkToolResult::Ok(Data, FString::Printf(
						TEXT("unbound %s on %s - compile for it to take effect"),
						*PropertyName.ToString(), *WidgetName));
				}

				// Only a property with a sibling <Name>Delegate can be bound;
				// the compiler and the runtime both look that up by name. Asking
				// first turns a compile error nobody can place into a refusal
				// that names the bindable properties.
				const FString DelegateName = PropertyName.ToString() + TEXT("Delegate");
				if (!FindFProperty<FDelegateProperty>(Widget->GetClass(), *DelegateName))
				{
					TArray<FString> Bindable;
					for (TFieldIterator<FDelegateProperty> It(Widget->GetClass()); It && Bindable.Num() < 20; ++It)
					{
						FString Each = It->GetName();
						if (Each.RemoveFromEnd(TEXT("Delegate")))
						{
							Bindable.Add(Each);
						}
					}
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' on %s cannot be bound - a bindable property needs a matching %s on its class, and %s has none. Bindable here: %s."),
						*PropertyName.ToString(), *Widget->GetClass()->GetName(), *DelegateName,
						*Widget->GetClass()->GetName(),
						Bindable.Num() > 0 ? *FString::Join(Bindable, TEXT(", ")) : TEXT("nothing")));
				}

				if (!Widget->bIsVariable)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("%s is not a variable, and a binding resolves its widget through the generated class's variable map - so it would compile and never fire"),
						*WidgetName));
				}

				const FName FunctionName(*GetString(Ctx.Params, TEXT("function")));
				const UFunction* Function = WidgetBlueprint->SkeletonGeneratedClass
					? WidgetBlueprint->SkeletonGeneratedClass->FindFunctionByName(FunctionName)
					: nullptr;
				if (!Function)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no function '%s' on this Widget Blueprint - bp_modify add_function makes one, and it must be pure with no parameters and a return value matching the property"),
						*FunctionName.ToString()));
				}

				// The compiler refuses an impure binding with "needs to be bound
				// to a pure function", pointing at the widget rather than at the
				// function - so the same check runs here where the function is
				// in hand.
				if (!Function->HasAnyFunctionFlags(FUNC_BlueprintPure | FUNC_Const))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' is not pure, and a property binding is evaluated during layout so it has to be - pass pure:true to bp_modify add_function"),
						*FunctionName.ToString()));
				}

				FDelegateEditorBinding Binding;
				Binding.ObjectName = Widget->GetName();
				Binding.PropertyName = PropertyName;
				Binding.FunctionName = FunctionName;
				Binding.Kind = EBindingKind::Function;

				WidgetBlueprint->Bindings.RemoveAll(
					[&Binding](const FDelegateEditorBinding& Each)
					{
						return Each.ObjectName == Binding.ObjectName && Each.PropertyName == Binding.PropertyName;
					});
				WidgetBlueprint->Bindings.Add(Binding);

				Data->SetStringField(TEXT("function"), FunctionName.ToString());
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				return FUplinkToolResult::Ok(Data, FString::Printf(
					TEXT("%s on %s now reads from %s - compile before checking it, since only a full compile copies bindings onto the generated class"),
					*PropertyName.ToString(), *WidgetName, *FunctionName.ToString()));
			}

			return FUplinkToolResult::Error(FString::Printf(
				TEXT("unknown op '%s' - one of remove, reparent, set_slot, clear_slot, bind_property, unbind_property"), *Op));
		});

}
