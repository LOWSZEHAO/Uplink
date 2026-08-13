// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// PIE lifecycle tools: pie_start, pie_stop, pie_status, pie_pause, pie_resume, pie_step.

#include "UplinkTools.h"
#include "UplinkPieManager.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "GameFramework/GameModeBase.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UnrealEdGlobals.h"

using namespace UplinkToolUtil;

namespace
{
	// ---- pie_start ---------------------------------------------------------

	class FPieStartInvocation final : public IUplinkInvocation
	{
	public:
		explicit FPieStartInvocation(FUplinkPieManager& InPie) : Pie(InPie) {}

		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			if (Pie.GetState() != EUplinkPieState::None)
			{
				Out = FUplinkToolResult::Error(TEXT("a PIE session is already running or starting; call pie_stop first"));
				return EUplinkToolStep::Done;
			}
			if (!GEditor)
			{
				Out = FUplinkToolResult::Error(TEXT("no editor"));
				return EUplinkToolStep::Done;
			}

			FRequestPlaySessionParams Params;

			// Automation-friendly play settings: keep the editor visible and
			// leave the user's mouse alone.
			ULevelEditorPlaySettings* PlaySettings =
				DuplicateObject(GetMutableDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
			PlaySettings->GameGetsMouseControl = false;
			PlaySettings->bShouldMinimizeEditorOnNonVRPIE = false;
			Params.EditorPlaySettings = PlaySettings;

			const FString Mode = GetString(Ctx.Params, TEXT("mode"), TEXT("viewport"));
			if (Mode == TEXT("viewport"))
			{
				FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
				const TSharedPtr<IAssetViewport> Viewport = LevelEditor.GetFirstActiveViewport();
				if (Viewport.IsValid())
				{
					Params.DestinationSlateViewport = TWeakPtr<IAssetViewport>(Viewport);
				}
				// No viewport available -> falls through to a new window.
			}
			else if (Mode != TEXT("window"))
			{
				Out = FUplinkToolResult::Error(TEXT("mode must be 'viewport' or 'window'"));
				return EUplinkToolStep::Done;
			}

			FVector Location;
			if (TryGetVector(Ctx.Params, TEXT("location"), Location))
			{
				Params.StartLocation = Location;
				FRotator Rotation = FRotator::ZeroRotator;
				TryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);
				Params.StartRotation = Rotation;
			}

			const FString GameModePath = GetString(Ctx.Params, TEXT("game_mode"));
			if (!GameModePath.IsEmpty())
			{
				UClass* GameModeClass = StaticLoadClass(AGameModeBase::StaticClass(), nullptr, *GameModePath);
				if (!GameModeClass)
				{
					Out = FUplinkToolResult::Error(FString::Printf(TEXT("game_mode class not found: %s"), *GameModePath));
					return EUplinkToolStep::Done;
				}
				Params.GameModeOverride = GameModeClass;
			}

			const FString MapOverride = GetString(Ctx.Params, TEXT("map"));
			if (!MapOverride.IsEmpty())
			{
				Params.GlobalMapOverride = MapOverride;
			}

			SerialBefore = Pie.GetSessionSerial();
			GEditor->RequestPlaySession(Params);
			bRequested = true;
			return EUplinkToolStep::Pending;
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			if (Pie.GetSessionSerial() != SerialBefore && Pie.GetState() == EUplinkPieState::Running)
			{
				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("map"), Pie.GetPieMapName());
				Data->SetNumberField(TEXT("log_start_index"), static_cast<double>(Pie.GetLogIndexAtStart()));
				Out = FUplinkToolResult::Ok(Data, TEXT("PIE session running (BeginPlay has executed)"));
				return EUplinkToolStep::Done;
			}

			// A failed start winds down through EndPIE/Cancel back to None
			// without ever bumping the serial.
			if (bRequested && !GEditor->IsPlaySessionRequestQueued() && Pie.GetState() == EUplinkPieState::None)
			{
				// Give the request one tick of grace before declaring failure.
				if (++TicksInNone > 2)
				{
					Out = FUplinkToolResult::Error(TEXT("PIE failed to start (check output_log for map/game-mode errors)"));
					return EUplinkToolStep::Done;
				}
			}
			return EUplinkToolStep::Pending;
		}

	private:
		FUplinkPieManager& Pie;
		uint64 SerialBefore = 0;
		bool bRequested = false;
		int32 TicksInNone = 0;
	};

	// ---- pie_stop ----------------------------------------------------------

	class FPieStopInvocation final : public IUplinkInvocation
	{
	public:
		explicit FPieStopInvocation(FUplinkPieManager& InPie) : Pie(InPie) {}

		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			if (Pie.GetState() == EUplinkPieState::None)
			{
				Out = FUplinkToolResult::Ok(nullptr, TEXT("no PIE session was running"));
				return EUplinkToolStep::Done;
			}
			GEditor->RequestEndPlayMap();
			return EUplinkToolStep::Pending;
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			if (Pie.GetState() == EUplinkPieState::None)
			{
				Out = FUplinkToolResult::Ok(nullptr, TEXT("PIE session ended"));
				return EUplinkToolStep::Done;
			}
			return EUplinkToolStep::Pending;
		}

	private:
		FUplinkPieManager& Pie;
	};

	// ---- pie_step ----------------------------------------------------------

	class FPieStepInvocation final : public IUplinkInvocation
	{
	public:
		explicit FPieStepInvocation(FUplinkPieManager& InPie) : Pie(InPie) {}

		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			if (Pie.GetState() != EUplinkPieState::Running || !GEditor->PlayWorld)
			{
				Out = FUplinkToolResult::Error(TEXT("no running PIE session"));
				return EUplinkToolStep::Done;
			}
			if (!Pie.IsPaused())
			{
				Out = FUplinkToolResult::Error(TEXT("PIE must be paused first (pie_pause) before stepping frames"));
				return EUplinkToolStep::Done;
			}

			Remaining = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("frames"), 1)), 1, 600);
			Requested = Remaining;
			BeginStep();
			return EUplinkToolStep::Pending;
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
			if (!PlayWorld)
			{
				Out = FUplinkToolResult::Error(TEXT("PIE session ended while stepping"));
				return EUplinkToolStep::Done;
			}

			// The engine clears bDebugFrameStepExecution once the stepped frame
			// has ticked and the world is paused again.
			if (PlayWorld->bDebugFrameStepExecution)
			{
				return EUplinkToolStep::Pending;
			}

			if (--Remaining > 0)
			{
				BeginStep();
				return EUplinkToolStep::Pending;
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("frames_stepped"), Requested);
			Out = FUplinkToolResult::Ok(Data, TEXT("stepped; world is paused"));
			return EUplinkToolStep::Done;
		}

	private:
		void BeginStep()
		{
			GEditor->PlayWorld->bDebugFrameStepExecution = true;
			if (GUnrealEd)
			{
				GUnrealEd->PlaySessionSingleStepped();
			}
		}

		FUplinkPieManager& Pie;
		int32 Remaining = 0;
		int32 Requested = 0;
	};

	FUplinkToolResult SetPaused(FUplinkPieManager& Pie, bool bPause)
	{
		if (Pie.GetState() != EUplinkPieState::Running || !GEditor->PlayWorld)
		{
			return FUplinkToolResult::Error(TEXT("no running PIE session"));
		}
		if (Pie.IsPaused() == bPause)
		{
			return FUplinkToolResult::Ok(nullptr, bPause ? TEXT("already paused") : TEXT("already running"));
		}

		GEditor->SetPIEWorldsPaused(bPause);
		if (GUnrealEd)
		{
			// Broadcast PausePIE/ResumePIE so editor UI and listeners stay in sync.
			bPause ? GUnrealEd->PlaySessionPaused() : GUnrealEd->PlaySessionResumed();
		}
		return FUplinkToolResult::Ok(nullptr, bPause ? TEXT("paused") : TEXT("resumed"));
	}
}

void UplinkTools::RegisterPie(FUplinkToolRegistry& Registry, FUplinkPieManager& Pie)
{
	{
		FUplinkToolInfo Info;
		Info.Name = TEXT("pie_start");
		Info.Description = TEXT("Start a Play-In-Editor session and wait until gameplay has actually begun (BeginPlay executed). Optional: play in the level viewport or a new window, override the player start location/rotation, the game mode, or the map.");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"mode":{"type":"string","enum":["viewport","window"],"default":"viewport"},"location":{"type":"object","description":"Optional spawn override","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"rotation":{"type":"object","properties":{"pitch":{"type":"number"},"yaw":{"type":"number"},"roll":{"type":"number"}}},"game_mode":{"type":"string","description":"Optional GameMode class path override"},"map":{"type":"string","description":"Optional map override, e.g. /Game/Maps/Arena"}}})json"));
		Info.bReadOnly = false;
		Info.TimeoutSeconds = 180.0;
		Registry.Register(MoveTemp(Info), [&Pie]() -> TSharedRef<IUplinkInvocation>
		{
			return MakeShared<FPieStartInvocation>(Pie);
		});
	}

	{
		FUplinkToolInfo Info;
		Info.Name = TEXT("pie_stop");
		Info.Description = TEXT("End the current Play-In-Editor session and wait for it to fully shut down.");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"));
		Info.bReadOnly = false;
		Info.TimeoutSeconds = 60.0;
		Registry.Register(MoveTemp(Info), [&Pie]() -> TSharedRef<IUplinkInvocation>
		{
			return MakeShared<FPieStopInvocation>(Pie);
		});
	}

	{
		FUplinkToolInfo Info;
		Info.Name = TEXT("pie_step");
		Info.Description = TEXT("Advance a paused PIE session by exactly N frames, then re-pause. Pause with pie_pause first.");
		Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"frames":{"type":"number","default":1,"description":"1-600"}}})json"));
		Info.bReadOnly = false;
		Info.TimeoutSeconds = 120.0;
		Registry.Register(MoveTemp(Info), [&Pie]() -> TSharedRef<IUplinkInvocation>
		{
			return MakeShared<FPieStepInvocation>(Pie);
		});
	}

	Registry.RegisterQuick(
		TEXT("pie_status"),
		TEXT("State of the Play-In-Editor session: none/starting/running/stopping, paused flag, map, elapsed seconds, and the output_log index at session start (pass it to output_log as since_index to read only this session's logs)."),
		TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
		/*bReadOnly=*/true,
		[&Pie](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			const EUplinkPieState State = Pie.GetState();
			const TCHAR* StateNames[] = { TEXT("none"), TEXT("starting"), TEXT("running"), TEXT("stopping") };
			Data->SetStringField(TEXT("state"), StateNames[static_cast<int32>(State)]);
			Data->SetBoolField(TEXT("paused"), Pie.IsPaused());
			if (State == EUplinkPieState::Running)
			{
				Data->SetStringField(TEXT("map"), Pie.GetPieMapName());
				Data->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Pie.GetSessionStartTime());
				Data->SetNumberField(TEXT("log_start_index"), static_cast<double>(Pie.GetLogIndexAtStart()));
			}
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("pie_pause"),
		TEXT("Pause the running PIE session (freezes the game world; the editor stays interactive)."),
		TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
		/*bReadOnly=*/false,
		[&Pie](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			return SetPaused(Pie, true);
		});

	Registry.RegisterQuick(
		TEXT("pie_resume"),
		TEXT("Resume a paused PIE session."),
		TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
		/*bReadOnly=*/false,
		[&Pie](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			return SetPaused(Pie, false);
		});
}
