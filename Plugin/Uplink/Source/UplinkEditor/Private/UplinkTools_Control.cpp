// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// PIE control tools: input_action, input_key, possess, player_teleport, player_info.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "EnhancedInputSubsystems.h"
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

using namespace UplinkToolUtil;

namespace
{
	struct FPiePlayer
	{
		APlayerController* PC = nullptr;
		UEnhancedInputLocalPlayerSubsystem* Input = nullptr;
	};

	FPiePlayer GetPiePlayer(FString& OutError, bool bNeedEnhancedInput)
	{
		FPiePlayer Result;
		UWorld* PieWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
		if (!PieWorld)
		{
			OutError = TEXT("no PIE session is running (input/control tools drive the live game — call pie_start first)");
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
			Out = FUplinkToolResult::Ok(nullptr, TEXT("hold finished and released"));
			return EUplinkToolStep::Done;
		}

	private:
		TWeakObjectPtr<const UInputAction> ActionPtr;
		double EndAt = 0.0;
	};

	// Tap: key down on Start, key up shortly after.
	class FKeyTapInvocation final : public IUplinkInvocation
	{
	public:
		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			FString Error;
			FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/false);
			if (!Player.PC)
			{
				Out = FUplinkToolResult::Error(Error);
				return EUplinkToolStep::Done;
			}

			Key = FKey(FName(*GetString(Ctx.Params, TEXT("key"))));
			if (!EKeys::GetKeyDetails(Key).IsValid())
			{
				Out = FUplinkToolResult::Error(FString::Printf(TEXT("unknown key '%s' (use UE key names: W, SpaceBar, LeftMouseButton, Gamepad_FaceButton_Bottom, ...)"), *Key.ToString()));
				return EUplinkToolStep::Done;
			}

			const float Amount = static_cast<float>(GetNumber(Ctx.Params, TEXT("amount"), 1.0));
			FInputKeyEventArgs Press = FInputKeyEventArgs::CreateSimulated(Key, IE_Pressed, Amount);
			Press.DeltaTime = FApp::GetDeltaTime();
			Player.PC->InputKey(Press);

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

			FString Error;
			FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/false);
			if (Player.PC)
			{
				FInputKeyEventArgs Release = FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.0f);
				Release.DeltaTime = FApp::GetDeltaTime();
				Player.PC->InputKey(Release);
			}
			Out = FUplinkToolResult::Ok(nullptr, TEXT("tapped"));
			return EUplinkToolStep::Done;
		}

	private:
		FKey Key;
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
				return Finish(Pawn, false,
					TEXT("stalled - no path progress for 3s (is there a NavMeshBoundsVolume covering the area? spawn one and run console 'RebuildNavigation')"), Out);
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
						Player.Input->StopContinuousInputInjectionForAction(Action);
						Out = FUplinkToolResult::Ok(nullptr, TEXT("released"));
						return EUplinkToolStep::Done;
					}

					FInputActionValue Value;
					if (!ParseActionValue(Action, Ctx.Params->TryGetField(FStringView(TEXT("value"))), Value, Error))
					{
						Out = FUplinkToolResult::Error(Error);
						return EUplinkToolStep::Done;
					}

					if (Mode == TEXT("pulse"))
					{
						Player.Input->InjectInputForAction(Action, Value, {}, {});
						Out = FUplinkToolResult::Ok(nullptr, TEXT("pulsed for one input tick"));
					}
					else if (Mode == TEXT("hold"))
					{
						Player.Input->StartContinuousInputInjectionForAction(Action, Value, {}, {});
						Out = FUplinkToolResult::Ok(nullptr, TEXT("holding until mode 'release'"));
					}
					else if (Mode == TEXT("update"))
					{
						Player.Input->UpdateValueOfContinuousInputInjectionForAction(Action, Value);
						Out = FUplinkToolResult::Ok(nullptr, TEXT("held value updated"));
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
		Info.Description = TEXT("Simulate a raw key on the running game's player controller (the engine's own simulated-input path - no OS focus needed). event 'tap' presses then releases after 'duration' seconds; 'pressed'/'released' send one edge; 'axis' sends an analog value (use 'amount', e.g. Gamepad_LeftX).");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"key":{"type":"string","description":"UE key name: W, SpaceBar, LeftMouseButton, Gamepad_FaceButton_Bottom, Gamepad_LeftX, ..."},"event":{"type":"string","enum":["tap","pressed","released","axis"],"default":"tap"},"amount":{"type":"number","default":1.0},"duration":{"type":"number","description":"tap only: seconds between press and release (default 0.12)"}},"required":["key"]})json"));
		Info.bReadOnly = false;
		Info.TimeoutSeconds = 30.0;
		Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
		{
			class FDispatch final : public IUplinkInvocation
			{
			public:
				virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
				{
					const FString Event = GetString(Ctx.Params, TEXT("event"), TEXT("tap"));
					if (Event == TEXT("tap"))
					{
						Inner = MakeShared<FKeyTapInvocation>();
						return Inner->Start(Ctx, Out);
					}

					FString Error;
					FPiePlayer Player = GetPiePlayer(Error, /*bNeedEnhancedInput=*/false);
					if (!Player.PC)
					{
						Out = FUplinkToolResult::Error(Error);
						return EUplinkToolStep::Done;
					}
					const FKey Key(FName(*GetString(Ctx.Params, TEXT("key"))));
					if (!EKeys::GetKeyDetails(Key).IsValid())
					{
						Out = FUplinkToolResult::Error(FString::Printf(TEXT("unknown key '%s'"), *Key.ToString()));
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

			APawn* Target = Cast<APawn>(FindActor(GEditor->PlayWorld, GetString(Ctx.Params, TEXT("pawn"))));
			if (!Target)
			{
				return FUplinkToolResult::Error(TEXT("pawn not found in the PIE world (must be an APawn)"));
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
			FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr, MouseDown);

			const FPointerEvent MouseUp(
				0, ClickPos, ClickPos, TSet<FKey>(),
				EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
			FSlateApplication::Get().ProcessMouseButtonUpEvent(MouseUp);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			if (!ClickedName.IsEmpty())
			{
				Data->SetStringField(TEXT("widget"), ClickedName);
			}
			TSharedRef<FJsonObject> PosJson = MakeShared<FJsonObject>();
			PosJson->SetNumberField(TEXT("x"), ClickPos.X);
			PosJson->SetNumberField(TEXT("y"), ClickPos.Y);
			Data->SetObjectField(TEXT("position"), PosJson);
			return FUplinkToolResult::Ok(Data, TEXT("clicked"));
		});

	{
		FUplinkToolInfo Info;
		Info.Name = TEXT("navigate_to");
		Info.Description = TEXT("Walk the player pawn to a location or actor using the game's navmesh - like a click-to-move player, no manual input math. Resolves when within 'accept_radius' of the goal; reports a stall instead of hanging if there is no path (most often: the level has no NavMeshBoundsVolume - spawn one and run console 'RebuildNavigation'). PIE only.");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"location":{"type":"object"},"actor":{"type":"string"},"accept_radius":{"type":"number","default":100}}})json"));
		Info.bReadOnly = false;
		Info.TimeoutSeconds = 120.0;
		Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
		{
			return MakeShared<FNavigateInvocation>();
		});
	}
}
