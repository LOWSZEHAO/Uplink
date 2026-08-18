// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// run_scenario - execute a list of tool steps as one task and return a
// structured pass/fail report: a whole scripted playtest in a single call.

#include "UplinkTools.h"
#include "UplinkCompat.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Dom/JsonValue.h"

using namespace UplinkToolUtil;

namespace
{
	struct FStepSpec
	{
		FString Tool;
		TSharedPtr<FJsonObject> Params;
		TSharedPtr<FJsonObject> Expect;
		double TimeoutSeconds = 60.0;
		// The step is asserting a refusal: it passes when the tool errors, and
		// fails if the tool lets it through. Without this the only way to test
		// rejection was a whole scenario of nothing but rejections.
		bool bExpectFailure = false;
	};

	/**
	 * Which phase of a scenario a list of steps belongs to. Setup and teardown
	 * exist because of one recurring problem: every scenario in this repo ends
	 * with steps that delete what it made, and stop_on_failure meant those
	 * never ran on a failed run - the exact run that left debris behind. The
	 * next run then started dirty and its count assertions quietly meant
	 * something else.
	 */
	enum class EScenarioPhase : uint8
	{
		Setup,
		Main,
		Artifacts,
		Teardown,
		Complete,
	};

	const TCHAR* PhaseName(EScenarioPhase Phase)
	{
		switch (Phase)
		{
		case EScenarioPhase::Setup:     return TEXT("setup");
		case EScenarioPhase::Main:      return TEXT("main");
		case EScenarioPhase::Artifacts: return TEXT("artifacts");
		case EScenarioPhase::Teardown:  return TEXT("teardown");
		default:                        return TEXT("complete");
		}
	}

	/**
	 * "$steps[N].path.to.field" pulls a value out of an earlier step's result
	 * data, so a spawn's returned name can feed a later teleport or assert.
	 * "$setup[N]..." reaches into the setup phase the same way, which is how a
	 * fixture built in setup gets addressed by the steps that test it.
	 *
	 * $steps[] deliberately indexes the MAIN steps only. Numbering them from
	 * the start of the whole scenario would silently shift every existing
	 * scenario's references the moment a setup step was added.
	 */
	TSharedPtr<FJsonValue> ResolveStepReference(const FString& Token,
		const TArray<TSharedPtr<FJsonValue>>& Reports,
		const TArray<TSharedPtr<FJsonValue>>& SetupReports, FString& OutError)
	{
		const bool bSetup = Token.StartsWith(TEXT("$setup["));
		if (!bSetup && !Token.StartsWith(TEXT("$steps[")))
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>& Source = bSetup ? SetupReports : Reports;
		const int32 PrefixLen = bSetup ? 7 : 7;
		const TCHAR* Kind = bSetup ? TEXT("setup step") : TEXT("step");

		int32 CloseBracket = INDEX_NONE;
		if (!Token.FindChar(TEXT(']'), CloseBracket) || !Token.IsValidIndex(CloseBracket + 1) || Token[CloseBracket + 1] != TEXT('.'))
		{
			OutError = FString::Printf(TEXT("bad step reference '%s' (expected $steps[N].field.path)"), *Token);
			return nullptr;
		}
		const int32 StepIndex = FCString::Atoi(*Token.Mid(PrefixLen, CloseBracket - PrefixLen));
		if (!Source.IsValidIndex(StepIndex))
		{
			OutError = FString::Printf(TEXT("'%s' references %s %d, but only %d have run"), *Token, Kind, StepIndex, Source.Num());
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Report = nullptr;
		const TSharedPtr<FJsonObject>* StepData = nullptr;
		if (!Source[StepIndex]->TryGetObject(Report)
			|| !(*Report)->TryGetObjectField(FStringView(TEXT("data")), StepData) || !StepData->IsValid())
		{
			OutError = FString::Printf(TEXT("%s %d returned no data for '%s'"), Kind, StepIndex, *Token);
			return nullptr;
		}

		TSharedPtr<FJsonValue> Current = MakeShared<FJsonValueObject>(*StepData);
		TArray<FString> Segments;
		Token.Mid(CloseBracket + 2).ParseIntoArray(Segments, TEXT("."));
		for (const FString& Segment : Segments)
		{
			const TSharedPtr<FJsonObject>* AsObject = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* AsArray = nullptr;
			if (Current.IsValid() && Current->TryGetObject(AsObject))
			{
				Current = (*AsObject)->TryGetField(FStringView(Segment));
			}
			else if (Current.IsValid() && Current->TryGetArray(AsArray) && Segment.IsNumeric())
			{
				const int32 Index = FCString::Atoi(*Segment);
				Current = AsArray->IsValidIndex(Index) ? (*AsArray)[Index] : nullptr;
			}
			else
			{
				Current = nullptr;
			}
			if (!Current.IsValid())
			{
				OutError = FString::Printf(TEXT("'%s': field '%s' not found in step data"), *Token, *Segment);
				return nullptr;
			}
		}
		return Current;
	}

	/** Deep-copy a params JSON value, expanding every "$steps[...]" string. */
	TSharedPtr<FJsonValue> ExpandTemplates(const TSharedPtr<FJsonValue>& Value,
		const TArray<TSharedPtr<FJsonValue>>& Reports,
		const TArray<TSharedPtr<FJsonValue>>& SetupReports, FString& OutError)
	{
		if (!Value.IsValid() || !OutError.IsEmpty())
		{
			return Value;
		}
		FString AsString;
		if (Value->TryGetString(AsString)
			&& (AsString.StartsWith(TEXT("$steps[")) || AsString.StartsWith(TEXT("$setup["))))
		{
			return ResolveStepReference(AsString, Reports, SetupReports, OutError);
		}
		const TSharedPtr<FJsonObject>* AsObject = nullptr;
		if (Value->TryGetObject(AsObject))
		{
			TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
			for (const auto& Pair : (*AsObject)->Values)
			{
				Copy->SetField(UplinkCompat::JsonKeyToString(Pair.Key), ExpandTemplates(Pair.Value, Reports, SetupReports, OutError));
			}
			return MakeShared<FJsonValueObject>(Copy);
		}
		const TArray<TSharedPtr<FJsonValue>>* AsArray = nullptr;
		if (Value->TryGetArray(AsArray))
		{
			TArray<TSharedPtr<FJsonValue>> Copy;
			for (const TSharedPtr<FJsonValue>& Element : *AsArray)
			{
				Copy.Add(ExpandTemplates(Element, Reports, SetupReports, OutError));
			}
			return MakeShared<FJsonValueArray>(Copy);
		}
		return Value;
	}

	bool ValuesMatch(const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual)
	{
		if (!Expected.IsValid() || !Actual.IsValid())
		{
			return Expected.IsValid() == Actual.IsValid();
		}
		double ExpectedNumber = 0.0, ActualNumber = 0.0;
		if (Expected->TryGetNumber(ExpectedNumber) && Actual->TryGetNumber(ActualNumber))
		{
			return FMath::Abs(ExpectedNumber - ActualNumber) <= 0.0001;
		}
		return FJsonValue::CompareEqual(*Expected, *Actual);
	}

	class FScenarioInvocation final : public IUplinkInvocation
	{
	public:
		explicit FScenarioInvocation(FUplinkToolRegistry& InRegistry) : Registry(InRegistry) {}

		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			const TArray<TSharedPtr<FJsonValue>>* StepArray = nullptr;
			if (!Ctx.Params->TryGetArrayField(FStringView(TEXT("steps")), StepArray) || StepArray->Num() == 0)
			{
				Out = FUplinkToolResult::Error(TEXT("'steps' must be a non-empty array of {tool, params?, expect?, timeout?}"));
				return EUplinkToolStep::Done;
			}
			Ctx.Params->TryGetBoolField(FStringView(TEXT("stop_on_failure")), bStopOnFailure);

			FString ParseError;
			if (!ParsePhase(Ctx.Params, TEXT("setup"), SetupSteps, ParseError)
				|| !ParsePhase(Ctx.Params, TEXT("steps"), Steps, ParseError)
				|| !ParsePhase(Ctx.Params, TEXT("teardown"), TeardownSteps, ParseError))
			{
				Out = FUplinkToolResult::Error(ParseError);
				return EUplinkToolStep::Done;
			}

			const TSharedPtr<FJsonObject>* ArtifactsObject = nullptr;
			if (Ctx.Params->TryGetObjectField(FStringView(TEXT("artifacts")), ArtifactsObject)
				&& ArtifactsObject->IsValid())
			{
				(*ArtifactsObject)->TryGetBoolField(FStringView(TEXT("screenshot_on_failure")), bScreenshotOnFailure);
			}
			BudgetSeconds = GetNumber(Ctx.Params, TEXT("budget_seconds"), 0.0);

			Phase = SetupSteps.Num() > 0 ? EScenarioPhase::Setup : EScenarioPhase::Main;
			ScenarioStartedAt = FPlatformTime::Seconds();
			return EUplinkToolStep::Pending; // first step begins on the next tick
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			// Begin the next step if none is active.
			if (!Child.IsValid())
			{
				if (StepIndex >= CurrentSteps().Num())
				{
					return AdvancePhase(Out);
				}
				const FStepSpec& Spec = CurrentSteps()[StepIndex];
				const FUplinkToolDef* Def = Registry.Find(Spec.Tool);
				if (!Def)
				{
					RecordStep(FUplinkToolResult::Error(TEXT("tool disappeared mid-scenario")), 0.0);
					return AdvanceOrFinish(Out);
				}
				// Expand "$steps[N].field" references against earlier results.
				FString TemplateError;
				TSharedPtr<FJsonValue> Expanded = ExpandTemplates(
					MakeShared<FJsonValueObject>(Spec.Params.ToSharedRef()), StepReports, SetupReports, TemplateError);
				const TSharedPtr<FJsonObject>* ExpandedParams = nullptr;
				if (!TemplateError.IsEmpty()
					|| !Expanded.IsValid() || !Expanded->TryGetObject(ExpandedParams) || !ExpandedParams->IsValid())
				{
					RecordStep(FUplinkToolResult::Error(TemplateError.IsEmpty()
						? TEXT("parameter template expansion failed") : TemplateError), 0.0);
					return AdvanceOrFinish(Out);
				}

				Child = Def->Factory();
				bChildStarted = false;
				ChildContext.Params = *ExpandedParams;
				ChildContext.WorldSpec.Empty();
				ChildContext.Params->TryGetStringField(FStringView(TEXT("world")), ChildContext.WorldSpec);
				StepStartedAt = FPlatformTime::Seconds();
			}

			// Drive the active step.
			const FStepSpec& Spec = CurrentSteps()[StepIndex];
			FUplinkToolResult StepResult;
			EUplinkToolStep StepState;
			if (!bChildStarted)
			{
				bChildStarted = true;
				StepState = Child->Start(ChildContext, StepResult);
			}
			else
			{
				StepState = Child->Tick(ChildContext, StepResult);
			}

			const double Elapsed = FPlatformTime::Seconds() - StepStartedAt;
			if (StepState == EUplinkToolStep::Pending)
			{
				if (Elapsed > Spec.TimeoutSeconds)
				{
					Child.Reset();
					RecordStep(FUplinkToolResult::Error(FString::Printf(
						TEXT("step timed out after %.0fs"), Spec.TimeoutSeconds)), Elapsed);
					return AdvanceOrFinish(Out);
				}
				return EUplinkToolStep::Pending;
			}

			Child.Reset();
			RecordStep(MoveTemp(StepResult), Elapsed);
			return AdvanceOrFinish(Out);
		}

	private:
		/** Steps of the phase currently running. */
		TArray<FStepSpec>& CurrentSteps()
		{
			switch (Phase)
			{
			case EScenarioPhase::Setup:     return SetupSteps;
			case EScenarioPhase::Artifacts: return ArtifactSteps;
			case EScenarioPhase::Teardown:  return TeardownSteps;
			default:                        return Steps;
			}
		}

		/** Reports of the phase currently running. */
		TArray<TSharedPtr<FJsonValue>>& CurrentReports()
		{
			switch (Phase)
			{
			case EScenarioPhase::Setup:     return SetupReports;
			case EScenarioPhase::Artifacts: return ArtifactReports;
			case EScenarioPhase::Teardown:  return TeardownReports;
			default:                        return StepReports;
			}
		}

		bool ParsePhase(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
			TArray<FStepSpec>& OutSteps, FString& OutError)
		{
			const TArray<TSharedPtr<FJsonValue>>* StepArray = nullptr;
			if (!Params->TryGetArrayField(FStringView(Field), StepArray))
			{
				return true; // setup and teardown are optional
			}
			for (int32 Index = 0; Index < StepArray->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* StepObject = nullptr;
				if (!(*StepArray)[Index]->TryGetObject(StepObject) || !StepObject->IsValid())
				{
					OutError = FString::Printf(TEXT("%s step %d is not an object"), Field, Index);
					return false;
				}

				FStepSpec Spec;
				Spec.Tool = GetString(*StepObject, TEXT("tool"));
				if (Spec.Tool == TEXT("run_scenario"))
				{
					OutError = TEXT("scenarios cannot nest run_scenario");
					return false;
				}
				if (!Registry.Find(Spec.Tool))
				{
					OutError = FString::Printf(TEXT("%s step %d: unknown tool '%s'"), Field, Index, *Spec.Tool);
					return false;
				}

				const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
				Spec.Params = ((*StepObject)->TryGetObjectField(FStringView(TEXT("params")), ParamsObject) && ParamsObject->IsValid())
					? *ParamsObject : MakeShared<FJsonObject>();
				const TSharedPtr<FJsonObject>* ExpectObject = nullptr;
				if ((*StepObject)->TryGetObjectField(FStringView(TEXT("expect")), ExpectObject) && ExpectObject->IsValid())
				{
					Spec.Expect = *ExpectObject;
				}
				// Default to the TOOL's own declared budget, not a flat 60s.
				// pie_start asks for 900 because a real project recompiles a lot
				// of Blueprints on the way into play; capping that step at 60s
				// made the playtest scenario fail on every project big enough to
				// matter, while the identical call outside a scenario succeeded.
				double ToolBudget = 60.0;
				if (const FUplinkToolDef* ToolDef = Registry.Find(Spec.Tool))
				{
					ToolBudget = ToolDef->Info.TimeoutSeconds;
				}
				Spec.TimeoutSeconds = FMath::Clamp(GetNumber(*StepObject, TEXT("timeout"), ToolBudget), 0.5, 900.0);
				(*StepObject)->TryGetBoolField(FStringView(TEXT("expect_failure")), Spec.bExpectFailure);
				OutSteps.Add(MoveTemp(Spec));
			}
			return true;
		}

		/**
		 * Move to the next phase that has work. Teardown is reached from every
		 * path, including an aborted main phase - that is the whole point of
		 * having phases rather than one list.
		 */
		EUplinkToolStep AdvancePhase(FUplinkToolResult& Out)
		{
			for (;;)
			{
				switch (Phase)
				{
				case EScenarioPhase::Setup:
					// A failed fixture makes every assertion after it
					// meaningless - they would fail for the wrong reason and
					// send whoever reads the report chasing the wrong bug.
					Phase = SetupFailed > 0 ? EScenarioPhase::Teardown : EScenarioPhase::Main;
					break;

				case EScenarioPhase::Main:
					if (bScreenshotOnFailure && FailedSteps > 0)
					{
						FStepSpec Shot;
						Shot.Tool = TEXT("viewport_screenshot");
						Shot.Params = MakeShared<FJsonObject>();
						Shot.TimeoutSeconds = 30.0;
						ArtifactSteps.Add(MoveTemp(Shot));
					}
					Phase = EScenarioPhase::Artifacts;
					break;

				case EScenarioPhase::Artifacts:
					Phase = EScenarioPhase::Teardown;
					break;

				default:
					return Finish(Out);
				}

				StepIndex = 0;
				if (CurrentSteps().Num() > 0)
				{
					return EUplinkToolStep::Pending;
				}
			}
		}

		void RecordStep(FUplinkToolResult Result, double Elapsed)
		{
			const FStepSpec& Spec = CurrentSteps()[StepIndex];
			bool bSuccess = !Result.bError;
			FString FailReason = Result.bError ? Result.Message : FString();

			if (bSuccess && Spec.Expect.IsValid() && Result.Data.IsValid())
			{
				for (const auto& Pair : Spec.Expect->Values)
				{
					const FString Key = UplinkCompat::JsonKeyToString(Pair.Key);
					if (!ValuesMatch(Pair.Value, Result.Data->TryGetField(FStringView(Key))))
					{
						bSuccess = false;
						FailReason = FString::Printf(TEXT("expectation not met: %s"), *Key);
						break;
					}
				}
			}
			else if (bSuccess && Spec.Expect.IsValid() && !Result.Data.IsValid())
			{
				bSuccess = false;
				FailReason = TEXT("step returned no data to match expectations against");
			}

			// Convenience default: an un-expected wait_until that timed out is a
			// failed assertion, not a passed step.
			if (bSuccess && Spec.Tool == TEXT("wait_until") && !Spec.Expect.IsValid() && Result.Data.IsValid())
			{
				bool bConditionMet = false;
				Result.Data->TryGetBoolField(FStringView(TEXT("condition_met")), bConditionMet);
				if (!bConditionMet)
				{
					bSuccess = false;
					FailReason = TEXT("wait_until timed out (add expect:{condition_met:false} if intentional)");
				}
			}

			// Invert last, so an expected refusal that was in fact refused counts
			// as a pass and the report still carries what it refused with.
			if (Spec.bExpectFailure)
			{
				if (bSuccess)
				{
					bSuccess = false;
					FailReason = TEXT("expected this to be refused, but it succeeded");
				}
				else
				{
					bSuccess = true;
					FailReason = FString();
				}
			}

			TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
			Report->SetNumberField(TEXT("index"), StepIndex);
			Report->SetStringField(TEXT("phase"), PhaseName(Phase));
			Report->SetBoolField(TEXT("expected_failure"), Spec.bExpectFailure);
			Report->SetStringField(TEXT("tool"), Spec.Tool);
			Report->SetBoolField(TEXT("success"), bSuccess);
			Report->SetNumberField(TEXT("seconds"), Elapsed);
			if (!FailReason.IsEmpty())
			{
				Report->SetStringField(TEXT("fail_reason"), FailReason);
			}
			else if (!Result.Message.IsEmpty())
			{
				Report->SetStringField(TEXT("message"), Result.Message);
			}
			if (Result.Data.IsValid())
			{
				Report->SetObjectField(TEXT("data"), Result.Data);
			}
			if (Result.Png.Num() > 0)
			{
				// Keep image bytes out of the report; note the capture instead.
				Report->SetNumberField(TEXT("image_bytes"), static_cast<double>(Result.Png.Num()));
			}
			CurrentReports().Add(MakeShared<FJsonValueObject>(Report));

			if (!bSuccess)
			{
				switch (Phase)
				{
				case EScenarioPhase::Setup:     ++SetupFailed; break;
				case EScenarioPhase::Teardown:  ++TeardownFailed; break;
				case EScenarioPhase::Artifacts: break; // a missed capture is not a test result
				default:                        ++FailedSteps; break;
				}
				if (Phase == EScenarioPhase::Main && FirstFailedStep < 0)
				{
					FirstFailedStep = StepIndex;
				}
			}
		}

		EUplinkToolStep AdvanceOrFinish(FUplinkToolResult& Out)
		{
			// stop_on_failure ends the MAIN phase early. It does not end the
			// scenario: teardown still runs, because the run that failed is the
			// one most likely to have left something behind.
			if (Phase == EScenarioPhase::Main && FailedSteps > 0 && bStopOnFailure)
			{
				return AdvancePhase(Out);
			}
			// Setup always stops at the first failure. Carrying on building a
			// fixture whose first half did not work only buys cascading errors
			// that bury the one that mattered. Teardown deliberately does not
			// stop: every cleanup step should get its chance.
			if (Phase == EScenarioPhase::Setup && SetupFailed > 0)
			{
				return AdvancePhase(Out);
			}
			++StepIndex;
			if (StepIndex >= CurrentSteps().Num())
			{
				return AdvancePhase(Out);
			}
			return EUplinkToolStep::Pending;
		}

		EUplinkToolStep Finish(FUplinkToolResult& Out)
		{
			Phase = EScenarioPhase::Complete;
			const double Total = FPlatformTime::Seconds() - ScenarioStartedAt;

			// A scenario that suddenly takes three times as long still "passes"
			// every assertion, which is how a performance regression hides in a
			// green suite. An explicit budget makes that visible.
			const bool bOverBudget = BudgetSeconds > 0.0 && Total > BudgetSeconds;

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("passed"),
				FailedSteps == 0 && SetupFailed == 0 && TeardownFailed == 0 && !bOverBudget);
			Data->SetNumberField(TEXT("steps_run"), StepReports.Num());
			Data->SetNumberField(TEXT("steps_failed"), FailedSteps);
			if (FirstFailedStep >= 0)
			{
				Data->SetNumberField(TEXT("first_failed_step"), FirstFailedStep);
			}
			if (SetupReports.Num() > 0)
			{
				Data->SetArrayField(TEXT("setup"), SetupReports);
				Data->SetNumberField(TEXT("setup_failed"), SetupFailed);
			}
			if (TeardownReports.Num() > 0)
			{
				Data->SetArrayField(TEXT("teardown"), TeardownReports);
				Data->SetNumberField(TEXT("teardown_failed"), TeardownFailed);
			}
			if (ArtifactReports.Num() > 0)
			{
				Data->SetArrayField(TEXT("artifacts"), ArtifactReports);
			}
			Data->SetNumberField(TEXT("total_seconds"), Total);
			if (BudgetSeconds > 0.0)
			{
				Data->SetNumberField(TEXT("budget_seconds"), BudgetSeconds);
				Data->SetBoolField(TEXT("over_budget"), bOverBudget);
			}
			Data->SetArrayField(TEXT("steps"), StepReports);

			FString Summary;
			if (SetupFailed > 0)
			{
				Summary = TEXT("scenario FAILED in setup - the steps never ran");
			}
			else if (FailedSteps > 0)
			{
				Summary = TEXT("scenario FAILED");
			}
			else if (TeardownFailed > 0)
			{
				Summary = TEXT("steps passed but teardown FAILED - this run may have left something behind");
			}
			else if (bOverBudget)
			{
				Summary = FString::Printf(
					TEXT("steps passed but the scenario took %.1fs against a %.1fs budget"), Total, BudgetSeconds);
			}
			else
			{
				Summary = TEXT("scenario passed");
			}

			Out = FUplinkToolResult::Ok(Data, Summary);
			Out.bError = false; // a failed scenario is still a valid report
			return EUplinkToolStep::Done;
		}

		FUplinkToolRegistry& Registry;
		TArray<FStepSpec> SetupSteps;
		TArray<FStepSpec> Steps;
		TArray<FStepSpec> ArtifactSteps;
		TArray<FStepSpec> TeardownSteps;
		TArray<TSharedPtr<FJsonValue>> SetupReports;
		TArray<TSharedPtr<FJsonValue>> StepReports;
		TArray<TSharedPtr<FJsonValue>> ArtifactReports;
		TArray<TSharedPtr<FJsonValue>> TeardownReports;
		TSharedPtr<IUplinkInvocation> Child;
		FUplinkToolContext ChildContext;
		EScenarioPhase Phase = EScenarioPhase::Main;
		bool bChildStarted = false;
		bool bStopOnFailure = true;
		bool bScreenshotOnFailure = false;
		int32 StepIndex = 0;
		int32 FailedSteps = 0;
		int32 SetupFailed = 0;
		int32 TeardownFailed = 0;
		int32 FirstFailedStep = -1;
		double StepStartedAt = 0.0;
		double ScenarioStartedAt = 0.0;
		double BudgetSeconds = 0.0;
	};
}

void UplinkTools::RegisterScenario(FUplinkToolRegistry& Registry)
{
	FUplinkToolInfo Info;
	Info.Name = TEXT("run_scenario");
	Info.Description = TEXT("Run a scripted playtest: a list of tool steps executed in order as one task, returning a structured pass/fail report with per-step timing. Each step is {tool, params?, expect?, timeout?}. Param strings of the form \"$steps[N].field.path\" are replaced with values from step N's result data (e.g. spawn an actor in step 0, then teleport to \"$steps[0].location\"). 'expect' matches fields of the step's result data; a wait_until step without an expect fails the scenario if its condition times out. stop_on_failure (default true) aborts at the first failed step. Each step inherits the timeout its own tool declares unless overridden. A step with expect_failure:true is asserting a refusal - it passes when the tool errors, which is how a scenario mixes normal steps with rejection tests. Optional phases: 'setup' builds the fixture and, if any setup step fails, the steps are skipped entirely rather than failing for the wrong reason (reference setup results as \"$setup[N].field\"); 'teardown' ALWAYS runs, including after an aborted run, which is when cleanup matters most. artifacts:{screenshot_on_failure:true} captures the viewport when a step fails. budget_seconds fails a scenario that passed every assertion but took longer than it should - a performance regression otherwise hides in a green suite.");
	Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"steps":{"type":"array","items":{"type":"object","properties":{"tool":{"type":"string"},"params":{"type":"object"},"expect":{"type":"object","description":"Result-data fields that must match"},"expect_failure":{"type":"boolean","default":false,"description":"This step asserts a refusal: it passes when the tool errors and fails when it succeeds"},"timeout":{"type":"number","description":"Seconds for this step; defaults to the tool's own declared timeout - pie_start asks for 900"}},"required":["tool"]}},"setup":{"type":"array","description":"Fixture steps run first; if one fails the main steps are skipped","items":{"type":"object","properties":{"tool":{"type":"string"},"params":{"type":"object"},"expect":{"type":"object"},"expect_failure":{"type":"boolean"},"timeout":{"type":"number"}},"required":["tool"]}},"teardown":{"type":"array","description":"Cleanup steps that always run, including after an aborted run","items":{"type":"object","properties":{"tool":{"type":"string"},"params":{"type":"object"},"expect":{"type":"object"},"expect_failure":{"type":"boolean"},"timeout":{"type":"number"}},"required":["tool"]}},"artifacts":{"type":"object","properties":{"screenshot_on_failure":{"type":"boolean","default":false}}},"budget_seconds":{"type":"number","description":"Fail the scenario if the whole run takes longer than this"},"stop_on_failure":{"type":"boolean","default":true}},"required":["steps"]})json"));
	Info.bReadOnly = false;
		Info.bTransactional = false; // drives the session, not an undoable edit
	Info.TimeoutSeconds = 600.0;

	Registry.Register(MoveTemp(Info), [&Registry]() -> TSharedRef<IUplinkInvocation>
	{
		return MakeShared<FScenarioInvocation>(Registry);
	});
}
