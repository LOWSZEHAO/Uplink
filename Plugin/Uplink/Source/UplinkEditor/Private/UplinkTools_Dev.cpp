// Copyright (c) 2026 Low Sze Hao. MIT License.
// Developer-loop tools: live_compile - trigger a Live Coding patch of the
// running editor so C++ iteration doesn't need an editor restart.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Modules/ModuleManager.h"

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
}
