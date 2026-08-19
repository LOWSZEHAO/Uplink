// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Core tools: console_command, output_log, task_status / task_result / task_cancel / task_list, status.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkTaskManager.h"
#include "UplinkLogCapture.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/OutputDeviceHelper.h"

using namespace UplinkToolUtil;

namespace
{
	ELogVerbosity::Type ParseVerbosity(const FString& In)
	{
		if (In.Equals(TEXT("error"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Error; }
		if (In.Equals(TEXT("warning"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Warning; }
		if (In.Equals(TEXT("display"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Display; }
		if (In.Equals(TEXT("verbose"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Verbose; }
		return ELogVerbosity::Log;
	}

	FGuid ParseTaskId(const FUplinkToolContext& Ctx, FString& OutError)
	{
		FGuid Id;
		if (!FGuid::Parse(GetString(Ctx.Params, TEXT("task_id")), Id))
		{
			OutError = TEXT("invalid or missing 'task_id'");
		}
		return Id;
	}
}

void UplinkTools::RegisterCore(FUplinkToolRegistry& Registry, FUplinkLogCapture& LogCapture, FUplinkTaskManager& Tasks)
{
	Registry.RegisterQuick(
		TEXT("status"),
		TEXT("Editor status: engine version, loaded project, current map, and whether a PIE (Play-In-Editor) session is running."),
		TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString(EVersionComponent::Patch));
			Data->SetStringField(TEXT("project"), FApp::GetProjectName());
			const UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			Data->SetStringField(TEXT("map"), EditorWorld ? EditorWorld->GetOutermost()->GetName() : TEXT(""));
			Data->SetBoolField(TEXT("pie_active"), GEditor != nullptr && GEditor->PlayWorld != nullptr);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("console_command"),
		TEXT("Run an Unreal console command (e.g. 'stat fps', 'ke * ...', 'open MapName') and return its captured output. Runs against the PIE world when one is active unless world='editor'."),
		TEXT(R"json({"type":"object","properties":{"command":{"type":"string","description":"The console command line"},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["command"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Command = GetString(Ctx.Params, TEXT("command"));
			if (Command.IsEmpty())
			{
				return FUplinkToolResult::Error(TEXT("'command' is required"));
			}

			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return FUplinkToolResult::Error(WorldError);
			}

			FStringOutputDevice Captured;
			const bool bHandled = GEngine->Exec(World, *Command, Captured);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("handled"), bHandled);
			Data->SetStringField(TEXT("output"), Captured);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("output_log"),
		TEXT("Read recent editor/game log lines from an in-memory ring buffer. Returns lines plus 'next_index'; pass it back as 'since_index' to read only what is new (ideal for watching a PIE session)."),
		TEXT(R"json({"type":"object","properties":{"since_index":{"type":"number","description":"Only lines at or after this index (from a previous call's next_index)"},"max":{"type":"number","default":100},"category":{"type":"string","description":"Substring filter on log category, e.g. LogBlueprint"},"contains":{"type":"string","description":"Substring filter on the line text"},"verbosity":{"type":"string","enum":["error","warning","display","log","verbose"],"description":"Minimum severity to include (default log)"}}})json"),
		/*bReadOnly=*/true,
		[&LogCapture](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const int64 Since = static_cast<int64>(GetNumber(Ctx.Params, TEXT("since_index"), 0));
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 100)), 1, 1000);
			const ELogVerbosity::Type MinVerbosity = ParseVerbosity(GetString(Ctx.Params, TEXT("verbosity"), TEXT("log")));

			const TArray<FUplinkLogCapture::FLine> Lines = LogCapture.Get(
				Since, Max, GetString(Ctx.Params, TEXT("category")), GetString(Ctx.Params, TEXT("contains")), MinVerbosity);

			TArray<TSharedPtr<FJsonValue>> JsonLines;
			for (const FUplinkLogCapture::FLine& Line : Lines)
			{
				JsonLines.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("[%s] %s: %s"),
					ToString(Line.Verbosity), *Line.Category.ToString(), *Line.Text)));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("lines"), JsonLines);
			Data->SetNumberField(TEXT("next_index"), static_cast<double>(LogCapture.NewestIndex()));
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("task_status"),
		TEXT("Check a background task started by a long-running tool (a call that returned a task_id with status 'running')."),
		TEXT(R"json({"type":"object","properties":{"task_id":{"type":"string"}},"required":["task_id"]})json"),
		/*bReadOnly=*/true,
		[&Tasks](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			const FGuid Id = ParseTaskId(Ctx, Error);
			if (!Error.IsEmpty())
			{
				return FUplinkToolResult::Error(Error);
			}
			const FUplinkTask* Task = Tasks.Find(Id);
			if (!Task)
			{
				return FUplinkToolResult::Error(TEXT("unknown task id (results are retained ~3 minutes)"));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("task_id"), Task->Id.ToString(EGuidFormats::DigitsWithHyphens));
			Data->SetStringField(TEXT("tool"), Task->ToolName);
			Data->SetStringField(TEXT("status"), FUplinkTaskManager::StatusToString(Task->Status));
			Data->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Task->StartedAt);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("task_result"),
		TEXT("Fetch the final result of a finished background task."),
		TEXT(R"json({"type":"object","properties":{"task_id":{"type":"string"}},"required":["task_id"]})json"),
		/*bReadOnly=*/true,
		[&Tasks](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			const FGuid Id = ParseTaskId(Ctx, Error);
			if (!Error.IsEmpty())
			{
				return FUplinkToolResult::Error(Error);
			}
			const FUplinkTask* Task = Tasks.Find(Id);
			if (!Task)
			{
				return FUplinkToolResult::Error(TEXT("unknown task id (results are retained ~3 minutes)"));
			}
			if (Task->Status == EUplinkTaskStatus::Running)
			{
				return FUplinkToolResult::Error(TEXT("task is still running; poll task_status first"));
			}
			return Task->Result; // includes Png for screenshot tasks
		});

	Registry.RegisterQuick(
		TEXT("task_cancel"),
		TEXT("Cancel a running background task."),
		TEXT(R"json({"type":"object","properties":{"task_id":{"type":"string"}},"required":["task_id"]})json"),
		/*bReadOnly=*/false,
		[&Tasks](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			const FGuid Id = ParseTaskId(Ctx, Error);
			if (!Error.IsEmpty())
			{
				return FUplinkToolResult::Error(Error);
			}
			// RequestCancel distinguishes four outcomes and writes a sentence
			// for each. The older Cancel() collapses them into a bool, so a
			// task that refused interruption - still running, ending on its own
			// deadline - was reported as "task not found or not running", which
			// states the opposite of what happened.
			FString Outcome;
			switch (Tasks.RequestCancel(Id, Outcome))
			{
			case EUplinkCancelOutcome::Stopped:
				return FUplinkToolResult::Ok(nullptr, TEXT("cancelled"));

			case EUplinkCancelOutcome::AlreadyEnded:
				// Nothing is running, which is what the caller wanted; saying
				// "cancelled" would take credit for someone else's ending.
				return FUplinkToolResult::Ok(nullptr, Outcome);

			case EUplinkCancelOutcome::Refused:
			case EUplinkCancelOutcome::UnknownTask:
			default:
				return FUplinkToolResult::Error(Outcome);
			}
		});

	Registry.RegisterQuick(
		TEXT("task_list"),
		TEXT("List recent background tasks and their states."),
		TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
		/*bReadOnly=*/true,
		[&Tasks](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			TArray<TSharedPtr<FJsonValue>> List;
			for (const FUplinkTask* Task : Tasks.Snapshot())
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("task_id"), Task->Id.ToString(EGuidFormats::DigitsWithHyphens));
				Row->SetStringField(TEXT("tool"), Task->ToolName);
				Row->SetStringField(TEXT("status"), FUplinkTaskManager::StatusToString(Task->Status));
				List.Add(MakeShared<FJsonValueObject>(Row));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("tasks"), List);
			return FUplinkToolResult::Ok(Data);
		});
}
