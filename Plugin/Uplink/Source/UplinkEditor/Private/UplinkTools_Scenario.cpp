// Copyright (c) 2026 Low Sze Hao. MIT License.
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
	};

	/**
	 * "$steps[N].path.to.field" pulls a value out of an earlier step's result
	 * data, so a spawn's returned name can feed a later teleport or assert.
	 */
	TSharedPtr<FJsonValue> ResolveStepReference(const FString& Token,
		const TArray<TSharedPtr<FJsonValue>>& Reports, FString& OutError)
	{
		if (!Token.StartsWith(TEXT("$steps[")))
		{
			return nullptr;
		}
		int32 CloseBracket = INDEX_NONE;
		if (!Token.FindChar(TEXT(']'), CloseBracket) || !Token.IsValidIndex(CloseBracket + 1) || Token[CloseBracket + 1] != TEXT('.'))
		{
			OutError = FString::Printf(TEXT("bad step reference '%s' (expected $steps[N].field.path)"), *Token);
			return nullptr;
		}
		const int32 StepIndex = FCString::Atoi(*Token.Mid(7, CloseBracket - 7));
		if (!Reports.IsValidIndex(StepIndex))
		{
			OutError = FString::Printf(TEXT("'%s' references step %d, but only %d step(s) have run"), *Token, StepIndex, Reports.Num());
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Report = nullptr;
		const TSharedPtr<FJsonObject>* StepData = nullptr;
		if (!Reports[StepIndex]->TryGetObject(Report)
			|| !(*Report)->TryGetObjectField(FStringView(TEXT("data")), StepData) || !StepData->IsValid())
		{
			OutError = FString::Printf(TEXT("step %d returned no data for '%s'"), StepIndex, *Token);
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
		const TArray<TSharedPtr<FJsonValue>>& Reports, FString& OutError)
	{
		if (!Value.IsValid() || !OutError.IsEmpty())
		{
			return Value;
		}
		FString AsString;
		if (Value->TryGetString(AsString) && AsString.StartsWith(TEXT("$steps[")))
		{
			return ResolveStepReference(AsString, Reports, OutError);
		}
		const TSharedPtr<FJsonObject>* AsObject = nullptr;
		if (Value->TryGetObject(AsObject))
		{
			TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
			for (const auto& Pair : (*AsObject)->Values)
			{
				Copy->SetField(UplinkCompat::JsonKeyToString(Pair.Key), ExpandTemplates(Pair.Value, Reports, OutError));
			}
			return MakeShared<FJsonValueObject>(Copy);
		}
		const TArray<TSharedPtr<FJsonValue>>* AsArray = nullptr;
		if (Value->TryGetArray(AsArray))
		{
			TArray<TSharedPtr<FJsonValue>> Copy;
			for (const TSharedPtr<FJsonValue>& Element : *AsArray)
			{
				Copy.Add(ExpandTemplates(Element, Reports, OutError));
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

			for (int32 Index = 0; Index < StepArray->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* StepObject = nullptr;
				if (!(*StepArray)[Index]->TryGetObject(StepObject) || !StepObject->IsValid())
				{
					Out = FUplinkToolResult::Error(FString::Printf(TEXT("step %d is not an object"), Index));
					return EUplinkToolStep::Done;
				}

				FStepSpec Spec;
				Spec.Tool = GetString(*StepObject, TEXT("tool"));
				if (Spec.Tool == TEXT("run_scenario"))
				{
					Out = FUplinkToolResult::Error(TEXT("scenarios cannot nest run_scenario"));
					return EUplinkToolStep::Done;
				}
				if (!Registry.Find(Spec.Tool))
				{
					Out = FUplinkToolResult::Error(FString::Printf(TEXT("step %d: unknown tool '%s'"), Index, *Spec.Tool));
					return EUplinkToolStep::Done;
				}

				const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
				Spec.Params = ((*StepObject)->TryGetObjectField(FStringView(TEXT("params")), ParamsObject) && ParamsObject->IsValid())
					? *ParamsObject : MakeShared<FJsonObject>();
				const TSharedPtr<FJsonObject>* ExpectObject = nullptr;
				if ((*StepObject)->TryGetObjectField(FStringView(TEXT("expect")), ExpectObject) && ExpectObject->IsValid())
				{
					Spec.Expect = *ExpectObject;
				}
				Spec.TimeoutSeconds = FMath::Clamp(GetNumber(*StepObject, TEXT("timeout"), 60.0), 0.5, 300.0);
				Steps.Add(MoveTemp(Spec));
			}

			ScenarioStartedAt = FPlatformTime::Seconds();
			return EUplinkToolStep::Pending; // first step begins on the next tick
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			// Begin the next step if none is active.
			if (!Child.IsValid())
			{
				if (StepIndex >= Steps.Num())
				{
					return Finish(Out);
				}
				const FStepSpec& Spec = Steps[StepIndex];
				const FUplinkToolDef* Def = Registry.Find(Spec.Tool);
				if (!Def)
				{
					RecordStep(FUplinkToolResult::Error(TEXT("tool disappeared mid-scenario")), 0.0);
					return AdvanceOrFinish(Out);
				}
				// Expand "$steps[N].field" references against earlier results.
				FString TemplateError;
				TSharedPtr<FJsonValue> Expanded = ExpandTemplates(
					MakeShared<FJsonValueObject>(Spec.Params.ToSharedRef()), StepReports, TemplateError);
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
			const FStepSpec& Spec = Steps[StepIndex];
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
		void RecordStep(FUplinkToolResult Result, double Elapsed)
		{
			const FStepSpec& Spec = Steps[StepIndex];
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

			TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
			Report->SetNumberField(TEXT("index"), StepIndex);
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
			StepReports.Add(MakeShared<FJsonValueObject>(Report));

			if (!bSuccess)
			{
				++FailedSteps;
				if (FirstFailedStep < 0)
				{
					FirstFailedStep = StepIndex;
				}
			}
		}

		EUplinkToolStep AdvanceOrFinish(FUplinkToolResult& Out)
		{
			if (FailedSteps > 0 && bStopOnFailure)
			{
				return Finish(Out);
			}
			++StepIndex;
			if (StepIndex >= Steps.Num())
			{
				return Finish(Out);
			}
			return EUplinkToolStep::Pending;
		}

		EUplinkToolStep Finish(FUplinkToolResult& Out)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("passed"), FailedSteps == 0);
			Data->SetNumberField(TEXT("steps_run"), StepReports.Num());
			Data->SetNumberField(TEXT("steps_failed"), FailedSteps);
			if (FirstFailedStep >= 0)
			{
				Data->SetNumberField(TEXT("first_failed_step"), FirstFailedStep);
			}
			Data->SetNumberField(TEXT("total_seconds"), FPlatformTime::Seconds() - ScenarioStartedAt);
			Data->SetArrayField(TEXT("steps"), StepReports);
			Out = FUplinkToolResult::Ok(Data,
				FailedSteps == 0 ? TEXT("scenario passed") : TEXT("scenario FAILED"));
			Out.bError = false; // a failed scenario is still a valid report
			return EUplinkToolStep::Done;
		}

		FUplinkToolRegistry& Registry;
		TArray<FStepSpec> Steps;
		TArray<TSharedPtr<FJsonValue>> StepReports;
		TSharedPtr<IUplinkInvocation> Child;
		FUplinkToolContext ChildContext;
		bool bChildStarted = false;
		bool bStopOnFailure = true;
		int32 StepIndex = 0;
		int32 FailedSteps = 0;
		int32 FirstFailedStep = -1;
		double StepStartedAt = 0.0;
		double ScenarioStartedAt = 0.0;
	};
}

void UplinkTools::RegisterScenario(FUplinkToolRegistry& Registry)
{
	FUplinkToolInfo Info;
	Info.Name = TEXT("run_scenario");
	Info.Description = TEXT("Run a scripted playtest: a list of tool steps executed in order as one task, returning a structured pass/fail report with per-step timing. Each step is {tool, params?, expect?, timeout?}. Param strings of the form \"$steps[N].field.path\" are replaced with values from step N's result data (e.g. spawn an actor in step 0, then teleport to \"$steps[0].location\"). 'expect' matches fields of the step's result data; a wait_until step without an expect fails the scenario if its condition times out. stop_on_failure (default true) aborts at the first failed step.");
	Info.InputSchema = FUplinkToolRegistry::ParseSchema(TEXT(R"json({"type":"object","properties":{"steps":{"type":"array","items":{"type":"object","properties":{"tool":{"type":"string"},"params":{"type":"object"},"expect":{"type":"object","description":"Result-data fields that must match"},"timeout":{"type":"number","default":60}},"required":["tool"]}},"stop_on_failure":{"type":"boolean","default":true}},"required":["steps"]})json"));
	Info.bReadOnly = false;
	Info.TimeoutSeconds = 600.0;

	Registry.Register(MoveTemp(Info), [&Registry]() -> TSharedRef<IUplinkInvocation>
	{
		return MakeShared<FScenarioInvocation>(Registry);
	});
}
