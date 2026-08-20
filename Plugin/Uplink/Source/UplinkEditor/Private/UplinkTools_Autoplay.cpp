// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
//
// Tools for driving a project nobody has explained to you.
//
// Every one of these exists because its absence cost real time on a real game:
// starting on the gameplay map gave an empty world because the game had to be
// entered through its menu; a menu was discovered only by reading the error
// message of a tool that happened to list widgets; a component was unreachable
// because its name was CharMoveComp and not CharacterMovement; and a modal
// dialog froze every call with no way to even report it.

#include "UplinkTools.h"
#include "UplinkObservation.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/TextBlock.h"
#include "Editor.h"
#include "Engine/GameInstance.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputSubsystems.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "GameMapsSettings.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "AssetRegistry/AssetRegistryModule.h"

using namespace UplinkToolUtil;

namespace
{
	IAssetRegistry& Registry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

}

namespace UplinkObservation
{
	void GatherLiveWidgets(UWorld* World, TArray<UUserWidget*>& Out)
	{
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			UUserWidget* Widget = *It;
			if (!Widget || Widget->IsTemplate() || !IsValid(Widget))
			{
				continue;
			}
			if (World && Widget->GetWorld() != World)
			{
				continue;
			}
			if (Widget->IsInViewport())
			{
				Out.Add(Widget);
			}
		}
	}

}

namespace UplinkObservation
{
	void ForEachWidgetInScreen(UUserWidget* Screen, TSet<UWidget*>& Seen, TFunctionRef<void(UWidget*, UUserWidget*)> Visit)
	{
		if (!Screen || !Screen->WidgetTree)
		{
			return;
		}

		TArray<UUserWidget*> Nested;
		Screen->WidgetTree->ForEachWidget([Screen, &Seen, &Nested, &Visit](UWidget* Widget)
		{
			if (!Widget || Seen.Contains(Widget))
			{
				return;
			}
			Seen.Add(Widget);
			Visit(Widget, Screen);
			if (UUserWidget* AsUserWidget = Cast<UUserWidget>(Widget))
			{
				Nested.Add(AsUserWidget);
			}
		});

		for (UUserWidget* Child : Nested)
		{
			ForEachWidgetInScreen(Child, Seen, Visit);
		}
	}
}

void UplinkTools::RegisterAutoplay(FUplinkToolRegistry& InRegistry)
{
	// ------------------------------------------------------------------
	// project_entry - how do I start this game the way a player does?
	// ------------------------------------------------------------------
	InRegistry.RegisterQuick(
		TEXT("project_entry"),
		TEXT("How to start this project the way a player does. Reports the game's default map, the editor startup map, the global default game mode, the current level's own game-mode override and running game mode, and every map under /Game (maps that ship inside a plugin are not listed). 'streamedLevelCount' counts the sublevels of the current world only - use streaming_status to see them. Read this FIRST on an unfamiliar project: starting play on a gameplay map directly can give an empty world, because a game whose menu loads the level expects to be entered through that menu - the difference showed up as 40 actors versus 1917 on the same map."),
		TEXT(R"json({"type":"object","properties":{"max_maps":{"type":"number","default":40}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

			Data->SetStringField(TEXT("gameDefaultMap"), UGameMapsSettings::GetGameDefaultMap());
			Data->SetStringField(TEXT("globalDefaultGameMode"), UGameMapsSettings::GetGlobalDefaultGameMode());
			if (const UGameMapsSettings* Settings = GetDefault<UGameMapsSettings>())
			{
				Data->SetStringField(TEXT("editorStartupMap"), Settings->EditorStartupMap.ToString());
			}

			FString WorldError;
			if (UWorld* World = Ctx.ResolveWorld(WorldError))
			{
				Data->SetStringField(TEXT("currentMap"), World->GetOutermost()->GetName());
				if (const AWorldSettings* WorldSettings = World->GetWorldSettings())
				{
					Data->SetStringField(TEXT("levelGameModeOverride"),
						WorldSettings->DefaultGameMode ? WorldSettings->DefaultGameMode->GetPathName() : FString());
				}
				if (const AGameModeBase* GameMode = World->GetAuthGameMode())
				{
					Data->SetStringField(TEXT("runningGameMode"), GameMode->GetClass()->GetPathName());
				}
				Data->SetNumberField(TEXT("streamedLevelCount"), World->GetStreamingLevels().Num());
			}
			else
			{
				// Half of this answer is about the current level, and
				// swallowing the reason left those fields simply absent,
				// which reads as a project that has none of them.
				Data->SetStringField(TEXT("worldError"), WorldError);
			}

			// Every map under /Game, so the caller can see what exists rather
			// than guessing names.
			const int32 MaxMaps = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max_maps"), 40.0)), 1, 500);
			FARFilter Filter;
			Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("World")));
			Filter.bRecursiveClasses = true;
			Filter.PackagePaths.Add(TEXT("/Game"));
			Filter.bRecursivePaths = true;

			TArray<FAssetData> Maps;
			Registry().GetAssets(Filter, Maps);

			TArray<TSharedPtr<FJsonValue>> MapList;
			for (const FAssetData& Map : Maps)
			{
				if (MapList.Num() >= MaxMaps)
				{
					break;
				}
				MapList.Add(MakeShared<FJsonValueString>(Map.PackageName.ToString()));
			}
			Data->SetArrayField(TEXT("maps"), MapList);
			Data->SetNumberField(TEXT("mapsTotal"), Maps.Num());
			Data->SetBoolField(TEXT("truncated"), Maps.Num() > MapList.Num());

			// A registry still building its index answers with whatever it has
			// so far, and a short map list is indistinguishable from a small
			// project unless it says which one this is.
			const bool bScanning = Registry().IsLoadingAssets();
			Data->SetBoolField(TEXT("assetRegistryScanning"), bScanning);

			return FUplinkToolResult::Ok(Data, bScanning
				? TEXT("the asset registry is still scanning, so this map list is partial - ask again in a few seconds")
				: TEXT("if the game has a menu that loads the level, start play on the menu map and let it open the gameplay level itself"));
		});

	// ------------------------------------------------------------------
	// ui_live - what is on screen, and what can I click?
	// ------------------------------------------------------------------
	InRegistry.RegisterQuick(
		TEXT("ui_live"),
		TEXT("What UMG is on screen right now: every widget in every UserWidget that was added to the viewport, nested sub-widgets included, with its class, any text it displays, screen rect, visibility, and whether a click could land on it ('interactive' means hit-testable and laid out; a widget set HitTestInvisible lets clicks pass straight through). This is how you find a menu without reading its Blueprint - the names here are what click_widget takes, and 'rect' is [x, y, width, height] in the same desktop pixels click_widget's 'position' takes, which are not viewport-relative screenshot pixels. UI drawn on a WidgetComponent in the world is not covered, because nothing added it to a viewport. Each row also carries 'object_path' for the element and 'screen_path' for the UserWidget whose tree it lives in - a nested sub-widget rather than the outer screen, when the element sits inside one - and both are what get_property, set_property and call_function accept - so a menu you can see is also one you can read a variable off or call a function on, which matters when a widget swallows input and no key or click will advance it. A widget with a zero rect exists but is not laid out, which usually means the screen it belongs to has not been shown yet. PIE only."),
		TEXT(R"json({"type":"object","properties":{"contains":{"type":"string","description":"Only widgets whose name, class or text contains this"},"interactive_only":{"type":"boolean","default":false,"description":"Only widgets that accept a click"},"on_screen_only":{"type":"boolean","default":true,"description":"Skip widgets with no laid-out geometry"},"max":{"type":"number","default":80},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			if (!World)
			{
				return FUplinkToolResult::Error(Error);
			}

			const FString Filter = GetString(Ctx.Params, TEXT("contains"));
			bool bInteractiveOnly = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("interactive_only")), bInteractiveOnly);
			bool bOnScreenOnly = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("on_screen_only")), bOnScreenOnly);
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 80.0)), 1, 500);

			TArray<UUserWidget*> Roots;
			UplinkObservation::GatherLiveWidgets(World, Roots);

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Total = 0;
			TSet<UWidget*> Seen;

			for (UUserWidget* Root : Roots)
			{
				if (!Root || !Root->WidgetTree)
				{
					continue;
				}
				UplinkObservation::ForEachWidgetInScreen(Root, Seen, [&](UWidget* Widget, UUserWidget* Owner)
				{
					if (!Widget || !Owner)
					{
						return;
					}
					const FGeometry Geometry = Widget->GetCachedGeometry();
					const FVector2D Size = Geometry.GetAbsoluteSize();
					const bool bLaidOut = Size.X > 0.0 && Size.Y > 0.0;
					if (bOnScreenOnly && !bLaidOut)
					{
						return;
					}

					// Text is what a human reads off the screen, so it is the
					// most useful handle for "click the New Game button".
					FString Text;
					if (const UTextBlock* AsText = Cast<UTextBlock>(Widget))
					{
						Text = AsText->GetText().ToString();
					}

					const FString Name = Widget->GetName();
					const FString ClassName = Widget->GetClass()->GetName();
					if (!Filter.IsEmpty()
						&& !Name.Contains(Filter, ESearchCase::IgnoreCase)
						&& !ClassName.Contains(Filter, ESearchCase::IgnoreCase)
						&& !Text.Contains(Filter, ESearchCase::IgnoreCase))
					{
						return;
					}

					// Visible is the only hit-testable state - HitTestInvisible
					// and SelfHitTestInvisible are the engine's words for
					// "clicks pass through me" - and a widget with no geometry
					// has nowhere for a click to land, so answering "what can I
					// click" honestly takes both.
					const bool bInteractive = bLaidOut && Widget->GetVisibility() == ESlateVisibility::Visible;
					if (bInteractiveOnly && !bInteractive)
					{
						return;
					}

					++Total;
					if (Rows.Num() >= Max)
					{
						return;
					}

					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Name);
					Row->SetStringField(TEXT("class"), ClassName);
					// The UserWidget itself, which is what carries the Blueprint's
					// own variables and functions - a page index, a Next handler.
					// The row's object_path reaches the individual element; this
					// reaches the widget owning it, and they are rarely the one
					// you want twice.
					Row->SetStringField(TEXT("screen"), Owner->GetName());
					Row->SetStringField(TEXT("screen_path"), Owner->GetPathName());
					// The path get_property, set_property and call_function take.
					// Without it this tool could show you a menu it gave you no way
					// to reach: a runtime UserWidget is outered to the transient
					// package under a generated name, so there is nothing a caller
					// could reasonably guess. Seeing a widget and being able to ask
					// it a question should not be two separate problems.
					Row->SetStringField(TEXT("object_path"), Widget->GetPathName());
					if (!Text.IsEmpty())
					{
						Row->SetStringField(TEXT("text"), Text);
					}
					const UEnum* VisibilityEnum = StaticEnum<ESlateVisibility>();
					Row->SetStringField(TEXT("visibility"), VisibilityEnum
						? VisibilityEnum->GetNameStringByValue(static_cast<int64>(Widget->GetVisibility()))
						: FString());
					// Promised by the description since this tool existed, and
					// never actually sent: interactive_only filtered on it
					// while every row kept the answer to itself.
					Row->SetBoolField(TEXT("interactive"), bInteractive);
					Row->SetBoolField(TEXT("onScreen"), bLaidOut);
					if (bLaidOut)
					{
						const FVector2D Position = Geometry.GetAbsolutePosition();
						TArray<TSharedPtr<FJsonValue>> Rect;
						Rect.Add(MakeShared<FJsonValueNumber>(Position.X));
						Rect.Add(MakeShared<FJsonValueNumber>(Position.Y));
						Rect.Add(MakeShared<FJsonValueNumber>(Size.X));
						Rect.Add(MakeShared<FJsonValueNumber>(Size.Y));
						Row->SetArrayField(TEXT("rect"), Rect);
					}
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				});
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("widgets"), Rows);
			Data->SetNumberField(TEXT("screens"), Roots.Num());
			Data->SetNumberField(TEXT("total"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Rows.Num());
			return FUplinkToolResult::Ok(Data, Roots.Num() == 0
				? TEXT("nothing is on screen (is the game running?)")
				: FString());
		});

	// ------------------------------------------------------------------
	// actor_components - what is this actor actually made of?
	// ------------------------------------------------------------------
	InRegistry.RegisterQuick(
		TEXT("actor_components"),
		TEXT("List a live actor's components with the names get_property and set_property expect. Guessing is unreliable: a character's movement component is called CharMoveComp, not CharacterMovement, and the wrong guess simply reports that the component does not exist."),
		TEXT(R"json({"type":"object","properties":{"actor":{"type":"string"},"class_contains":{"type":"string"},"max":{"type":"number","default":60},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["actor"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			if (!World)
			{
				return FUplinkToolResult::Error(Error);
			}
			const FString Wanted = GetString(Ctx.Params, TEXT("actor"));
			AActor* Actor = FindActor(World, Wanted);
			if (!Actor)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("no actor named '%s' in this world"), *Wanted));
			}

			const FString ClassFilter = GetString(Ctx.Params, TEXT("class_contains"));
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 60.0)), 1, 500);

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Total = 0;
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (!Component)
				{
					continue;
				}
				const FString ClassName = Component->GetClass()->GetName();
				if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				++Total;
				if (Rows.Num() >= Max)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Component->GetName());
				Row->SetStringField(TEXT("class"), ClassName);
				Row->SetBoolField(TEXT("active"), Component->IsActive());
				if (const USceneComponent* AsScene = Cast<USceneComponent>(Component))
				{
					Row->SetBoolField(TEXT("visible"), AsScene->IsVisible());
					Row->SetStringField(TEXT("attachedTo"),
						AsScene->GetAttachParent() ? AsScene->GetAttachParent()->GetName() : FString());
					Row->SetNumberField(TEXT("children"), AsScene->GetNumChildrenComponents());
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("actor"), Actor->GetName());
			Data->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
			Data->SetArrayField(TEXT("components"), Rows);
			Data->SetNumberField(TEXT("total"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Rows.Num());
			return FUplinkToolResult::Ok(Data);
		});

	// ------------------------------------------------------------------
	// streaming_status - is the world still loading, or is it just empty?
	// ------------------------------------------------------------------
	InRegistry.RegisterQuick(
		TEXT("streaming_status"),
		TEXT("Which sublevels are loaded, visible, or still coming. Without this an unfinished stream is indistinguishable from a broken level or an empty one - the only clue is an actor count that looks too low, which is exactly how a game that had not been entered through its menu looked. 'settled' means nothing is mid-transition, not that everything is loaded: a sublevel the game deliberately leaves unloaded is settled, and 'pending' names the ones actually moving. 'actorCount' counts the levels that are loaded now, so it climbs as they arrive."),
		TEXT(R"json({"type":"object","properties":{"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			if (!World)
			{
				return FUplinkToolResult::Error(Error);
			}

			int32 Loaded = 0;
			int32 Visible = 0;
			int32 Pending = 0;
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (ULevelStreaming* Streaming : World->GetStreamingLevels())
			{
				if (!Streaming)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("level"), Streaming->GetWorldAssetPackageName());
				const bool bLoaded = Streaming->IsLevelLoaded();
				const bool bVisible = Streaming->IsLevelVisible();
				// The engine's own word for "mid-transition": loaded state does
				// not match what was asked for, or the package is still on its
				// way in or out.
				const bool bPending = Streaming->IsStreamingStatePending();
				Row->SetBoolField(TEXT("loaded"), bLoaded);
				Row->SetBoolField(TEXT("visible"), bVisible);
				Row->SetBoolField(TEXT("shouldBeLoaded"), Streaming->ShouldBeLoaded());
				Row->SetBoolField(TEXT("shouldBeVisible"), Streaming->ShouldBeVisible());
				Row->SetBoolField(TEXT("pending"), bPending);
				Loaded += bLoaded ? 1 : 0;
				Visible += bVisible ? 1 : 0;
				Pending += bPending ? 1 : 0;
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			int32 ActorCount = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				++ActorCount;
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("persistentLevel"), World->GetOutermost()->GetName());
			Data->SetArrayField(TEXT("streamingLevels"), Rows);
			Data->SetNumberField(TEXT("streamingCount"), Rows.Num());
			Data->SetNumberField(TEXT("loaded"), Loaded);
			Data->SetNumberField(TEXT("visible"), Visible);
			Data->SetNumberField(TEXT("actorCount"), ActorCount);
			Data->SetNumberField(TEXT("pending"), Pending);
			// Settled means nothing is still moving, not "everything is
			// loaded". Comparing loaded against the sublevel count called a
			// world unsettled forever whenever it held a sublevel the game
			// never intends to load, which is most of them.
			Data->SetBoolField(TEXT("settled"), Pending == 0);

			return FUplinkToolResult::Ok(Data, Pending > 0
				? TEXT("sublevels are still coming or going - a low actor count here is not an empty level; ask again in a second")
				: FString());
		});

	// ------------------------------------------------------------------
	// input_map - what can I press, and what does it do?
	// ------------------------------------------------------------------
	InRegistry.RegisterQuick(
		TEXT("input_map"),
		TEXT("Enhanced Input mapping contexts with their actions and the keys bound to them, and - during play - which of them are actually applied to the player. Use it to find the real move/interact/confirm actions instead of searching assets by name and hoping. 'applied' is only computed for the contexts the scan covers, so a context that lives in a plugin is not merely unapplied, it is unseen: pass path_prefix '/' to scan every mounted root when the game's input comes from somewhere other than /Game."),
		TEXT(R"json({"type":"object","properties":{"path_prefix":{"type":"string","default":"/Game","description":"Package path to scan; '/' or empty means every mounted root, plugins included"},"applied_only":{"type":"boolean","default":false,"description":"Only contexts currently applied to the player (PIE)"},"max":{"type":"number","default":20}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString PathPrefix = GetString(Ctx.Params, TEXT("path_prefix"), TEXT("/Game"));
			bool bAppliedOnly = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("applied_only")), bAppliedOnly);
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 20.0)), 1, 100);

			// The live subsystem tells us which contexts are actually in force.
			UEnhancedInputLocalPlayerSubsystem* Input = nullptr;
			bool bPieRunning = false;
			if (UWorld* PieWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr)
			{
				bPieRunning = true;
				if (const APlayerController* PC = PieWorld->GetFirstPlayerController())
				{
					if (ULocalPlayer* LocalPlayer = PC->GetLocalPlayer())
					{
						Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer);
					}
				}
			}

			FARFilter Filter;
			Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/EnhancedInput"), TEXT("InputMappingContext")));
			// No package path at all is how the registry spells "every mounted
			// root". The default stays /Game, but a game whose input context
			// ships inside a plugin needs a way to be seen, and scoping the
			// scan silently scoped the applied answer with it.
			if (!PathPrefix.IsEmpty() && PathPrefix != TEXT("/"))
			{
				Filter.PackagePaths.Add(FName(*PathPrefix));
				Filter.bRecursivePaths = true;
			}

			TArray<FAssetData> Contexts;
			Registry().GetAssets(Filter, Contexts);

			TArray<TSharedPtr<FJsonValue>> Rows;
			bool bTruncated = false;
			for (const FAssetData& Asset : Contexts)
			{
				if (Rows.Num() >= Max)
				{
					// Stopping here also stops the applied test, so the rest of
					// the project is unexamined rather than unapplied.
					bTruncated = true;
					break;
				}
				const UInputMappingContext* Context = Cast<UInputMappingContext>(Asset.GetAsset());
				if (!Context)
				{
					continue;
				}
				const bool bApplied = Input && Input->HasMappingContext(Context);
				if (bAppliedOnly && !bApplied)
				{
					continue;
				}

				// Group the keys by action, which is the way a person thinks
				// about it: "what moves the character".
				TMap<FString, TArray<FString>> KeysByAction;
				TMap<FString, FString> PathByAction;
				for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
				{
					const FString ActionName = Mapping.Action ? Mapping.Action->GetName() : TEXT("(none)");
					KeysByAction.FindOrAdd(ActionName).Add(Mapping.Key.ToString());
					if (Mapping.Action)
					{
						PathByAction.FindOrAdd(ActionName) = Mapping.Action->GetPathName();
					}
				}

				TArray<TSharedPtr<FJsonValue>> Actions;
				for (const auto& Pair : KeysByAction)
				{
					TSharedRef<FJsonObject> ActionRow = MakeShared<FJsonObject>();
					ActionRow->SetStringField(TEXT("action"), Pair.Key);
					// The path, not just the name: input_action takes the asset
					// path, and guessing it from the name is wrong as often as
					// not - the third-person template keeps IA_Move under
					// /Game/Input/Actions/, one folder deeper than the context
					// that references it.
					if (const FString* Path = PathByAction.Find(Pair.Key))
					{
						ActionRow->SetStringField(TEXT("path"), *Path);
					}
					ActionRow->SetStringField(TEXT("keys"), FString::Join(Pair.Value, TEXT(", ")));
					Actions.Add(MakeShared<FJsonValueObject>(ActionRow));
				}

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("context"), Asset.GetObjectPathString());
				Row->SetBoolField(TEXT("applied"), bApplied);
				Row->SetArrayField(TEXT("actions"), Actions);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("contexts"), Rows);
			// total counts the contexts found under path_prefix, which is not
			// the number of rows here: applied_only filters, and max stops the
			// scan early. Both of those used to be invisible.
			Data->SetNumberField(TEXT("total"), Contexts.Num());
			Data->SetNumberField(TEXT("returned"), Rows.Num());
			Data->SetBoolField(TEXT("truncated"), bTruncated);
			Data->SetStringField(TEXT("scanned"), PathPrefix.IsEmpty() ? FString(TEXT("/")) : PathPrefix);
			Data->SetBoolField(TEXT("livePlayer"), Input != nullptr);

			FString Message;
			if (!Input)
			{
				Message = bPieRunning
					? TEXT("play is running but has no local player yet, so 'applied' is unknown - ask again once a player controller exists")
					: TEXT("no running player, so 'applied' is unknown - start play to see which contexts are in force");
			}
			else if (bAppliedOnly && Rows.Num() == 0)
			{
				Message = TEXT("no applied context was found under this path - the game's context may ship in a plugin, so try path_prefix '/'");
			}
			return FUplinkToolResult::Ok(Data, Message);
		});

	// ------------------------------------------------------------------
	// dialog_state - is a modal holding the whole editor hostage?
	// ------------------------------------------------------------------
	InRegistry.RegisterQuick(
		TEXT("dialog_state"),
		TEXT("Whether a modal window is up, what it says, and what else is open. Every tool runs on the game thread, and a modal that blocks it blocks this tool too: while one is up, calls do not fail or time out, they simply do not run. So the ordinary blocking kind is visible here only after it has gone, and 'blocked':false is not evidence that no modal was involved in a hang - ask this first once Uplink answers again and read 'windows', which names what is open. A 'blocked':true answer means a modal that lets the editor keep ticking, such as a slow-task window, and that is also the only kind 'dismiss' can reach. Read the title before dismissing: closing a prompt answers it."),
		TEXT(R"json({"type":"object","properties":{"dismiss":{"type":"boolean","default":false,"description":"Close the blocking modal. Read the title first - closing a prompt chooses its default answer."}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			if (!FSlateApplication::IsInitialized())
			{
				return FUplinkToolResult::Error(TEXT("Slate is not initialized"));
			}
			FSlateApplication& Slate = FSlateApplication::Get();

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			const TSharedPtr<SWindow> Modal = Slate.GetActiveModalWindow();
			Data->SetBoolField(TEXT("blocked"), Modal.IsValid());

			// Name every top-level window whichever answer this is: what else
			// is open is half of it, and the description sends the caller to
			// 'windows' precisely when no modal is left to name.
			TArray<TSharedPtr<FJsonValue>> Windows;
			for (const TSharedRef<SWindow>& Window : Slate.GetTopLevelWindows())
			{
				const FString Title = Window->GetTitle().ToString();
				if (!Title.IsEmpty())
				{
					Windows.Add(MakeShared<FJsonValueString>(Title));
				}
			}
			Data->SetArrayField(TEXT("windows"), Windows);

			if (!Modal.IsValid())
			{
				return FUplinkToolResult::Ok(Data, TEXT("nothing is blocking"));
			}

			const FString Title = Modal->GetTitle().ToString();
			Data->SetStringField(TEXT("title"), Title);

			bool bDismiss = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("dismiss")), bDismiss);
			if (bDismiss)
			{
				Modal->RequestDestroyWindow();
				Data->SetBoolField(TEXT("dismissed"), true);
				return FUplinkToolResult::Ok(Data, FString::Printf(
					TEXT("closed '%s' - if it was a question, it just took its default answer"), *Title));
			}

			// Not "blocking every tool" - this call answered, so it is one of
			// the modals that lets the editor keep ticking. Saying otherwise
			// told the caller the session was stuck when it plainly was not.
			return FUplinkToolResult::Ok(Data, FString::Printf(
				TEXT("'%s' is modal and up now, though it still lets tools run; pass dismiss:true to close it once you are content with the answer that gives it"), *Title));
		});
}
