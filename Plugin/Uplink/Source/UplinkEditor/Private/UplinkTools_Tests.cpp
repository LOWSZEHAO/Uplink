// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// run_tests - run the engine/project automation tests matching a filter and
// return a structured pass/fail report per test.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Misc/AutomationTest.h"

using namespace UplinkToolUtil;

namespace
{
	/** Errors kept per failing test; the true count is reported alongside. */
	constexpr int32 GMaxErrorsPerTest = 5;

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
			if (bFilterWidened)
			{
				// Back to how the framework constructs itself. There is no
				// getter for the previous value in 5.7 or 5.8, so the default is
				// the only thing that can honestly be restored - and the tool
				// refuses to start while another run is in flight, so there is
				// no concurrent owner to clobber.
				FAutomationTestFramework::Get().SetRequestedTestFilter(EAutomationTestFlags::SmokeFilter);
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

			// GetValidTestNames returns only tests whose filter flag is in the
			// framework's RequestedTestFilter, and the framework constructs that
			// as SmokeFilter alone (AutomationTest.cpp, both 5.7 and 5.8). Every
			// test declares exactly one filter type, so without widening this
			// nothing outside the smoke set is visible at all: a filter naming a
			// real engine test came back "no tests match", against a registered
			// count that was itself only the smoke subset. Wrong answer, wrong
			// number, and no mention of the filter that caused either.
			Framework.SetRequestedTestFilter(EAutomationTestFlags_FilterMask);
			bFilterWidened = true;

			TArray<FAutomationTestInfo> AllTests;
			Framework.GetValidTestNames(AllTests);
			for (const FAutomationTestInfo& Test : AllTests)
			{
				if (Test.GetDisplayName().Contains(Filter, ESearchCase::IgnoreCase)
					|| Test.GetTestName().Contains(Filter, ESearchCase::IgnoreCase))
				{
					// Keep counting past the cap: "ran 20" over a filter that hit
					// two hundred tests is a different result from a clean run,
					// and stopping the count hides which one happened.
					++Matched;
					if (Pending.Num() < MaxTests)
					{
						Pending.Add(Test.GetTestName());
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
						if (Entry.Event.Type == EAutomationEventType::Error && Errors.Num() < GMaxErrorsPerTest)
						{
							Errors.Add(MakeShared<FJsonValueString>(Entry.Event.Message));
						}
					}
					Row->SetArrayField(TEXT("errors"), Errors);

					// A test that fails a hundred assertions shows five of them;
					// without this the caller reads five and thinks that is all
					// of them.
					Row->SetNumberField(TEXT("error_count"), ExecutionInfo.GetErrorTotal());
					++Failed;
				}
				Results.Add(MakeShared<FJsonValueObject>(Row));
				Current.Reset();
			}
			else if (!Current.IsEmpty())
			{
				// The test we started is no longer in flight and we did not stop
				// it - something else did. Dropping the row silently would leave
				// a test the caller asked about missing from the report with no
				// hint that it ever ran.
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("test"), Current);
				Row->SetBoolField(TEXT("passed"), false);
				TArray<TSharedPtr<FJsonValue>> Errors;
				Errors.Add(MakeShared<FJsonValueString>(
					TEXT("stopped outside this tool before a result could be read - something else drove the automation framework during the run")));
				Row->SetArrayField(TEXT("errors"), Errors);
				Results.Add(MakeShared<FJsonValueObject>(Row));
				++Failed;
				Current.Reset();
			}

			if (Pending.Num() > 0 && StartUntilRunning())
			{
				return EUplinkToolStep::Pending;
			}
			return Finish(Out);
		}

		/**
		 * Stop the test still in flight before letting go of the invocation.
		 *
		 * The framework is process-wide: an abandoned test leaves
		 * GIsAutomationTesting set, so every later run_tests refuses with "an
		 * automation test is already running outside this tool", GWarn stays
		 * hijacked by the test message filter, and the queued latent commands
		 * keep running against the editor after the caller has been told the
		 * task is over.
		 */
		virtual void Cancel(EUplinkCancelReason Reason) override
		{
			Pending.Reset();
			Current.Reset();
			if (GIsAutomationTesting)
			{
				FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

				// Same order the framework uses when it stops a test to start
				// another: drop the queue first, so nothing is left to run
				// against whatever test comes next.
				Framework.DequeueAllCommands();
				FAutomationTestExecutionInfo Abandoned;
				Framework.StopTest(Abandoned);
			}
			if (bOwnsRun)
			{
				bOwnsRun = false;
				bRunActive = false;
			}
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
				Current.Reset();
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
			Data->SetNumberField(TEXT("matched"), Matched);
			Data->SetBoolField(TEXT("truncated"), Matched > Results.Num());
			FUplinkToolResult Result = FUplinkToolResult::Ok(Data,
				Failed == 0 ? TEXT("all tests passed") : FString::Printf(TEXT("%d test(s) FAILED"), Failed));
			Result.bError = Failed > 0;
			Out = Result;
			return EUplinkToolStep::Done;
		}

		TArray<FString> Pending;
		TArray<TSharedPtr<FJsonValue>> Results;
		FString Current;
		int32 Matched = 0;
		int32 Failed = 0;
		bool bOwnsRun = false;
		bool bFilterWidened = false;
	};
}

void UplinkTools::RegisterTests(FUplinkToolRegistry& Registry)
{
	FUplinkToolInfo Info;
	Info.Name = TEXT("run_tests");
	Info.Description = TEXT("Run engine/project automation tests whose name contains 'filter' and report per-test pass/fail with errors and durations. Runs up to 'max' matches sequentially; 'matched' says how many the filter really hit and 'truncated' whether the cap left some unrun. Only one run_tests at a time. A run with any failure comes back as an ERROR carrying the full report, so a failing suite cannot read as a successful call; 'ran' counts every test reported on, including ones that never started, which count as failures. Each failing test lists at most five errors, with 'error_count' saying how many there were. Be specific with the filter - some editor tests open maps or take minutes.");
	Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"filter":{"type":"string"},"max":{"type":"number","default":20}},"required":["filter"]})json"));
	Info.bReadOnly = false;
	// Twenty minutes is far too long to hold the undo buffer open, and a test
	// run is not an edit the user would ever want to undo.
	Info.bTransactional = false;
	Info.TimeoutSeconds = 1200.0;
	Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
	{
		return MakeShared<FRunTestsInvocation>();
	});
}
