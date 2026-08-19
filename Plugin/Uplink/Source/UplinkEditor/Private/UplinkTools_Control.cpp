// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// PIE control tools: input_action, input_key, possess, player_teleport, player_info.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputKeyEventArgs.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/App.h"
#include "NavigationSystem.h"

using namespace UplinkToolUtil;

namespace
{
	struct FPiePlayer
	{
		APlayerController* PC = nullptr;
		UEnhancedInputLocalPlayerSubsystem* Input = nullptr;
	};

	/**
	 * How many places actually listen for this action. Injection always
	 * "succeeds" even when nothing is bound, which looks identical to a game
	 * that ignored the input - so report the count and let the caller tell the
	 * difference.
	 */
	int32 CountActionBindings(APlayerController* PC, const UInputAction* Action)
	{
		int32 Count = 0;
		auto Scan = [&Count, Action](UInputComponent* Component)
		{
			if (const UEnhancedInputComponent* Enhanced = Cast<UEnhancedInputComponent>(Component))
			{
				for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Enhanced->GetActionEventBindings())
				{
					if (Binding.IsValid() && Binding->GetAction() == Action)
					{
						++Count;
					}
				}
			}
		};
		if (PC)
		{
			Scan(PC->InputComponent);
			if (const APawn* Pawn = PC->GetPawn())
			{
				Scan(Pawn->InputComponent);
			}
		}
		return Count;
	}

	/**
	 * Send a key the way a real keyboard does: as a Slate event to whatever
	 * currently holds focus.
	 *
	 * The player-controller path never reaches a UMG widget that overrides
	 * OnKeyDown, which is how menus are usually built - a real game's title
	 * screen sat there ignoring every key while the tool reported success,
	 * because the widget was listening to Slate and the key was going to the
	 * pawn. Returns whether anything accepted it.
	 */
	bool SendSlateKey(const FKey& Key, bool bDown, bool bWithCharacter)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}
		FSlateApplication& Slate = FSlateApplication::Get();

		// Focus the game viewport first: a key event goes to whatever has
		// keyboard focus, and after a tool-driven session that is often the
		// editor UI rather than the game.
		if (Slate.GetGameViewport().IsValid())
		{
			Slate.SetAllUserFocusToGameViewport(EFocusCause::SetDirectly);
		}

		// Note the order: this fills KeyCode first, then CharCode.
		const uint32* KeyCodePtr = nullptr;
		const uint32* CharCodePtr = nullptr;
		FInputKeyManager::Get().GetCodesFromKey(Key, KeyCodePtr, CharCodePtr);
		const uint32 KeyCode = KeyCodePtr ? *KeyCodePtr : 0;
		const uint32 CharCode = CharCodePtr ? *CharCodePtr : 0;

		const FKeyEvent Event(Key, FModifierKeysState(), /*UserIndex=*/0,
			/*bIsRepeat=*/false, CharCode, KeyCode);

		bool bHandled = false;
		if (bDown)
		{
			bHandled = Slate.ProcessKeyDownEvent(Event);
			// A printable key also produces a character event, which is what
			// text boxes and "press any key" prompts often listen for.
			if (bWithCharacter && CharCode != 0)
			{
				const FCharacterEvent CharEvent(static_cast<TCHAR>(CharCode), FModifierKeysState(), 0, false);
				bHandled |= Slate.ProcessKeyCharEvent(CharEvent);
			}
		}
		else
		{
			bHandled = Slate.ProcessKeyUpEvent(Event);
		}
		return bHandled;
	}

	FPiePlayer GetPiePlayer(FString& OutError, bool bNeedEnhancedInput)
	{
		FPiePlayer Result;
		UWorld* PieWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
		if (!PieWorld)
		{
			OutError = TEXT("no PIE session is running (input/control tools drive the live game - call pie_start first)");
			return Result;
		}
		Result.PC = PieWorld->GetFirstPlayerController();
		if (!Result.PC)
		{
			OutError = TEXT("PIE world has no player controller");
			return Result;
		}
		if (bNeedEnhancedInput)
		{
			if (ULocalPlayer* LocalPlayer = Result.PC->GetLocalPlayer())
			{
				Result.Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer);
			}
			if (!Result.Input)
			{
				OutError = TEXT("Enhanced Input local-player subsystem unavailable");
				Result.PC = nullptr;
			}
		}
		return Result;
	}

	bool ParseActionValue(const UInputAction* Action, const TSharedPtr<FJsonValue>& Raw,
		FInputActionValue& Out, FString& OutError)
	{
		const EInputActionValueType ValueType = Action->ValueType;
		FVector Vector = FVector::ZeroVector;

		bool bBool = false;
		double Number = 0.0;
		const TSharedPtr<FJsonObject>* Object = nullptr;

		if (Raw.IsValid() && Raw->TryGetBool(bBool))
		{
			Vector.X = bBool ? 1.0 : 0.0;
		}
		else if (Raw.IsValid() && Raw->TryGetNumber(Number))
		{
			Vector.X = Number;
		}
		else if (Raw.IsValid() && Raw->TryGetObject(Object) && Object->IsValid())
		{
			(*Object)->TryGetNumberField(FStringView(TEXT("x")), Vector.X);
			(*Object)->TryGetNumberField(FStringView(TEXT("y")), Vector.Y);
			(*Object)->TryGetNumberField(FStringView(TEXT("z")), Vector.Z);
		}
		else
		{
			OutError = TEXT("'value' must be a bool, a number, or an object {x,y,z}");
			return false;
		}

		// Typed constructor zeroes components the action's value type can't carry.
		Out = FInputActionValue(ValueType, Vector);
		return true;
	}

	const UInputAction* LoadAction(const FUplinkToolContext& Ctx, FString& OutError)
	{
		const FString ActionPath = GetString(Ctx.Params, TEXT("action"));
		if (ActionPath.IsEmpty())
		{
			OutError = TEXT("'action' (UInputAction asset path) is required");
			return nullptr;
		}
		const UInputAction* Action = Cast<UInputAction>(
			StaticLoadObject(UInputAction::StaticClass(), nullptr, *ActionPath));
		if (!Action)
		{
			OutError = FString::Printf(TEXT("input action not found: %s (use asset_search with class_contains=InputAction)"), *ActionPath);
		}
		return Action;
	}

	// Timed hold: starts continuous injection, stops it after 'duration' seconds.
	class FTimedActionHoldInvocation final : public IUplinkInvocation
	{
	public:
		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			FString Error;
			const UInputAction* Action = LoadAction(Ctx, Error);
			if (!Action)
			{
				Out = FUplinkToolResult::Error(Error);
				return EUplinkToolStep::Done;
			}
			FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/true);
			if (!Player.PC)
			{
				Out = FUplinkToolResult::Error(Error);
				return EUplinkToolStep::Done;
			}
			FInputActionValue Value;
			if (!ParseActionValue(Action, Ctx.Params->TryGetField(FStringView(TEXT("value"))), Value, Error))
			{
				Out = FUplinkToolResult::Error(Error);
				return EUplinkToolStep::Done;
			}

			ActionPtr = Action;
			// Counted at the moment of injection, not at the end. A hold that
			// runs across a possession change would otherwise report the new
			// pawn's bindings and say nothing about the one that was actually
			// being driven.
			BoundAtStart = CountActionBindings(Player.PC, Action);
			Player.Input->StartContinuousInputInjectionForAction(Action, Value, {}, {});
			EndAt = FPlatformTime::Seconds() + FMath::Clamp(GetNumber(Ctx.Params, TEXT("duration"), 1.0), 0.05, 60.0);
			return EUplinkToolStep::Pending;
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			if (FPlatformTime::Seconds() < EndAt)
			{
				FString Error;
				if (!GEditor || !GEditor->PlayWorld)
				{
					Out = FUplinkToolResult::Error(TEXT("PIE ended during the input hold"));
					return EUplinkToolStep::Done;
				}
				return EUplinkToolStep::Pending;
			}

			FString Error;
			FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/true);
			if (Player.Input && ActionPtr.IsValid())
			{
				Player.Input->StopContinuousInputInjectionForAction(ActionPtr.Get());
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("boundHandlers"), BoundAtStart);
			Out = FUplinkToolResult::Ok(Data, BoundAtStart == 0
				? TEXT("hold finished and released - but nothing was bound to this action, so it had no effect")
				: TEXT("hold finished and released"));
			return EUplinkToolStep::Done;
		}

	private:
		int32 BoundAtStart = 0;
		TWeakObjectPtr<const UInputAction> ActionPtr;
		double EndAt = 0.0;
	};

	// Tap: key down on Start, key up shortly after.
	class FKeyTapInvocation final : public IUplinkInvocation
	{
	public:
		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			Key = FKey(FName(*GetString(Ctx.Params, TEXT("key"))));
			if (!EKeys::GetKeyDetails(Key).IsValid())
			{
				Out = FUplinkToolResult::Error(FString::Printf(TEXT("unknown key '%s' (use UE key names: W, SpaceBar, LeftMouseButton, Gamepad_FaceButton_Bottom, ...)"), *Key.ToString()));
				return EUplinkToolStep::Done;
			}
			Route = GetString(Ctx.Params, TEXT("route"), TEXT("game"));

			// A tap at the UI needs no player controller - a menu often runs
			// on a controller with no pawn.
			if (Route == TEXT("ui"))
			{
				bUiHandled = SendSlateKey(Key, /*bDown=*/true, /*bWithCharacter=*/true);
				ReleaseAt = FPlatformTime::Seconds() + FMath::Clamp(GetNumber(Ctx.Params, TEXT("duration"), 0.12), 0.03, 10.0);
				return EUplinkToolStep::Pending;
			}

			FString Error;
			FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/false);
			if (!Player.PC)
			{
				Out = FUplinkToolResult::Error(Error);
				return EUplinkToolStep::Done;
			}

			const float Amount = static_cast<float>(GetNumber(Ctx.Params, TEXT("amount"), 1.0));
			FInputKeyEventArgs Press = FInputKeyEventArgs::CreateSimulated(Key, IE_Pressed, Amount);
			Press.DeltaTime = FApp::GetDeltaTime();
			Player.PC->InputKey(Press);
			if (Route == TEXT("both"))
			{
				bUiHandled = SendSlateKey(Key, /*bDown=*/true, /*bWithCharacter=*/true);
			}

			ReleaseAt = FPlatformTime::Seconds() + FMath::Clamp(GetNumber(Ctx.Params, TEXT("duration"), 0.12), 0.03, 10.0);
			return EUplinkToolStep::Pending;
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			if (FPlatformTime::Seconds() < ReleaseAt)
			{
				if (!GEditor || !GEditor->PlayWorld)
				{
					Out = FUplinkToolResult::Error(TEXT("PIE ended during the key tap"));
					return EUplinkToolStep::Done;
				}
				return EUplinkToolStep::Pending;
			}

			if (Route != TEXT("ui"))
			{
				FString Error;
				FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/false);
				if (Player.PC)
				{
					FInputKeyEventArgs Release = FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.0f);
					Release.DeltaTime = FApp::GetDeltaTime();
					Player.PC->InputKey(Release);
				}
			}
			if (Route == TEXT("ui") || Route == TEXT("both"))
			{
				SendSlateKey(Key, /*bDown=*/false, /*bWithCharacter=*/false);
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("route"), Route);
			if (Route != TEXT("game"))
			{
				Data->SetBoolField(TEXT("uiHandled"), bUiHandled);
			}
			Out = FUplinkToolResult::Ok(Data, (Route != TEXT("game") && !bUiHandled)
				? TEXT("tapped, but no widget handled it - is anything focused, and does it override OnKeyDown?")
				: TEXT("tapped"));
			return EUplinkToolStep::Done;
		}

	private:
		FKey Key;
		FString Route = TEXT("game");
		bool bUiHandled = false;
		double ReleaseAt = 0.0;
	};

	/**
	 * navigate_to: walk the player pawn to a goal using the game's own navmesh,
	 * like a click-to-move player. Resolves when the pawn arrives, and reports
	 * a stall (usually: no NavMeshBoundsVolume in the level) instead of hanging.
	 */
	class FNavigateInvocation final : public IUplinkInvocation
	{
	public:
		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			FString Error;
			FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/false);
			if (!Player.PC || !Player.PC->GetPawn())
			{
				Out = FUplinkToolResult::Error(Error.IsEmpty() ? TEXT("player has no pawn") : Error);
				return EUplinkToolStep::Done;
			}

			const FString TargetActorName = GetString(Ctx.Params, TEXT("actor"));
			if (!TargetActorName.IsEmpty())
			{
				AActor* Target = FindActor(GEditor->PlayWorld.Get(), TargetActorName);
				if (!Target)
				{
					Out = FUplinkToolResult::Error(FString::Printf(TEXT("actor not found: %s"), *TargetActorName));
					return EUplinkToolStep::Done;
				}
				Goal = Target->GetActorLocation();
			}
			else if (!TryGetVector(Ctx.Params, TEXT("location"), Goal))
			{
				Out = FUplinkToolResult::Error(TEXT("provide 'location' {x,y,z} or 'actor'"));
				return EUplinkToolStep::Done;
			}

			AcceptRadius = FMath::Clamp(GetNumber(Ctx.Params, TEXT("accept_radius"), 100.0), 10.0, 2000.0);
			UAIBlueprintHelperLibrary::SimpleMoveToLocation(Player.PC, Goal);
			LastPosition = Player.PC->GetPawn()->GetActorLocation();
			LastProgressAt = FPlatformTime::Seconds();
			return EUplinkToolStep::Pending;
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			FString Error;
			FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/false);
			APawn* Pawn = Player.PC ? Player.PC->GetPawn() : nullptr;
			if (!Pawn)
			{
				Out = FUplinkToolResult::Error(TEXT("PIE or pawn went away mid-navigation"));
				return EUplinkToolStep::Done;
			}

			const FVector Position = Pawn->GetActorLocation();
			const double Distance = FVector::Dist2D(Position, Goal);
			if (Distance <= AcceptRadius)
			{
				return Finish(Pawn, true, TEXT("arrived"), Out);
			}

			if (FVector::Dist2D(Position, LastPosition) > 5.0)
			{
				LastPosition = Position;
				LastProgressAt = FPlatformTime::Seconds();
			}
			else if (FPlatformTime::Seconds() - LastProgressAt > 3.0)
			{
				// Standing still and pushing are different failures, and the
				// velocity separates them. A pawn with speed and no progress has
				// a path and is being held - a modal that pins the player, a
				// scripted sequence, a blocking volume - and sending someone to
				// audit their navmesh over that wastes the one clue they had.
				// Only the motionless case is worth blaming navigation for.
				const double Speed = Pawn->GetVelocity().Size2D();
				FString Reason;
				if (Speed > 1.0)
				{
					Reason = FString::Printf(
						TEXT("stalled - no path progress for 3s, but the pawn is moving at %.0f uu/s, so it has a path and something is holding it in place ")
						TEXT("(a modal that locks the player, a scripted sequence, or geometry it cannot pass). Navigation is not the problem here - check ui_live and observe."),
						Speed);
				}
				else
				{
					// Ask the navigation system instead of guessing at it. "Is
					// there a NavMeshBoundsVolume?" is the wrong question when
					// six of them exist and none has generated data where the
					// pawn stands - which is what a real project turned out to
					// be doing, with the volumes, the RecastNavMesh and the
					// PathFollowingComponent all present and no path produced.
					// Projecting the pawn's own position onto the navmesh
					// answers it in one call.
					const UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(Pawn->GetWorld());
					FNavLocation Projected;
					const bool bOnNavMesh = Nav && Nav->ProjectPointToNavigation(Position, Projected);
					Reason = Nav == nullptr
						? TEXT("stalled - the world has no navigation system, so nothing can path here")
						: bOnNavMesh
							? TEXT("stalled - the pawn is standing on the navmesh but never moved, so the path to the goal could not be built. ")
								TEXT("The goal is most likely off the navmesh, or on a disconnected island from where the pawn stands.")
							: TEXT("stalled - the pawn is NOT standing on generated navmesh, so no path could start. ")
								TEXT("A NavMeshBoundsVolume existing is not enough; it has to have built data here. Check its extent and run console 'RebuildNavigation'.");
				}
				return Finish(Pawn, false, *Reason, Out);
			}
			return EUplinkToolStep::Pending;
		}

	private:
		EUplinkToolStep Finish(APawn* Pawn, bool bReached, const TCHAR* Message, FUplinkToolResult& Out)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("reached"), bReached);
			Data->SetObjectField(TEXT("location"), VectorToJson(Pawn->GetActorLocation()));
			Data->SetNumberField(TEXT("distance_to_goal"), FVector::Dist2D(Pawn->GetActorLocation(), Goal));
			Out = FUplinkToolResult::Ok(Data, Message);
			Out.bError = !bReached;
			return EUplinkToolStep::Done;
		}

		FVector Goal = FVector::ZeroVector;
		FVector LastPosition = FVector::ZeroVector;
		double AcceptRadius = 100.0;
		double LastProgressAt = 0.0;
	};
}

void UplinkTools::RegisterControl(FUplinkToolRegistry& Registry)
{
	{
		FUplinkToolInfo Info;
		Info.Name = TEXT("input_action");
		Info.Description = TEXT("Inject Enhanced Input into the running game at the action level (bypasses keyboard/device mapping - works with no physical input at all). mode 'pulse' fires the action for one input tick; 'hold' starts sustained injection (auto-released after 'duration' seconds if given, else until mode 'release'); 'update' changes the held value; 'release' stops it. 'action' is the UInputAction asset path.");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"action":{"type":"string","description":"UInputAction asset path, e.g. /Game/Input/IA_Jump.IA_Jump"},"value":{"description":"bool, number, or {x,y,z} matching the action's value type"},"mode":{"type":"string","enum":["pulse","hold","update","release"],"default":"pulse"},"duration":{"type":"number","description":"hold mode only: seconds until auto-release (0.05-60)"}},"required":["action"]})json"));
		Info.bReadOnly = false;
		Info.bTransactional = false; // drives the session, not an undoable edit
		Info.TimeoutSeconds = 90.0;
		Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
		{
			// The timed-hold path is latent; everything else completes in Start.
			class FDispatch final : public IUplinkInvocation
			{
			public:
				virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
				{
					const FString Mode = GetString(Ctx.Params, TEXT("mode"), TEXT("pulse"));
					const bool bTimedHold = Mode == TEXT("hold") && Ctx.Params->HasField(FStringView(TEXT("duration")));
					if (bTimedHold)
					{
						Inner = MakeShared<FTimedActionHoldInvocation>();
						return Inner->Start(Ctx, Out);
					}

					FString Error;
					const UInputAction* Action = LoadAction(Ctx, Error);
					if (!Action)
					{
						Out = FUplinkToolResult::Error(Error);
						return EUplinkToolStep::Done;
					}
					FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/true);
					if (!Player.PC)
					{
						Out = FUplinkToolResult::Error(Error);
						return EUplinkToolStep::Done;
					}

					if (Mode == TEXT("release"))
					{
						// Reported here too, so every mode answers the same
						// question. A release is the one call where a zero is
						// unremarkable rather than a warning - the hold that
						// preceded it is what should have complained.
						TSharedRef<FJsonObject> ReleaseData = MakeShared<FJsonObject>();
						ReleaseData->SetNumberField(TEXT("boundHandlers"), CountActionBindings(Player.PC, Action));
						Player.Input->StopContinuousInputInjectionForAction(Action);
						Out = FUplinkToolResult::Ok(ReleaseData, TEXT("released"));
						return EUplinkToolStep::Done;
					}

					FInputActionValue Value;
					if (!ParseActionValue(Action, Ctx.Params->TryGetField(FStringView(TEXT("value"))), Value, Error))
					{
						Out = FUplinkToolResult::Error(Error);
						return EUplinkToolStep::Done;
					}

					const int32 BoundCount = CountActionBindings(Player.PC, Action);
					TSharedRef<FJsonObject> InjectData = MakeShared<FJsonObject>();
					InjectData->SetNumberField(TEXT("boundHandlers"), BoundCount);
					const FString Unbound = BoundCount == 0
						? TEXT(" - but nothing is bound to this action right now, so it will have no effect (is the owning mapping context added, and does the possessed pawn bind it?)")
						: FString();

					if (Mode == TEXT("pulse"))
					{
						Player.Input->InjectInputForAction(Action, Value, {}, {});
						Out = FUplinkToolResult::Ok(InjectData,
							FString::Printf(TEXT("pulsed for one input tick%s"), *Unbound));
					}
					else if (Mode == TEXT("hold"))
					{
						Player.Input->StartContinuousInputInjectionForAction(Action, Value, {}, {});
						Out = FUplinkToolResult::Ok(InjectData,
							FString::Printf(TEXT("holding until mode 'release'%s"), *Unbound));
					}
					else if (Mode == TEXT("update"))
					{
						Player.Input->UpdateValueOfContinuousInputInjectionForAction(Action, Value);
						Out = FUplinkToolResult::Ok(InjectData,
							FString::Printf(TEXT("held value updated%s"), *Unbound));
					}
					else
					{
						Out = FUplinkToolResult::Error(TEXT("mode must be pulse, hold, update, or release"));
					}
					return EUplinkToolStep::Done;
				}

				virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
				{
					return Inner->Tick(Ctx, Out);
				}

			private:
				TSharedPtr<IUplinkInvocation> Inner;
			};
			return MakeShared<FDispatch>();
		});
	}

	{
		FUplinkToolInfo Info;
		Info.Name = TEXT("input_key");
		Info.Description = TEXT("Simulate a raw key in the running game (the engine's own simulated-input path - no OS focus needed). event 'tap' presses then releases after 'duration' seconds; 'pressed'/'released' send one edge; 'axis' sends an analog value (use 'amount', e.g. Gamepad_LeftX). 'route' decides WHO receives it: 'game' (default) reaches the player controller's own input stack; 'ui' sends a real Slate key event to the focused widget, which is the only thing a UMG menu overriding OnKeyDown will ever see - if a title screen or menu ignores your keys, this is why; 'both' does each, like a real keypress. The reply reports whether anything handled it. IMPORTANT: on a project driven by Enhanced Input - which is most UE5 games - a raw key does NOT fire the Input Actions bound to it. Measured on a real game: W held for two seconds through this tool moved the character not at all and still answered handled:true, while the same two seconds of its IA_Move through input_action moved it 350 units. Use input_action for anything gameplay binds to, and keep this for menus and for games still on the legacy input stack.");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"key":{"type":"string","description":"UE key name: W, SpaceBar, LeftMouseButton, Gamepad_FaceButton_Bottom, Gamepad_LeftX, ..."},"event":{"type":"string","enum":["tap","pressed","released","axis"],"default":"tap"},"route":{"type":"string","enum":["game","ui","both"],"default":"game","description":"Who receives the key: the player controller, the focused UMG widget, or both"},"amount":{"type":"number","default":1.0},"duration":{"type":"number","description":"tap only: seconds between press and release (default 0.12)"}},"required":["key"]})json"));
		Info.bReadOnly = false;
		Info.bTransactional = false; // drives the session, not an undoable edit
		Info.TimeoutSeconds = 30.0;
		Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
		{
			class FDispatch final : public IUplinkInvocation
			{
			public:
				virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
				{
					const FString Event = GetString(Ctx.Params, TEXT("event"), TEXT("tap"));
					const FString Route = GetString(Ctx.Params, TEXT("route"), TEXT("game"));
					if (Route != TEXT("game") && Route != TEXT("ui") && Route != TEXT("both"))
					{
						Out = FUplinkToolResult::Error(TEXT("unknown route (game, ui, both)"));
						return EUplinkToolStep::Done;
					}

					if (Event == TEXT("tap"))
					{
						Inner = MakeShared<FKeyTapInvocation>();
						return Inner->Start(Ctx, Out);
					}

					const FKey Key(FName(*GetString(Ctx.Params, TEXT("key"))));
					if (!EKeys::GetKeyDetails(Key).IsValid())
					{
						Out = FUplinkToolResult::Error(FString::Printf(TEXT("unknown key '%s'"), *Key.ToString()));
						return EUplinkToolStep::Done;
					}

					// The UI route needs no player controller: menus commonly
					// run on a controller with no pawn, and sometimes before
					// any gameplay controller exists at all.
					if (Route == TEXT("ui"))
					{
						const bool bUiHandled = SendSlateKey(Key, Event != TEXT("released"), /*bWithCharacter=*/true);
						TSharedRef<FJsonObject> UiData = MakeShared<FJsonObject>();
						// Same field names as the tap path, so a caller reads one
						// shape regardless of which event it sent.
						UiData->SetBoolField(TEXT("handled"), bUiHandled);
						UiData->SetBoolField(TEXT("uiHandled"), bUiHandled);
						UiData->SetStringField(TEXT("route"), TEXT("ui"));
						Out = FUplinkToolResult::Ok(UiData, bUiHandled
							? TEXT("sent to the focused widget")
							: TEXT("sent, but no widget handled it - is anything focused, and does it override OnKeyDown?"));
						return EUplinkToolStep::Done;
					}

					FString Error;
					FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/false);
					if (!Player.PC)
					{
						Out = FUplinkToolResult::Error(Error);
						return EUplinkToolStep::Done;
					}
					const float Amount = static_cast<float>(GetNumber(Ctx.Params, TEXT("amount"), 1.0));

					EInputEvent InputEvent;
					float SendAmount = Amount;
					if (Event == TEXT("pressed")) { InputEvent = IE_Pressed; }
					else if (Event == TEXT("released")) { InputEvent = IE_Released; SendAmount = 0.0f; }
					else if (Event == TEXT("axis")) { InputEvent = IE_Axis; }
					else
					{
						Out = FUplinkToolResult::Error(TEXT("event must be tap, pressed, released, or axis"));
						return EUplinkToolStep::Done;
					}

					FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(Key, InputEvent, SendAmount,
						/*InNumSamplesOverride=*/InputEvent == IE_Axis ? 1 : -1);
					Args.DeltaTime = FApp::GetDeltaTime();
					const bool bHandled = Player.PC->InputKey(Args);

					TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("handled"), bHandled);
					Data->SetStringField(TEXT("route"), *Route);

					// handled says the controller consumed the key, not that
					// anything acted on it - and on an Enhanced Input project a
					// raw key consumed here still fires no Input Action, so a
					// true there reads as success for a press that did nothing.
					// The player input's own class is the machine-readable half
					// of that warning; the applied-context count would be better
					// still, but the engine keeps that accessor protected.
					if (Cast<UEnhancedPlayerInput>(Player.PC->PlayerInput) != nullptr)
					{
						Data->SetBoolField(TEXT("enhancedInput"), true);
					}

					if (Route == TEXT("both"))
					{
						// A real key reaches the UI as well as the game.
						Data->SetBoolField(TEXT("uiHandled"),
							SendSlateKey(Key, Event != TEXT("released"), /*bWithCharacter=*/true));
					}
					Out = FUplinkToolResult::Ok(Data);
					return EUplinkToolStep::Done;
				}

				virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
				{
					return Inner->Tick(Ctx, Out);
				}

			private:
				TSharedPtr<IUplinkInvocation> Inner;
			};
			return MakeShared<FDispatch>();
		});
	}

	Registry.RegisterQuick(
		TEXT("possess"),
		TEXT("Make the player controller possess a different pawn in the running game."),
		TEXT(R"json({"type":"object","properties":{"pawn":{"type":"string","description":"Pawn actor name or label in the PIE world"}},"required":["pawn"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/false);
			if (!Player.PC)
			{
				return FUplinkToolResult::Error(Error);
			}

			const FString Wanted = GetString(Ctx.Params, TEXT("pawn"));
			AActor* Found = FindActor(GEditor->PlayWorld, Wanted);
			APawn* Target = Cast<APawn>(Found);
			if (!Target)
			{
				// Name what is actually there. "not found" on its own gives the
				// caller nowhere to go, and the usual causes - a non-pawn actor,
				// or a runtime name that differs from the label - are both
				// obvious the moment the candidates are listed.
				TArray<FString> Pawns;
				for (TActorIterator<APawn> It(GEditor->PlayWorld); It && Pawns.Num() < 25; ++It)
				{
					Pawns.Add(It->GetName());
				}
				if (Found)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' is a %s, not a pawn. Pawns in the running game: %s"),
						*Wanted, *Found->GetClass()->GetName(),
						Pawns.Num() ? *FString::Join(Pawns, TEXT(", ")) : TEXT("(none)")));
				}
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("no pawn named '%s' in the running game. Pawns here: %s"),
					*Wanted, Pawns.Num() ? *FString::Join(Pawns, TEXT(", ")) : TEXT("(none)")));
			}

			const FString Previous = Player.PC->GetPawn() ? Player.PC->GetPawn()->GetName() : TEXT("");
			Player.PC->Possess(Target);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("previous_pawn"), Previous);
			Data->SetStringField(TEXT("pawn"), Target->GetName());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("player_teleport"),
		TEXT("Teleport the player pawn in the running game (physics-safe), optionally setting the control rotation (where the player faces)."),
		TEXT(R"json({"type":"object","properties":{"location":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"rotation":{"type":"object","properties":{"pitch":{"type":"number"},"yaw":{"type":"number"},"roll":{"type":"number"}}}},"required":["location"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/false);
			if (!Player.PC)
			{
				return FUplinkToolResult::Error(Error);
			}
			APawn* Pawn = Player.PC->GetPawn();
			if (!Pawn)
			{
				return FUplinkToolResult::Error(TEXT("player controller has no pawn (use possess first)"));
			}

			FVector Location;
			if (!TryGetVector(Ctx.Params, TEXT("location"), Location))
			{
				return FUplinkToolResult::Error(TEXT("'location' is required"));
			}
			Pawn->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);

			FRotator Rotation;
			if (TryGetRotator(Ctx.Params, TEXT("rotation"), Rotation))
			{
				Player.PC->SetControlRotation(Rotation);
				Pawn->SetActorRotation(FRotator(0.0, Rotation.Yaw, 0.0), ETeleportType::TeleportPhysics);
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("pawn"), Pawn->GetName());
			Data->SetObjectField(TEXT("location"), VectorToJson(Pawn->GetActorLocation()));
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("player_info"),
		TEXT("Who and where the player is in the running game: controller, pawn (name/class/location), and control rotation."),
		TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/false);
			if (!Player.PC)
			{
				return FUplinkToolResult::Error(Error);
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("controller"), Player.PC->GetName());
			if (APawn* Pawn = Player.PC->GetPawn())
			{
				Data->SetStringField(TEXT("pawn"), Pawn->GetName());
				Data->SetStringField(TEXT("pawn_class"), Pawn->GetClass()->GetPathName());
				Data->SetObjectField(TEXT("location"), VectorToJson(Pawn->GetActorLocation()));
			}
			const FRotator ControlRotation = Player.PC->GetControlRotation();
			TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>();
			RotationJson->SetNumberField(TEXT("pitch"), ControlRotation.Pitch);
			RotationJson->SetNumberField(TEXT("yaw"), ControlRotation.Yaw);
			RotationJson->SetNumberField(TEXT("roll"), ControlRotation.Roll);
			Data->SetObjectField(TEXT("control_rotation"), RotationJson);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("click_widget"),
		TEXT("Click a UMG widget in the running game - menus, buttons, HUD elements - by synthesizing a real mouse press at the widget's screen position (so it goes through actual hit-testing, like a player's click). 'widget' matches by name (exact first, then contains) across all live UserWidgets; or pass a raw screen 'position'. The widget must be on screen."),
		TEXT(R"json({"type":"object","properties":{"widget":{"type":"string","description":"Widget name, e.g. 'BtnStart'"},"position":{"type":"object","description":"{x,y} desktop pixels, instead of 'widget'"},"world":{"type":"string","enum":["editor","pie"]}}})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			if (!FSlateApplication::IsInitialized())
			{
				return FUplinkToolResult::Error(TEXT("Slate is not initialized"));
			}

			FVector2f ClickPos = FVector2f::ZeroVector;
			FString ClickedName;

			const FString WidgetName = GetString(Ctx.Params, TEXT("widget"));
			if (!WidgetName.IsEmpty())
			{
				FString Error;
				UWorld* World = Ctx.ResolveWorld(Error);
				if (!World)
				{
					return FUplinkToolResult::Error(Error);
				}

				TArray<UUserWidget*> Roots;
				UWidgetBlueprintLibrary::GetAllWidgetsOfClass(World, Roots, UUserWidget::StaticClass(), /*TopLevelOnly=*/false);

				UWidget* Exact = nullptr;
				UWidget* Contains = nullptr;
				TArray<FString> Available;
				for (UUserWidget* Root : Roots)
				{
					if (!Root || !Root->WidgetTree)
					{
						continue;
					}
					Root->WidgetTree->ForEachWidget([&](UWidget* Widget)
					{
						if (!Widget)
						{
							return;
						}
						const FString Name = Widget->GetName();
						if (Available.Num() < 60)
						{
							Available.Add(Name);
						}
						if (Name.Equals(WidgetName, ESearchCase::IgnoreCase) && !Exact)
						{
							Exact = Widget;
						}
						else if (Name.Contains(WidgetName, ESearchCase::IgnoreCase) && !Contains)
						{
							Contains = Widget;
						}
					});
				}
				UWidget* Target = Exact ? Exact : Contains;
				if (!Target)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no widget named '%s'. Live widgets: %s"), *WidgetName, *FString::Join(Available, TEXT(", "))));
				}

				const FGeometry Geometry = Target->GetCachedGeometry();
				const FVector2D Size = Geometry.GetAbsoluteSize();
				if (Size.X <= 0.0 || Size.Y <= 0.0)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("widget '%s' exists but is not on screen (zero geometry)"), *Target->GetName()));
				}
				ClickPos = FVector2f(Geometry.GetAbsolutePosition()) + FVector2f(Size) * 0.5f;
				ClickedName = Target->GetName();
			}
			else
			{
				FVector PosVector;
				if (!TryGetVector(Ctx.Params, TEXT("position"), PosVector))
				{
					const TSharedPtr<FJsonObject>* PosObject = nullptr;
					if (Ctx.Params->TryGetObjectField(FStringView(TEXT("position")), PosObject) && PosObject->IsValid())
					{
						PosVector.X = (*PosObject)->GetNumberField(FStringView(TEXT("x")));
						PosVector.Y = (*PosObject)->GetNumberField(FStringView(TEXT("y")));
					}
					else
					{
						return FUplinkToolResult::Error(TEXT("provide 'widget' or 'position' {x,y}"));
					}
				}
				ClickPos = FVector2f(PosVector.X, PosVector.Y);
			}

			const FPointerEvent MouseDown(
				0, ClickPos, ClickPos, TSet<FKey>{ EKeys::LeftMouseButton },
				EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
			const bool bDownHandled = FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr, MouseDown);

			const FPointerEvent MouseUp(
				0, ClickPos, ClickPos, TSet<FKey>(),
				EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
			const bool bUpHandled = FSlateApplication::Get().ProcessMouseButtonUpEvent(MouseUp);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			if (!ClickedName.IsEmpty())
			{
				Data->SetStringField(TEXT("widget"), ClickedName);
			}
			TSharedRef<FJsonObject> PosJson = MakeShared<FJsonObject>();
			PosJson->SetNumberField(TEXT("x"), ClickPos.X);
			PosJson->SetNumberField(TEXT("y"), ClickPos.Y);
			Data->SetObjectField(TEXT("position"), PosJson);

			// Synthesizing the event always "works"; whether anything accepted
			// it is the part worth knowing. An unhandled click usually means
			// something invisible is over the target, or it does not take input.
			const bool bHandled = bDownHandled || bUpHandled;
			Data->SetBoolField(TEXT("handled"), bHandled);
			return FUplinkToolResult::Ok(Data, bHandled
				? TEXT("clicked")
				: TEXT("click was sent but nothing handled it - the widget may be covered, hit-test invisible, or not accept input"));
		});

	{
		FUplinkToolInfo Info;
		Info.Name = TEXT("navigate_to");
		Info.Description = TEXT("Walk the player pawn to a location or actor using the game's navmesh - like a click-to-move player, no manual input math. Resolves when within 'accept_radius' of the goal; reports a stall instead of hanging, and says which kind: a pawn that is moving but not progressing has a path and is being held by something (a modal, a scripted sequence, geometry), while one that is not moving at all never got a path - usually a missing NavMeshBoundsVolume. PIE only.");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"location":{"type":"object"},"actor":{"type":"string"},"accept_radius":{"type":"number","default":100}}})json"));
		Info.bReadOnly = false;
		Info.bTransactional = false; // drives the session, not an undoable edit
		Info.TimeoutSeconds = 120.0;
		Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
		{
			return MakeShared<FNavigateInvocation>();
		});
	}
}
