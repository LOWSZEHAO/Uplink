// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Developer-loop tools: live_compile - trigger a Live Coding patch of the
// running editor so C++ iteration doesn't need an editor restart.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Modules/ModuleManager.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

using namespace UplinkToolUtil;

#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

namespace
{
#if WITH_LIVE_CODING
	/**
	 * Latent: kicks off an async Live Coding compile and ticks until the
	 * compiler goes idle. A successful patch fires the module's patch-complete
	 * delegate; if the compile ends without one, there were either no changes
	 * or errors, and nothing outside the module can tell those two apart - so
	 * the message says where to look instead of picking one.
	 */
	class FLiveCompileInvocation : public IUplinkInvocation
	{
	public:
		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
			if (!LiveCoding)
			{
				Out = FUplinkToolResult::Error(TEXT("Live Coding module is not loaded in this editor"));
				return EUplinkToolStep::Done;
			}
			if (!LiveCoding->IsEnabledForSession())
			{
				if (LiveCoding->CanEnableForSession())
				{
					LiveCoding->EnableForSession(true);
				}
				if (!LiveCoding->IsEnabledForSession())
				{
					Out = FUplinkToolResult::Error(FString::Printf(
						TEXT("Live Coding could not be enabled for this session: %s"),
						*LiveCoding->GetEnableErrorText().ToString()));
					return EUplinkToolStep::Done;
				}
			}
			if (LiveCoding->IsCompiling())
			{
				Out = FUplinkToolResult::Error(TEXT("a Live Coding compile is already in progress"));
				return EUplinkToolStep::Done;
			}

			PatchHandle = LiveCoding->GetOnPatchCompleteDelegate().AddLambda([this]()
			{
				bPatched = true;
			});
			StartTime = FPlatformTime::Seconds();

			// The argument-less Compile() throws the outcome away, and a request
			// that never became a compile leaves IsCompiling() false - so the
			// first Tick read it as a compile that had already finished with
			// nothing to do, and a console that failed to start was reported as
			// "no changes".
			ELiveCodingCompileResult Started = ELiveCodingCompileResult::Failure;
			if (!LiveCoding->Compile(ELiveCodingCompileFlags::None, &Started))
			{
				LiveCoding->GetOnPatchCompleteDelegate().Remove(PatchHandle);
				PatchHandle.Reset();
				Out = FUplinkToolResult::Error(Started == ELiveCodingCompileResult::CompileStillActive
					? TEXT("a Live Coding compile is already in progress")
					: TEXT("Live Coding did not start a compile - its console process is not running; open it from the editor (Ctrl+Alt+F11) and try again"));
				return EUplinkToolStep::Done;
			}
			bCompileStarted = true;
			return EUplinkToolStep::Pending;
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
			if (LiveCoding && LiveCoding->IsCompiling())
			{
				return EUplinkToolStep::Pending;
			}

			if (LiveCoding && PatchHandle.IsValid())
			{
				LiveCoding->GetOnPatchCompleteDelegate().Remove(PatchHandle);
				PatchHandle.Reset();
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("patched"), bPatched);
			Data->SetNumberField(TEXT("seconds"), FPlatformTime::Seconds() - StartTime);
			Out = FUplinkToolResult::Ok(Data, bPatched
				? TEXT("patched - new code is live")
				: TEXT("compile finished without a patch - either nothing had changed or the build failed; read output_log with category 'LogLiveCoding' to see which"));
			return EUplinkToolStep::Done;
		}

		/**
		 * A compile cannot be called back. The console process is already
		 * building and will patch this editor when it finishes, whatever is
		 * decided here, so answering a cancel with "stopped" would promise
		 * something that does not happen.
		 */
		virtual bool CanCancel() const override
		{
			return !bCompileStarted;
		}

		virtual ~FLiveCompileInvocation() override
		{
			if (PatchHandle.IsValid())
			{
				if (ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME))
				{
					LiveCoding->GetOnPatchCompleteDelegate().Remove(PatchHandle);
				}
			}
		}

	private:
		FDelegateHandle PatchHandle;
		double StartTime = 0.0;
		bool bPatched = false;
		bool bCompileStarted = false;
	};
#endif // WITH_LIVE_CODING
}

void UplinkTools::RegisterDev(FUplinkToolRegistry& Registry)
{
	FUplinkToolInfo Info;
	Info.Name = TEXT("live_compile");
	Info.Description = TEXT("Trigger a Live Coding compile and patch the RUNNING editor with changed C++ - no editor restart. Function-body edits apply in seconds; structural changes (new classes, members, virtuals) still need a real build. Resolves when the compiler goes idle: 'patched' true means new code is live. 'patched' false is NOT reported as an error - it means either nothing had changed or the build failed, and only the log tells those apart (output_log with category 'LogLiveCoding'). A compile that never started is an error. Once started it cannot be cancelled: the console process patches this editor whether or not the task is still waiting.");
	Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"));
	Info.bReadOnly = false;
	// A Live Coding compile is exactly when the user keeps working in the
	// editor; an open transaction would swallow their hand edits and let one
	// Ctrl+Z roll them back under an Uplink name.
	Info.bTransactional = false;
	Info.TimeoutSeconds = 600.0;
#if WITH_LIVE_CODING
	Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
	{
		return MakeShared<FLiveCompileInvocation>();
	});
#else
	Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
	{
		class FUnsupported : public IUplinkInvocation
		{
			virtual EUplinkToolStep Start(const FUplinkToolContext&, FUplinkToolResult& Out) override
			{
				Out = FUplinkToolResult::Error(TEXT("Live Coding is not available in this build"));
				return EUplinkToolStep::Done;
			}
		};
		return MakeShared<FUnsupported>();
	});
#endif

	Registry.RegisterQuick(
		TEXT("plugin_list"),
		TEXT("List engine and project plugins with their enabled state - what capabilities this project can draw on. Content and classes of every ENABLED plugin are already reachable (asset_search with path_prefix /PluginName, call_function/class_info see all loaded classes)."),
		TEXT(R"json({"type":"object","properties":{"filter":{"type":"string"},"enabled_only":{"type":"boolean","default":false},"max":{"type":"number","default":60}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Filter = GetString(Ctx.Params, TEXT("filter"));
			bool bEnabledOnly = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("enabled_only")), bEnabledOnly);
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 60)), 1, 500);

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Total = 0;
			for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
			{
				if (bEnabledOnly && !Plugin->IsEnabled())
				{
					continue;
				}
				if (!Filter.IsEmpty()
					&& !Plugin->GetName().Contains(Filter, ESearchCase::IgnoreCase)
					&& !Plugin->GetDescriptor().FriendlyName.Contains(Filter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				++Total;
				if (Rows.Num() >= Max)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Plugin->GetName());
				Row->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
				Row->SetStringField(TEXT("category"), Plugin->GetDescriptor().Category);
				if (Plugin->CanContainContent())
				{
					Row->SetStringField(TEXT("content_root"), FString::Printf(TEXT("/%s"), *Plugin->GetName()));
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("plugins"), Rows);
			Data->SetNumberField(TEXT("total_matching"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Rows.Num());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("edit_history"),
		TEXT("Inspect and walk the editor's undo stack. A mutating Uplink tool that edits the editor world runs inside its own transaction named 'Uplink: <tool>', so an agent's edit can be undone exactly like a hand edit. Two kinds of call are NOT on the stack: anything done while a play session is running, the editor world included, because the engine discards transactions that touch play objects; and the tools that drive the session or the compiler rather than edit the level (pie_start, pie_stop, pie_step, input_action, input_key, input_replay, navigate_to, run_tests, run_scenario, live_compile, and this tool). 'action': 'list' (default) shows what undo/redo would do, 'undo' or 'redo' walk 'steps' entries and report the transactions that actually moved. Walking an empty stack is a success - there was nothing to do. Being refused is an error: 'undoBlocked' then carries the buffer's own reason, which is a transaction still open elsewhere or an undo barrier, and is a different situation from an empty stack."),
		TEXT(R"json({"type":"object","properties":{"action":{"type":"string","enum":["list","undo","redo"],"default":"list"},"steps":{"type":"number","default":1,"description":"how many transactions to walk (undo/redo)"}}})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			if (!GEditor || !GEditor->Trans)
			{
				return FUplinkToolResult::Error(TEXT("no editor transaction buffer available"));
			}
			UTransactor* Trans = GEditor->Trans;

			const FString Action = GetString(Ctx.Params, TEXT("action"), TEXT("list"));
			int32 Steps = 1;
			if (double AsNumber = 0.0; Ctx.Params->TryGetNumberField(FStringView(TEXT("steps")), AsNumber))
			{
				Steps = FMath::Max(1, static_cast<int32>(AsNumber));
			}

			auto Describe = [Trans](TSharedRef<FJsonObject> Data)
			{
				FText UndoText;
				FText RedoText;
				const bool bCanUndo = Trans->CanUndo(&UndoText);
				const bool bCanRedo = Trans->CanRedo(&RedoText);
				Data->SetBoolField(TEXT("canUndo"), bCanUndo);
				Data->SetBoolField(TEXT("canRedo"), bCanRedo);
				Data->SetStringField(TEXT("nextUndo"),
					bCanUndo ? Trans->GetUndoContext().Title.ToString() : FString());
				Data->SetStringField(TEXT("nextRedo"),
					bCanRedo ? Trans->GetRedoContext().Title.ToString() : FString());

				// An empty nextUndo reads as an empty stack when the buffer is
				// saying something else entirely - a transaction open elsewhere,
				// or an undo barrier, blocks undo just as completely and is the
				// difference between "nothing to undo" and "not right now".
				if (!bCanUndo)
				{
					Data->SetStringField(TEXT("undoBlocked"), UndoText.ToString());
				}
				if (!bCanRedo)
				{
					Data->SetStringField(TEXT("redoBlocked"), RedoText.ToString());
				}
				Data->SetNumberField(TEXT("queueLength"), Trans->GetQueueLength());
				Data->SetNumberField(TEXT("undoCount"), Trans->GetUndoCount());
			};

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

			if (Action == TEXT("list"))
			{
				Describe(Data);
				return FUplinkToolResult::Ok(Data);
			}

			const bool bUndo = Action == TEXT("undo");
			if (!bUndo && Action != TEXT("redo"))
			{
				return FUplinkToolResult::Error(TEXT("unknown action (list, undo, redo)"));
			}

			// Record what each step actually was, so the caller learns which
			// edits were rolled back rather than just a count.
			TArray<TSharedPtr<FJsonValue>> Walked;
			bool bRefused = false;
			for (int32 i = 0; i < Steps; ++i)
			{
				if (bUndo ? !Trans->CanUndo() : !Trans->CanRedo())
				{
					break;
				}
				const FString Queued = bUndo
					? Trans->GetUndoContext().Title.ToString()
					: Trans->GetRedoContext().Title.ToString();
				const bool bOk = bUndo ? GEditor->UndoTransaction() : GEditor->RedoTransaction();
				if (!bOk)
				{
					// The buffer said the step was possible, so the editor turned
					// it down: UndoTransaction bails out during a package save or
					// a garbage collection. Reporting an empty stack here sends
					// the caller to look at an undo history that is fine.
					bRefused = true;
					break;
				}

				// One Undo() can eat several queue entries: the engine skips
				// expired transactions and applies the first live one behind
				// them, so the entry that was next is not always the entry that
				// moved. Reading the far end afterwards names the one that did -
				// what has just been undone is exactly what redo would put back.
				FString Applied = Queued;
				if (bUndo ? Trans->CanRedo() : Trans->CanUndo())
				{
					Applied = bUndo
						? Trans->GetRedoContext().Title.ToString()
						: Trans->GetUndoContext().Title.ToString();
				}
				Walked.Add(MakeShared<FJsonValueString>(Applied));
			}

			Data->SetArrayField(bUndo ? TEXT("undone") : TEXT("redone"), Walked);
			Describe(Data);

			const FString Busy = FString::Printf(
				TEXT("the editor refused to %s right now - it will not while a package is saving or garbage is being collected; try again"),
				*Action);
			if (Walked.Num() > 0)
			{
				return FUplinkToolResult::Ok(Data, bRefused
					? FString::Printf(TEXT("%s %d transaction(s), then %s"),
						bUndo ? TEXT("undid") : TEXT("redid"), Walked.Num(), *Busy)
					: FString::Printf(TEXT("%s %d transaction(s)"),
						bUndo ? TEXT("undid") : TEXT("redid"), Walked.Num()));
			}

			// CanUndo fills its reason text for an exhausted stack as well as a
			// blocked one, so the wording cannot tell "nothing left" from "not
			// right now". The queue positions can: undo is exhausted once every
			// entry has been walked, redo once none has.
			const bool bExhausted = bUndo
				? Trans->GetQueueLength() == Trans->GetUndoCount()
				: Trans->GetUndoCount() == 0;
			if (!bRefused && bExhausted)
			{
				return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("nothing to %s"), *Action));
			}

			// Asking to undo and having nothing happen because the buffer is
			// fenced or the editor is busy is a failed call, not a satisfied one:
			// answering ok is how a caller comes to believe an edit was rolled
			// back. The reason travels in undoBlocked/redoBlocked.
			FString Blocked;
			Data->TryGetStringField(FStringView(bUndo ? TEXT("undoBlocked") : TEXT("redoBlocked")), Blocked);
			FUplinkToolResult Result = FUplinkToolResult::Ok(Data, bRefused
				? Busy
				: FString::Printf(TEXT("could not %s: %s"), *Action, *Blocked));
			Result.bError = true;
			return Result;
		},
		// Walking the undo stack is not itself an undoable edit - wrapping it in
		// a transaction would fight the buffer it is driving. It is emphatically
		// not read-only either: this tool reverses the user's last change, and
		// readOnlyHint is what a client checks before auto-approving a call.
		/*bTransactional=*/false);

	Registry.RegisterQuick(
		TEXT("plugin_enable"),
		TEXT("Enable or disable a plugin for THIS project (writes the .uproject). Modules load and unload only on editor restart, and 'restart_required' says whether one is actually needed - it is false when the running editor already matched the state that was just written. Use plugin_list to find names."),
		TEXT(R"json({"type":"object","properties":{"name":{"type":"string"},"enable":{"type":"boolean","default":true}},"required":["name"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Name = GetString(Ctx.Params, TEXT("name"));
			bool bEnable = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("enable")), bEnable);

			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
			if (!Plugin.IsValid())
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("no plugin named '%s' (plugin_list to browse)"), *Name));
			}
			const bool bWasEnabled = Plugin->IsEnabled();

			FText FailReason;
			if (!IProjectManager::Get().SetPluginEnabled(Name, bEnable, FailReason))
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("could not %s '%s': %s"),
					bEnable ? TEXT("enable") : TEXT("disable"), *Name, *FailReason.ToString()));
			}
			if (!IProjectManager::Get().SaveCurrentProjectToDisk(FailReason))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT(".uproject save failed: %s. The change is set in memory only and is lost when the editor exits - check the file is writable (not read-only, not locked by source control) and call again."),
					*FailReason.ToString()));
			}

			// A restart only changes anything when the running editor does not
			// already match what was just written; promising one for a no-op
			// teaches the caller to restart for nothing.
			const bool bRestartRequired = bEnable != bWasEnabled;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("plugin"), Name);
			Data->SetBoolField(TEXT("enabled"), bEnable);
			Data->SetBoolField(TEXT("restart_required"), bRestartRequired);
			return FUplinkToolResult::Ok(Data, bRestartRequired
				? FString::Printf(TEXT("'%s' %s in the .uproject - restart the editor for it to take effect"),
					*Name, bEnable ? TEXT("enabled") : TEXT("disabled"))
				: FString::Printf(TEXT("'%s' was already %s in this editor and the .uproject now says so too - no restart needed"),
					*Name, bEnable ? TEXT("enabled") : TEXT("disabled")));
		});
}
