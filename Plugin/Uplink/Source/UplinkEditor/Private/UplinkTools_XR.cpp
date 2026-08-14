// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// XR tools: xr_simulate - drive a VR pawn's head and hands with no headset
// attached, so VR interactions can be play-tested from a desk.
//
// This works because of a deliberate engine behaviour: UMotionControllerComponent
// only overwrites its own transform while it is actually tracked ("we want the
// controller to stay in place rather than pop to 0,0,0"), and UCameraComponent
// only applies an HMD pose when head tracking is allowed. With no headset
// neither condition holds, so a pose written here survives every tick.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "IHeadMountedDisplay.h"
#include "IXRTrackingSystem.h"
#include "Kismet/GameplayStatics.h"
#include "MotionControllerComponent.h"

using namespace UplinkToolUtil;

namespace
{
	/**
	 * What the engine itself reports about XR. Read through GEngine's
	 * interfaces rather than the XR function library, which lives in an
	 * optional plugin - this has to work in projects with XR disabled.
	 */
	struct FHeadsetState
	{
		bool bTrackingSystem = false;
		bool bHeadTracking = false;
		bool bStereo = false;
		bool bConnected = false;
		FString SystemName;
	};

	FHeadsetState ReadHeadset(UWorld* World)
	{
		FHeadsetState State;
		if (!GEngine)
		{
			return State;
		}
		if (GEngine->XRSystem.IsValid())
		{
			State.bTrackingSystem = true;
			State.SystemName = GEngine->XRSystem->GetSystemName().ToString();
			State.bHeadTracking = World
				? GEngine->XRSystem->IsHeadTrackingAllowedForWorld(*World)
				: GEngine->XRSystem->IsHeadTrackingAllowed();
			if (IHeadMountedDisplay* Display = GEngine->XRSystem->GetHMDDevice())
			{
				State.bConnected = Display->IsHMDConnected();
			}
		}
		State.bStereo = GEngine->StereoRenderingDevice.IsValid()
			&& GEngine->StereoRenderingDevice->IsStereoEnabled();
		return State;
	}

	/** The head, the hands, and the rig they hang off. */
	struct FXRRig
	{
		APawn* Pawn = nullptr;
		UCameraComponent* Camera = nullptr;
		UMotionControllerComponent* Left = nullptr;
		UMotionControllerComponent* Right = nullptr;
		TArray<UMotionControllerComponent*> All;

		bool HasHands() const { return Left || Right; }
	};

	/** True when this component is the left hand, by motion source or by name. */
	bool IsLeftHanded(const UMotionControllerComponent* Controller)
	{
		const FString Source = Controller->MotionSource.ToString();
		if (!Source.IsEmpty())
		{
			return Source.StartsWith(TEXT("Left"), ESearchCase::IgnoreCase);
		}
		return Controller->GetName().Contains(TEXT("Left"), ESearchCase::IgnoreCase);
	}

	FXRRig FindRig(UWorld* World, const FString& PawnName, FString& OutError)
	{
		FXRRig Rig;
		if (!PawnName.IsEmpty())
		{
			if (AActor* Found = FindActor(World, PawnName))
			{
				Rig.Pawn = Cast<APawn>(Found);
				if (!Rig.Pawn)
				{
					OutError = FString::Printf(TEXT("'%s' is a %s, not a pawn"),
						*PawnName, *Found->GetClass()->GetName());
					return Rig;
				}
			}
			else
			{
				OutError = FString::Printf(TEXT("no actor named '%s'"), *PawnName);
				return Rig;
			}
		}
		else
		{
			Rig.Pawn = UGameplayStatics::GetPlayerPawn(World, 0);
			if (!Rig.Pawn)
			{
				OutError = TEXT("no player pawn (start the game first - there is nothing to drive in an editor world)");
				return Rig;
			}
		}

		Rig.Camera = Rig.Pawn->FindComponentByClass<UCameraComponent>();
		Rig.Pawn->GetComponents<UMotionControllerComponent>(Rig.All);
		for (UMotionControllerComponent* Controller : Rig.All)
		{
			if (!Controller)
			{
				continue;
			}
			// Two controllers with the same motion source is an authoring
			// mistake, but taking the first of each keeps the tool usable.
			if (IsLeftHanded(Controller))
			{
				Rig.Left = Rig.Left ? Rig.Left : Controller;
			}
			else
			{
				Rig.Right = Rig.Right ? Rig.Right : Controller;
			}
		}
		return Rig;
	}

	/**
	 * Whether a pawn's hands look like they are actually in use.
	 *
	 * Many VR pawns ship a desktop fallback and, with no headset, take that
	 * branch instead - leaving the motion controllers switched off, hidden, or
	 * carrying nothing. Posing those moves nothing anybody reads, and the call
	 * would still report success. This judges the rig on universal component
	 * state only: no knowledge of any particular framework's conventions.
	 */
	struct FRigVerdict
	{
		bool bUsable = false;
		TArray<FString> Reasons;
	};

	FRigVerdict JudgeHands(const TArray<UMotionControllerComponent*>& Controllers)
	{
		FRigVerdict Verdict;
		if (Controllers.Num() == 0)
		{
			Verdict.Reasons.Add(TEXT("the pawn has no motion controller components"));
			return Verdict;
		}

		int32 Active = 0;
		int32 Visible = 0;
		int32 WithChildren = 0;
		for (const UMotionControllerComponent* Controller : Controllers)
		{
			if (!Controller)
			{
				continue;
			}
			if (Controller->IsActive())
			{
				++Active;
			}
			if (Controller->IsVisible())
			{
				++Visible;
			}
			if (Controller->GetNumChildrenComponents() > 0)
			{
				++WithChildren;
			}
		}

		if (Active == 0)
		{
			Verdict.Reasons.Add(TEXT("every motion controller is deactivated"));
		}
		if (WithChildren == 0)
		{
			// Hands that carry nothing - no mesh, no collision, no scanner -
			// can be posed, but nothing downstream would notice.
			Verdict.Reasons.Add(TEXT("no motion controller has any child component, so moving one affects nothing"));
		}
		if (Visible == 0 && WithChildren > 0)
		{
			// Worth saying, not disqualifying: collision can still overlap
			// while hidden, so this alone does not make the rig unusable.
			Verdict.Reasons.Add(TEXT("no motion controller is visible (harmless if the interaction is collision-based)"));
		}

		Verdict.bUsable = Active > 0 && WithChildren > 0;
		return Verdict;
	}

	TSharedRef<FJsonObject> TransformJson(const USceneComponent* Component)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Component)
		{
			return Json;
		}
		const FVector Location = Component->GetComponentLocation();
		const FRotator Rotation = Component->GetComponentRotation();
		TSharedRef<FJsonObject> Loc = MakeShared<FJsonObject>();
		Loc->SetNumberField(TEXT("x"), Location.X);
		Loc->SetNumberField(TEXT("y"), Location.Y);
		Loc->SetNumberField(TEXT("z"), Location.Z);
		TSharedRef<FJsonObject> Rot = MakeShared<FJsonObject>();
		Rot->SetNumberField(TEXT("pitch"), Rotation.Pitch);
		Rot->SetNumberField(TEXT("yaw"), Rotation.Yaw);
		Rot->SetNumberField(TEXT("roll"), Rotation.Roll);
		Json->SetStringField(TEXT("component"), Component->GetName());
		Json->SetObjectField(TEXT("worldLocation"), Loc);
		Json->SetObjectField(TEXT("worldRotation"), Rot);
		return Json;
	}

	/** Resolve which component a 'device' name refers to. */
	USceneComponent* DeviceComponent(const FXRRig& Rig, const FString& Device, FString& OutError)
	{
		if (Device == TEXT("hmd") || Device == TEXT("head") || Device == TEXT("camera"))
		{
			if (!Rig.Camera)
			{
				OutError = FString::Printf(TEXT("'%s' has no CameraComponent"), *Rig.Pawn->GetName());
			}
			return Rig.Camera;
		}
		if (Device == TEXT("left"))
		{
			if (!Rig.Left)
			{
				OutError = FString::Printf(TEXT("'%s' has no left MotionControllerComponent (found %d controller(s))"),
					*Rig.Pawn->GetName(), Rig.All.Num());
			}
			return Rig.Left;
		}
		if (Device == TEXT("right"))
		{
			if (!Rig.Right)
			{
				OutError = FString::Printf(TEXT("'%s' has no right MotionControllerComponent (found %d controller(s))"),
					*Rig.Pawn->GetName(), Rig.All.Num());
			}
			return Rig.Right;
		}
		OutError = FString::Printf(TEXT("unknown device '%s' (hmd, left, right)"), *Device);
		return nullptr;
	}
}

void UplinkTools::RegisterXR(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("xr_simulate"),
		TEXT("Drive a VR pawn's head and hands with NO headset attached, so VR interactions can be play-tested from a desk. Motion controllers keep whatever pose you give them while untracked, so a written pose sticks. Actions: 'status' reports the headset state, the rig, and whether the hands look usable (start here); 'pose' places a device by location/rotation, in world space or relative to the pawn; 'reach' puts a hand at an actor (with optional offset) and points it there, which is how you drive a grab without doing the maths; 'reset' returns hands to a neutral pose in front of the chest. 'mode' picks what you are testing: 'vr' refuses to pose hands that are switched off or carry nothing, so a pawn that fell back to desktop cannot silently pass; 'desktop' aims the camera instead and leaves the hands alone; 'auto' (default) uses the hands when they look usable. Buttons and triggers are separate - inject those with input_action, which is what a real controller's grip and trigger map to."),
		TEXT(R"json({"type":"object","properties":{"action":{"type":"string","enum":["status","pose","reach","reset"],"default":"status"},"mode":{"type":"string","enum":["auto","vr","desktop"],"default":"auto","description":"'vr' requires a usable hand rig and fails loudly otherwise; 'desktop' drives the camera and never touches the hands"},"device":{"type":"string","enum":["hmd","left","right"],"description":"pose/reach: which device"},"location":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"rotation":{"type":"object","properties":{"pitch":{"type":"number"},"yaw":{"type":"number"},"roll":{"type":"number"}}},"space":{"type":"string","enum":["world","local"],"default":"local","description":"pose: 'local' is relative to the pawn, 'world' is absolute"},"target":{"type":"string","description":"reach: actor to put the hand at (in desktop mode, the camera looks at it instead)"},"offset":{"type":"object","description":"reach: offset from the target, in world axes"},"look_at":{"type":"boolean","default":true,"description":"reach: also point the hand at the target"},"pawn":{"type":"string","description":"Which pawn; defaults to the player pawn"},"world":{"type":"string","enum":["editor","pie"]}},"required":["action"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			if (!World)
			{
				return FUplinkToolResult::Error(Error);
			}

			const FXRRig Rig = FindRig(World, GetString(Ctx.Params, TEXT("pawn")), Error);
			if (!Rig.Pawn)
			{
				return FUplinkToolResult::Error(Error);
			}

			const FString Action = GetString(Ctx.Params, TEXT("action"), TEXT("status"));
			const FString Mode = GetString(Ctx.Params, TEXT("mode"), TEXT("auto"));
			if (Mode != TEXT("auto") && Mode != TEXT("vr") && Mode != TEXT("desktop"))
			{
				return FUplinkToolResult::Error(TEXT("unknown mode (auto, vr, desktop)"));
			}

			const FHeadsetState Headset = ReadHeadset(World);
			const FRigVerdict Hands = JudgeHands(Rig.All);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("pawn"), Rig.Pawn->GetName());
			Data->SetStringField(TEXT("pawnClass"), Rig.Pawn->GetClass()->GetName());

			TSharedRef<FJsonObject> HeadsetJson = MakeShared<FJsonObject>();
			HeadsetJson->SetBoolField(TEXT("trackingSystem"), Headset.bTrackingSystem);
			HeadsetJson->SetBoolField(TEXT("connected"), Headset.bConnected);
			HeadsetJson->SetBoolField(TEXT("headTracking"), Headset.bHeadTracking);
			HeadsetJson->SetBoolField(TEXT("stereo"), Headset.bStereo);
			HeadsetJson->SetStringField(TEXT("systemName"), Headset.SystemName);
			Data->SetObjectField(TEXT("headset"), HeadsetJson);

			TSharedRef<FJsonObject> HandsJson = MakeShared<FJsonObject>();
			HandsJson->SetBoolField(TEXT("usable"), Hands.bUsable);
			TArray<TSharedPtr<FJsonValue>> Reasons;
			for (const FString& Reason : Hands.Reasons)
			{
				Reasons.Add(MakeShared<FJsonValueString>(Reason));
			}
			HandsJson->SetArrayField(TEXT("notes"), Reasons);
			Data->SetObjectField(TEXT("hands"), HandsJson);

			// The mode actually in force, once 'auto' has looked at the rig.
			const FString EffectiveMode = Mode == TEXT("auto")
				? (Hands.bUsable ? TEXT("vr") : TEXT("desktop"))
				: Mode;
			Data->SetStringField(TEXT("mode"), EffectiveMode);

			// Refusing here is the point: a pawn that fell back to desktop
			// would otherwise accept every hand pose and pass a test that
			// exercised nothing.
			const bool bTouchesHands = (Action == TEXT("reset"))
				|| ((Action == TEXT("pose") || Action == TEXT("reach"))
					&& GetString(Ctx.Params, TEXT("device")) != TEXT("hmd"));
			if (Mode == TEXT("vr") && bTouchesHands && !Hands.bUsable)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("mode 'vr' asked for, but this pawn's hands are not in a usable state: %s. ")
					TEXT("A pawn with a desktop fallback often takes that branch when no headset is present, ")
					TEXT("in which case posing its controllers changes nothing. Put the pawn into its VR path first, ")
					TEXT("or use mode 'desktop' to drive the camera instead."),
					*FString::Join(Hands.Reasons, TEXT("; "))));
			}

			auto ReportRig = [&Rig, &Data]()
			{
				if (Rig.Camera)
				{
					Data->SetObjectField(TEXT("hmd"), TransformJson(Rig.Camera));
				}
				if (Rig.Left)
				{
					Data->SetObjectField(TEXT("left"), TransformJson(Rig.Left));
				}
				if (Rig.Right)
				{
					Data->SetObjectField(TEXT("right"), TransformJson(Rig.Right));
				}
				Data->SetNumberField(TEXT("controllerCount"), Rig.All.Num());
			};

			if (Action == TEXT("status"))
			{
				ReportRig();
				TArray<FString> Missing;
				if (!Rig.Camera) { Missing.Add(TEXT("camera")); }
				if (!Rig.Left) { Missing.Add(TEXT("left controller")); }
				if (!Rig.Right) { Missing.Add(TEXT("right controller")); }

				FString Summary;
				if (Missing.Num() > 0)
				{
					Summary = FString::Printf(TEXT("this pawn has no %s - it may not be a VR pawn"),
						*FString::Join(Missing, TEXT(", ")));
				}
				else if (Hands.bUsable)
				{
					Summary = FString::Printf(
						TEXT("full rig, hands look usable - testable as VR%s"),
						Headset.bConnected ? TEXT(" (a headset is connected, so the game also drives them)") : TEXT(" with no headset"));
				}
				else
				{
					Summary = FString::Printf(
						TEXT("full rig, but the hands are not in a usable state (%s) - this pawn is behaving as desktop; test it with mode 'desktop'"),
						*FString::Join(Hands.Reasons, TEXT("; ")));
				}
				return FUplinkToolResult::Ok(Data, Summary);
			}

			if (Action == TEXT("reset"))
			{
				// Neutral: hands in front of the chest, shoulder width apart.
				// Relative to the rig, so it is correct wherever the pawn is.
				if (Rig.Left)
				{
					Rig.Left->SetRelativeLocationAndRotation(FVector(40, -20, 0), FRotator::ZeroRotator);
				}
				if (Rig.Right)
				{
					Rig.Right->SetRelativeLocationAndRotation(FVector(40, 20, 0), FRotator::ZeroRotator);
				}
				ReportRig();
				return FUplinkToolResult::Ok(Data, Rig.HasHands()
					? TEXT("hands reset to a neutral pose")
					: TEXT("nothing to reset - this pawn has no motion controllers"));
			}

			// In desktop mode there are no hands to speak of, so a device that
			// was not asked for explicitly means the camera - looking at a
			// thing is what a desktop player does before interacting with it.
			FString Device = GetString(Ctx.Params, TEXT("device"));
			if (EffectiveMode == TEXT("desktop") && (Device.IsEmpty() || Device != TEXT("hmd")))
			{
				if (Mode == TEXT("desktop") || Device.IsEmpty())
				{
					Device = TEXT("hmd");
					Data->SetBoolField(TEXT("redirectedToCamera"), true);
				}
			}
			if (Device.IsEmpty())
			{
				return FUplinkToolResult::Error(TEXT("'device' is required (hmd, left, right)"));
			}

			USceneComponent* Component = DeviceComponent(Rig, Device, Error);
			if (!Component)
			{
				return FUplinkToolResult::Error(Error);
			}

			if (Action == TEXT("reach"))
			{
				const FString TargetName = GetString(Ctx.Params, TEXT("target"));
				AActor* Target = FindActor(World, TargetName);
				if (!Target)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no actor named '%s' to reach for"), *TargetName));
				}

				FVector Goal = Target->GetActorLocation();
				if (FVector Offset; TryGetVector(Ctx.Params, TEXT("offset"), Offset))
				{
					Goal += Offset;
				}

				const bool bCamera = Device == TEXT("hmd");
				if (bCamera)
				{
					// The camera stays where it is and turns to face the
					// target; teleporting a player's viewpoint onto an object
					// is not what "look at it" means.
					const FVector Direction = Goal - Component->GetComponentLocation();
					if (Direction.IsNearlyZero())
					{
						return FUplinkToolResult::Error(TEXT("the camera is already at that location, so there is no direction to look"));
					}
					Component->SetWorldRotation(Direction.Rotation());
				}
				else
				{
					Component->SetWorldLocation(Goal);

					bool bLookAt = true;
					Ctx.Params->TryGetBoolField(FStringView(TEXT("look_at")), bLookAt);
					if (bLookAt)
					{
						// Point the hand from where it now is toward the
						// target's centre; a hand that reaches but faces away
						// still fails the direction checks an interaction
						// system may do.
						const FVector Direction = Target->GetActorLocation() - Component->GetComponentLocation();
						if (!Direction.IsNearlyZero())
						{
							Component->SetWorldRotation(Direction.Rotation());
						}
					}
				}

				Data->SetStringField(TEXT("device"), Device);
				Data->SetStringField(TEXT("target"), Target->GetName());
				Data->SetNumberField(TEXT("distanceToTarget"),
					FVector::Dist(Component->GetComponentLocation(), Target->GetActorLocation()));
				ReportRig();
				return FUplinkToolResult::Ok(Data, bCamera
					? FString::Printf(TEXT("camera turned to look at %s"), *Target->GetName())
					: FString::Printf(TEXT("%s hand moved to %s"), *Device, *Target->GetName()));
			}

			if (Action != TEXT("pose"))
			{
				return FUplinkToolResult::Error(TEXT("unknown action (status, pose, reach, reset)"));
			}

			const bool bWorldSpace = GetString(Ctx.Params, TEXT("space"), TEXT("local")) == TEXT("world");
			FVector Location;
			const bool bHasLocation = TryGetVector(Ctx.Params, TEXT("location"), Location);
			FRotator Rotation;
			const bool bHasRotation = TryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);
			if (!bHasLocation && !bHasRotation)
			{
				return FUplinkToolResult::Error(TEXT("'pose' needs a 'location' and/or a 'rotation'"));
			}

			if (bHasLocation)
			{
				bWorldSpace ? Component->SetWorldLocation(Location) : Component->SetRelativeLocation(Location);
			}
			if (bHasRotation)
			{
				bWorldSpace ? Component->SetWorldRotation(Rotation) : Component->SetRelativeRotation(Rotation);
			}

			Data->SetStringField(TEXT("device"), Device);
			Data->SetStringField(TEXT("space"), bWorldSpace ? TEXT("world") : TEXT("local"));
			ReportRig();
			return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("%s posed"), *Device));
		});
}
