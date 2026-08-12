// Copyright (c) 2026 Low Sze Hao. MIT License.
// run_tests - run the engine/project automation tests matching a filter and
// return a structured pass/fail report per test.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Misc/AutomationTest.h"

using namespace UplinkToolUtil;

namespace
{
	/**
	 * Runs matched tests one at a time, ticking latent commands between frames.
	 * The framework is unforgiving global state: StartTestByName silently
	 * no-ops (unknown name, GIsSlowTask, or PIE active), and StopTest /
	 * ExecuteLatentCommands check() GIsAutomationTesting - so every call here
	 * is gated on whether a test is genuinely in flight.
	 */
	class FRunTestsInvocation : public IUplinkInvocation
	{
	public:
		// Two interleaved runners would trip the framework's global state.
		static inline bool bRunActive = false;

		virtual ~FRunTestsInvocation() override
		{
			if (bOwnsRun)
			{
				bRunActive = false;
			}
		}

		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			if (bRunActive)
			{
				Out = FUplinkToolResult::Error(TEXT("another run_tests is still executing - wait for it (task_list) before starting a new one"));
				return EUplinkToolStep::Done;
			}
			const FString Filter = GetString(Ctx.Params, TEXT("filter"));
			if (Filter.Len() < 3)
			{
				Out = FUplinkToolResult::Error(TEXT("'filter' (min 3 chars) is required - it substring-matches test names; be specific, some editor tests open maps or take minutes"));
				return EUplinkToolStep::Done;
			}
			const int32 MaxTests = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 20)), 1, 200);

			FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
			if (GIsAutomationTesting)
			{
				Out = FUplinkToolResult::Error(TEXT("an automation test is already running outside this tool"));
				return EUplinkToolStep::Done;
			}

			TArray<FAutomationTestInfo> AllTests;
			Framework.GetValidTestNames(AllTests);
			for (const FAutomationTestInfo& Test : AllTests)
			{
				if (Test.GetDisplayName().Contains(Filter, ESearchCase::IgnoreCase)
					|| Test.GetTestName().Contains(Filter, ESearchCase::IgnoreCase))
				{
					Pending.Add(Test.GetTestName());
					if (Pending.Num() >= MaxTests)
					{
						break;
					}
				}
			}
			if (Pending.Num() == 0)
			{
				Out = FUplinkToolResult::Error(FString::Printf(
					TEXT("no tests match '%s' (%d tests registered)"), *Filter, AllTests.Num()));
				return EUplinkToolStep::Done;
			}

			bRunActive = true;
			bOwnsRun = true;
			if (!StartUntilRunning())
			{
				return Finish(Out);
			}
			return EUplinkToolStep::Pending;
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
			if (GIsAutomationTesting)
			{
				if (!Framework.ExecuteLatentCommands())
				{
					return EUplinkToolStep::Pending;
				}
				FAutomationTestExecutionInfo ExecutionInfo;
				Framework.StopTest(ExecutionInfo);

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("test"), Current);
				Row->SetBoolField(TEXT("passed"), ExecutionInfo.bSuccessful);
				Row->SetNumberField(TEXT("seconds"), ExecutionInfo.Duration);
				if (!ExecutionInfo.bSuccessful)
				{
					TArray<TSharedPtr<FJsonValue>> Errors;
					for (const FAutomationExecutionEntry& Entry : ExecutionInfo.GetEntries())
					{
						if (Entry.Event.Type == EAutomationEventType::Error && Errors.Num() < 5)
						{
							Errors.Add(MakeShared<FJsonValueString>(Entry.Event.Message));
						}
					}
					Row->SetArrayField(TEXT("errors"), Errors);
					++Failed;
				}
				Results.Add(MakeShared<FJsonValueObject>(Row));
			}

			if (Pending.Num() > 0 && StartUntilRunning())
			{
				return EUplinkToolStep::Pending;
			}
			return Finish(Out);
		}

	private:
		/** Starts tests until one is genuinely in flight; records non-starters. */
		bool StartUntilRunning()
		{
			while (Pending.Num() > 0)
			{
				Current = Pending[0];
				Pending.RemoveAt(0);
				FAutomationTestFramework::Get().StartTestByName(Current, /*RoleIndex=*/0);
				if (GIsAutomationTesting)
				{
					return true;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("test"), Current);
				Row->SetBoolField(TEXT("passed"), false);
				TArray<TSharedPtr<FJsonValue>> Errors;
				Errors.Add(MakeShared<FJsonValueString>(
					TEXT("did not start - unknown test name, a slow task in progress, or PIE active (see LogAutomationTest in output_log)")));
				Row->SetArrayField(TEXT("errors"), Errors);
				Results.Add(MakeShared<FJsonValueObject>(Row));
				++Failed;
			}
			return false;
		}

		EUplinkToolStep Finish(FUplinkToolResult& Out)
		{
			bRunActive = false;
			bOwnsRun = false;

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("results"), Results);
			Data->SetNumberField(TEXT("ran"), Results.Num());
			Data->SetNumberField(TEXT("failed"), Failed);
			FUplinkToolResult Result = FUplinkToolResult::Ok(Data,
				Failed == 0 ? TEXT("all tests passed") : FString::Printf(TEXT("%d test(s) FAILED"), Failed));
			Result.bError = Failed > 0;
			Out = Result;
			return EUplinkToolStep::Done;
		}

		TArray<FString> Pending;
		TArray<TSharedPtr<FJsonValue>> Results;
		FString Current;
		int32 Failed = 0;
		bool bOwnsRun = false;
	};
}

void UplinkTools::RegisterTests(FUplinkToolRegistry& Registry)
{
	FUplinkToolInfo Info;
	Info.Name = TEXT("run_tests");
	Info.Description = TEXT("Run engine/project automation tests whose name contains 'filter' and report per-test pass/fail with errors and durations. Runs up to 'max' matches sequentially; only one run_tests at a time. Be specific with the filter - some editor tests open maps or take minutes.");
	Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"filter":{"type":"string"},"max":{"type":"number","default":20}},"required":["filter"]})json"));
	Info.bReadOnly = false;
	Info.TimeoutSeconds = 1200.0;
	Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
	{
		return MakeShared<FRunTestsInvocation>();
	});
}
