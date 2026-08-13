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
	 * or errors - the message says to check the Live Coding console/log.
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
			LiveCoding->Compile();
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
				: TEXT("compile finished without a patch (no changes, or compile errors - check the Live Coding console / output_log)"));
			return EUplinkToolStep::Done;
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
	};
#endif // WITH_LIVE_CODING
}

void UplinkTools::RegisterDev(FUplinkToolRegistry& Registry)
{
	FUplinkToolInfo Info;
	Info.Name = TEXT("live_compile");
	Info.Description = TEXT("Trigger a Live Coding compile and patch the RUNNING editor with changed C++ - no editor restart. Function-body edits apply in seconds; structural changes (new classes, members, virtuals) still need a real build. Resolves when the compiler goes idle: 'patched' true means new code is live.");
	Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"));
	Info.bReadOnly = false;
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
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("edit_history"),
		TEXT("Inspect and walk the editor's undo stack. Every mutating Uplink tool runs inside its own transaction named 'Uplink: <tool>', so an agent's edit can be undone exactly like a hand edit. 'action': 'list' (default) shows what undo/redo would do, 'undo' or 'redo' walk 'steps' entries. Does not apply to PIE-world edits, which the engine does not transact."),
		TEXT(R"json({"type":"object","properties":{"action":{"type":"string","enum":["list","undo","redo"],"default":"list"},"steps":{"type":"number","default":1,"description":"how many transactions to walk (undo/redo)"}}})json"),
		/*bReadOnly=*/true, // its own edits must not be wrapped in a transaction
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
			for (int32 i = 0; i < Steps; ++i)
			{
				FText Title;
				if (bUndo ? !Trans->CanUndo(&Title) : !Trans->CanRedo(&Title))
				{
					break;
				}
				const FString Name = bUndo
					? Trans->GetUndoContext().Title.ToString()
					: Trans->GetRedoContext().Title.ToString();
				const bool bOk = bUndo ? GEditor->UndoTransaction() : GEditor->RedoTransaction();
				if (!bOk)
				{
					break;
				}
				Walked.Add(MakeShared<FJsonValueString>(Name));
			}

			Data->SetArrayField(bUndo ? TEXT("undone") : TEXT("redone"), Walked);
			Describe(Data);
			return FUplinkToolResult::Ok(Data, Walked.Num() == 0
				? FString::Printf(TEXT("nothing to %s"), *Action)
				: FString::Printf(TEXT("%s %d transaction(s)"), bUndo ? TEXT("undid") : TEXT("redid"), Walked.Num()));
		});

	Registry.RegisterQuick(
		TEXT("plugin_enable"),
		TEXT("Enable or disable a plugin for THIS project (writes the .uproject). The editor must restart for the change to load/unload modules - the result says so. Use plugin_list to find names."),
		TEXT(R"json({"type":"object","properties":{"name":{"type":"string"},"enable":{"type":"boolean","default":true}},"required":["name"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Name = GetString(Ctx.Params, TEXT("name"));
			bool bEnable = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("enable")), bEnable);

			if (!IPluginManager::Get().FindPlugin(Name).IsValid())
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("no plugin named '%s' (plugin_list to browse)"), *Name));
			}
			FText FailReason;
			if (!IProjectManager::Get().SetPluginEnabled(Name, bEnable, FailReason))
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("could not %s '%s': %s"),
					bEnable ? TEXT("enable") : TEXT("disable"), *Name, *FailReason.ToString()));
			}
			if (!IProjectManager::Get().SaveCurrentProjectToDisk(FailReason))
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT(".uproject save failed: %s"), *FailReason.ToString()));
			}
			return FUplinkToolResult::Ok(nullptr, FString::Printf(
				TEXT("'%s' %s in the .uproject - restart the editor for it to take effect"),
				*Name, bEnable ? TEXT("enabled") : TEXT("disabled")));
		});
}
