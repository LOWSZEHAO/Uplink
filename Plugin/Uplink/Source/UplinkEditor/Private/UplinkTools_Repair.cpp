// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
//
// Blueprint repair at the C++ boundary.
//
// When C++ changes - a renamed variable, a changed signature, a removed function
// - every Blueprint referencing it breaks, and the engine's remedy is
// right-click Refresh Node, one node at a time, across every affected asset. A
// real project in this state had 17 broken Blueprints and roughly 200 error
// lines that all traced back to three API changes. Finding that by reading the
// list is the wrong job for a person; applying the same mechanical fix to every
// asset is the wrong job too.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

using namespace UplinkToolUtil;

namespace
{
	IAssetRegistry& AssetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	/** Compile and collect the error text, without touching the result shape. */
	struct FCompileOutcome
	{
		int32 Errors = 0;
		int32 Warnings = 0;
		TArray<FString> ErrorTexts;
	};

	FCompileOutcome CompileAndCollect(UBlueprint* Blueprint)
	{
		FCompileOutcome Outcome;
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);
		Outcome.Errors = Results.NumErrors;
		Outcome.Warnings = Results.NumWarnings;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			if (Message->GetSeverity() == EMessageSeverity::Error)
			{
				Outcome.ErrorTexts.Add(Message->ToText().ToString());
			}
		}
		return Outcome;
	}

	/**
	 * Reduce an error to the thing that actually broke.
	 *
	 * Two hundred error lines across a dozen assets usually come from two or
	 * three API changes; the message names the culprit but buries it in prose
	 * that differs per pin. Pulling that name out turns an unreadable list into
	 * "these twelve assets are broken by one renamed node".
	 *
	 * OutOwner receives the owning class when the message named one, which only
	 * the function-not-found wording does. The group key deliberately does not
	 * include it - see the merge below - so the owner is reported beside the
	 * group instead, where two of them means the group is really two changes.
	 */
	FString CauseOf(const FString& ErrorText, FString* OutOwner = nullptr)
	{
		auto Between = [&ErrorText](const TCHAR* Prefix, const TCHAR* Suffix) -> FString
		{
			const FString PrefixString(Prefix);
			const int32 Start = ErrorText.Find(PrefixString, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			if (Start == INDEX_NONE)
			{
				return FString();
			}
			const FString Rest = ErrorText.Mid(Start + PrefixString.Len());
			const FString SuffixString(Suffix);
			const int32 End = SuffixString.IsEmpty()
				? INDEX_NONE
				: Rest.Find(SuffixString, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			return (End == INDEX_NONE ? Rest : Rest.Left(End)).TrimStartAndEnd();
		};

		// A single missing function produces both "Could not find a function
		// named X" and "In use pin ... no longer exists on node X" - the same
		// root cause, worded twice, and the node title is spaced where the
		// function name is not ("Save Bool" vs "SaveBool"). Normalise so they
		// land in one group instead of doubling the apparent cause count.
		//
		// The label says "node" rather than "function", because the orphan-pin
		// wording is what the engine emits for ANY pin that went away: a struct
		// member, an enum entry, a macro's pin, a delegate signature. Calling
		// all of those a missing function named the wrong culprit on assets
		// where no function had changed at all.
		auto Canonical = [](const FString& Name)
		{
			return FString::Printf(TEXT("node no longer matches its definition: %s"),
				*Name.Replace(TEXT(" "), TEXT("")));
		};
		if (FString Node = Between(TEXT("no longer exists on node"), TEXT(".")); !Node.IsEmpty())
		{
			return Canonical(Node);
		}
		if (FString Function = Between(TEXT("Could not find a function named \""), TEXT("\"")); !Function.IsEmpty())
		{
			if (OutOwner)
			{
				*OutOwner = Between(TEXT("\" in '"), TEXT("'"));
			}
			return Canonical(Function);
		}
		// "The property associated with X could not be found in 'Y'"
		if (FString Property = Between(TEXT("The property associated with"), TEXT("could not be found"));
			!Property.IsEmpty())
		{
			return FString::Printf(TEXT("property no longer exists: %s"), *Property);
		}
		// Anything else: collapse to a bounded prefix so it still groups.
		return ErrorText.Left(90).TrimStartAndEnd();
	}

	/**
	 * Blueprint assets under a path, sorted by package name.
	 *
	 * The order matters because both tools here cap how many they touch: a
	 * capped run takes the first N in this order, so sorting is what makes two
	 * runs of the same call cover the same assets. It also means a capped run
	 * is an alphabetical slice, not a sample - which is why the reply says how
	 * many were left out.
	 */
	void GatherBlueprints(const FString& PathPrefix, TArray<FAssetData>& Out)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Blueprint")));
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(FName(*PathPrefix));
		Filter.bRecursivePaths = true;
		AssetRegistry().GetAssets(Filter, Out);
		Out.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.PackageName.LexicalLess(B.PackageName);
		});
	}
}

void UplinkTools::RegisterRepair(FUplinkToolRegistry& Registry)
{
	// ------------------------------------------------------------------
	// bp_find_broken
	// ------------------------------------------------------------------
	Registry.RegisterQuick(
		TEXT("bp_find_broken"),
		TEXT("Find Blueprints that do not compile, GROUPED BY WHAT BROKE THEM. This is the tool for a project that stopped working after a C++ change: one renamed function can break a dozen assets and produce hundreds of error lines that all say the same thing. The grouping is the point - it turns an unreadable list into 'these twelve assets are broken by this one node'. Compiling is real work, so it stops after 'max' assets, taking them in package-name order rather than sampling; 'checked', 'totalBlueprints', 'notChecked', 'truncated' and 'failedToLoad' say how much of the project the answer covers, and a capped run is never a whole-project verdict. Pair with bp_repair, which fixes the mechanical cases."),
		TEXT(R"json({"type":"object","properties":{"path_prefix":{"type":"string","default":"/Game"},"max":{"type":"number","default":120,"description":"How many Blueprints to compile-check, in package-name order. The rest are reported as 'notChecked', not as clean."},"max_assets_per_cause":{"type":"number","default":12,"description":"How many asset names to list per cause; 'assetCount' is always the full number"},"include_warnings":{"type":"boolean","default":false,"description":"Also report how many checked Blueprints compiled with warnings, as 'withWarnings'. Warning text itself is never returned - only errors are listed."}}})json"),
		// Not read-only: it loads and compiles every Blueprint it checks. A client
		// deciding what to auto-approve should be told that is real work.
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString PathPrefix = GetString(Ctx.Params, TEXT("path_prefix"), TEXT("/Game"));
			// Each one is a synchronous package load plus a real compile, and the
			// whole sweep runs inside a single tool call, so the default has to be
			// a number the editor can chew through without looking hung. The
			// description tells you to raise it deliberately; 300 did not match
			// that advice.
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 120.0)), 1, 5000);
			const int32 MaxPerCause = FMath::Clamp(
				static_cast<int32>(GetNumber(Ctx.Params, TEXT("max_assets_per_cause"), 12.0)), 1, 200);
			bool bIncludeWarnings = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("include_warnings")), bIncludeWarnings);

			TArray<FAssetData> Assets;
			GatherBlueprints(PathPrefix, Assets);

			// cause -> assets, preserving first-seen order for stable output
			TMap<FString, TArray<FString>> ByCause;
			TMap<FString, TArray<FString>> OwnersByCause;
			TArray<FString> CauseOrder;
			TArray<TSharedPtr<FJsonValue>> BrokenRows;
			TArray<TSharedPtr<FJsonValue>> FailedToLoad;

			int32 Checked = 0;
			int32 Broken = 0;
			int32 WithWarnings = 0;
			// Every asset the loop consumed, loadable or not. "Checked" alone
			// cannot answer "did we reach the end?", because it does not count
			// the ones that failed to load.
			int32 Considered = 0;

			for (const FAssetData& Asset : Assets)
			{
				if (Checked >= Max)
				{
					break;
				}
				++Considered;
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
				if (!Blueprint)
				{
					// An asset that will not even load is the most broken thing
					// in the project; skipping it silently made the sweep look
					// cleaner than the project it was reporting on.
					FailedToLoad.Add(MakeShared<FJsonValueString>(Asset.PackageName.ToString()));
					continue;
				}
				++Checked;

				const FCompileOutcome Outcome = CompileAndCollect(Blueprint);
				if (Outcome.Warnings > 0)
				{
					++WithWarnings;
				}
				if (Outcome.Errors == 0)
				{
					continue;
				}
				++Broken;

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("blueprint"), Asset.PackageName.ToString());
				Row->SetNumberField(TEXT("errors"), Outcome.Errors);
				Row->SetStringField(TEXT("firstError"),
					Outcome.ErrorTexts.Num() ? Outcome.ErrorTexts[0].Left(160) : FString());
				BrokenRows.Add(MakeShared<FJsonValueObject>(Row));

				// One asset can hit several distinct causes; count each once.
				TSet<FString> SeenHere;
				for (const FString& ErrorText : Outcome.ErrorTexts)
				{
					FString Owner;
					const FString Cause = CauseOf(ErrorText, &Owner);
					if (!Owner.IsEmpty())
					{
						OwnersByCause.FindOrAdd(Cause).AddUnique(Owner);
					}
					if (SeenHere.Contains(Cause))
					{
						continue;
					}
					SeenHere.Add(Cause);
					if (!ByCause.Contains(Cause))
					{
						CauseOrder.Add(Cause);
					}
					ByCause.FindOrAdd(Cause).Add(Asset.PackageName.ToString());
				}
			}

			// Most-damaging cause first: that is where a fix pays best.
			CauseOrder.Sort([&ByCause](const FString& A, const FString& B)
			{
				return ByCause[A].Num() > ByCause[B].Num();
			});

			TArray<TSharedPtr<FJsonValue>> Causes;
			for (const FString& Cause : CauseOrder)
			{
				const TArray<FString>& Affected = ByCause[Cause];
				TArray<TSharedPtr<FJsonValue>> AssetList;
				for (const FString& Name : Affected)
				{
					if (AssetList.Num() >= MaxPerCause)
					{
						break;
					}
					AssetList.Add(MakeShared<FJsonValueString>(Name));
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("cause"), Cause);
				Row->SetNumberField(TEXT("assetCount"), Affected.Num());
				Row->SetArrayField(TEXT("assets"), AssetList);
				// The key has to stay class-free or the two wordings the engine
				// uses for one missing function stop merging - which means two
				// same-named members on unrelated classes land in one group and
				// read as one change. Naming the owners the messages did carry
				// makes that visible: more than one owner here is more than one
				// root cause.
				if (const TArray<FString>* Owners = OwnersByCause.Find(Cause))
				{
					TArray<TSharedPtr<FJsonValue>> OwnerList;
					for (const FString& Owner : *Owners)
					{
						OwnerList.Add(MakeShared<FJsonValueString>(Owner));
					}
					Row->SetArrayField(TEXT("owners"), OwnerList);
				}
				Causes.Add(MakeShared<FJsonValueObject>(Row));
			}

			// Assets the loop never reached, as opposed to ones it reached and
			// could not load - those are named in failedToLoad and are a
			// different problem.
			const int32 NotChecked = Assets.Num() - Considered;

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("causes"), Causes);
			Data->SetArrayField(TEXT("broken"), BrokenRows);
			Data->SetNumberField(TEXT("checked"), Checked);
			Data->SetNumberField(TEXT("brokenCount"), Broken);
			Data->SetNumberField(TEXT("totalBlueprints"), Assets.Num());
			Data->SetBoolField(TEXT("truncated"), NotChecked > 0);
			Data->SetNumberField(TEXT("notChecked"), NotChecked);
			if (FailedToLoad.Num() > 0)
			{
				Data->SetArrayField(TEXT("failedToLoad"), FailedToLoad);
			}
			if (bIncludeWarnings)
			{
				Data->SetNumberField(TEXT("withWarnings"), WithWarnings);
			}

			// The summary is the sentence a caller acts on, and "all 120
			// Blueprints compiled" after checking 120 of 400 is the worst thing
			// this tool could say: it reads as a clean project. The cap has to
			// be in the sentence, not only in a field beside it.
			FString Summary;
			if (Assets.Num() == 0)
			{
				// A typo'd prefix and a healthy project both end here with
				// nothing compiled, and only one of them is good news.
				Summary = FString::Printf(
					TEXT("no Blueprints found under '%s' - nothing was compiled, so this is not a clean bill of health; pass a path_prefix that exists, e.g. /Game"),
					*PathPrefix);
			}
			else if (Broken == 0)
			{
				Summary = NotChecked > 0
					? FString::Printf(TEXT("the %d Blueprints checked all compiled"), Checked)
					: FString::Printf(TEXT("all %d Blueprints compiled"), Checked);
			}
			else
			{
				Summary = FString::Printf(
					TEXT("%d of %d Blueprints broken, from %d distinct cause(s) - bp_repair fixes the mechanical ones"),
					Broken, Checked, Causes.Num());
			}
			if (NotChecked > 0)
			{
				Summary += FString::Printf(
					TEXT(" - NOT a whole-project answer: %d of %d Blueprints under %s were never checked, raise 'max' above %d to cover them"),
					NotChecked, Assets.Num(), *PathPrefix, Max);
			}
			if (FailedToLoad.Num() > 0)
			{
				Summary += FString::Printf(
					TEXT("; %d asset(s) would not load at all and could not be compiled - see failedToLoad"),
					FailedToLoad.Num());
			}
			return FUplinkToolResult::Ok(Data, Summary);
		});

	// ------------------------------------------------------------------
	// bp_repair
	// ------------------------------------------------------------------
	Registry.RegisterQuick(
		TEXT("bp_repair"),
		TEXT("Apply the editor's own repair to Blueprints broken by a C++ change: refresh every node against its current signature and replace deprecated nodes, then recompile and report what healed. This is the bulk version of right-click Refresh Node, which is the actual fix for a stale pin or a node whose function changed shape. It cannot invent a function that no longer exists - those come back in 'stillBroken' with the reason, and need a real decision. Runs on named blueprints, or on everything still broken under 'path_prefix' - that sweep stops at 'max' targets and says so in 'truncated', and anything it could not open lands in 'failedToLoad' rather than counting as clean, so a capped run is never a whole-project repair. Nothing is written to disk unless 'save' is set, a blueprint that still fails is never saved, and 'saved' counts only what actually reached disk."),
		TEXT(R"json({"type":"object","properties":{"blueprints":{"type":"array","items":{"type":"string"},"description":"Specific assets to repair; omit to repair everything broken under path_prefix"},"path_prefix":{"type":"string","default":"/Game"},"max":{"type":"number","default":100,"description":"How many broken blueprints the path_prefix sweep will take, in package-name order. Ignored when 'blueprints' names them - a named list is repaired in full."},"save":{"type":"boolean","default":false,"description":"Write repaired blueprints to disk. Only those that compile clean are saved."},"dry_run":{"type":"boolean","default":false,"description":"List what would be refreshed, without refreshing or saving. Not free when 'blueprints' is omitted: finding the broken set still loads and compiles every Blueprint the path sweep examines."}}})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 100.0)), 1, 2000);
			bool bSave = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("save")), bSave);
			bool bDryRun = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("dry_run")), bDryRun);

			// Either an explicit list, or everything currently broken.
			TArray<UBlueprint*> Targets;
			const FString PathPrefix = GetString(Ctx.Params, TEXT("path_prefix"), TEXT("/Game"));
			bool bSweptPath = false;
			int32 Scanned = 0;
			int32 TotalBlueprints = 0;
			bool bStoppedAtMax = false;
			TArray<TSharedPtr<FJsonValue>> SweepFailedToLoad;
			const TArray<TSharedPtr<FJsonValue>>* Named = nullptr;
			if (Ctx.Params->TryGetArrayField(FStringView(TEXT("blueprints")), Named) && Named->Num() > 0)
			{
				for (const TSharedPtr<FJsonValue>& Entry : *Named)
				{
					FString Path;
					if (!Entry.IsValid() || !Entry->TryGetString(Path))
					{
						continue;
					}
					UObject* Loaded = StaticFindObject(UBlueprint::StaticClass(), nullptr, *Path);
					if (!Loaded)
					{
						Loaded = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *Path);
					}
					if (UBlueprint* Blueprint = Cast<UBlueprint>(Loaded))
					{
						Targets.Add(Blueprint);
					}
					else
					{
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("no Blueprint at '%s' (a package path like /Game/Folder/BP_Thing works)"), *Path));
					}
				}
			}
			else
			{
				bSweptPath = true;
				TArray<FAssetData> Assets;
				GatherBlueprints(PathPrefix, Assets);
				TotalBlueprints = Assets.Num();
				for (const FAssetData& Asset : Assets)
				{
					if (Targets.Num() >= Max)
					{
						// Stopping here leaves the rest of the project unlooked
						// at, not proven clean, and the reply has to carry that
						// or "repaired all 100" reads as "the project is fixed".
						bStoppedAtMax = true;
						break;
					}
					++Scanned;
					UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
					if (!Blueprint)
					{
						// Nothing was compiled for it, so it is not proven clean.
						// Dropping it here is what let "nothing broken to repair"
						// come back from a sweep that could not open a thing.
						SweepFailedToLoad.Add(MakeShared<FJsonValueString>(Asset.PackageName.ToString()));
						continue;
					}
					if (CompileAndCollect(Blueprint).Errors > 0)
					{
						Targets.Add(Blueprint);
					}
				}
			}

			// Assets the sweep opened and could not read are not proven clean
			// either, so every reply below has to carry them - "nothing broken
			// to repair" most of all.
			FString FailedToLoadClause;
			if (SweepFailedToLoad.Num() > 0)
			{
				FailedToLoadClause = FString::Printf(
					TEXT("; %d asset(s) under %s would not load and were never checked - see failedToLoad"),
					SweepFailedToLoad.Num(), *PathPrefix);
			}

			if (Targets.Num() == 0)
			{
				TSharedRef<FJsonObject> None = MakeShared<FJsonObject>();
				None->SetNumberField(TEXT("attempted"), 0);
				if (bSweptPath)
				{
					None->SetNumberField(TEXT("checked"), Scanned);
					None->SetNumberField(TEXT("totalBlueprints"), TotalBlueprints);
					None->SetBoolField(TEXT("truncated"), false);
					if (SweepFailedToLoad.Num() > 0)
					{
						None->SetArrayField(TEXT("failedToLoad"), SweepFailedToLoad);
					}
				}
				// A path that matched nothing and a path where everything
				// compiles produce the same empty target list, and only one of
				// them means there is nothing wrong.
				FString NoneSummary = (bSweptPath && TotalBlueprints == 0)
					? FString::Printf(
						TEXT("no Blueprints found under '%s' - nothing was checked; pass a path_prefix that exists, e.g. /Game"),
						*PathPrefix)
					: FString(TEXT("nothing broken to repair"));
				NoneSummary += FailedToLoadClause;
				return FUplinkToolResult::Ok(None, NoneSummary);
			}

			if (bDryRun)
			{
				TArray<TSharedPtr<FJsonValue>> Would;
				for (const UBlueprint* Blueprint : Targets)
				{
					Would.Add(MakeShared<FJsonValueString>(Blueprint->GetOutermost()->GetName()));
				}
				TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
				Plan->SetArrayField(TEXT("wouldRepair"), Would);
				Plan->SetNumberField(TEXT("attempted"), Would.Num());
				FString PlanSummary = FString::Printf(
					TEXT("%d blueprint(s) would be refreshed and recompiled"), Would.Num());
				if (bSweptPath)
				{
					Plan->SetNumberField(TEXT("checked"), Scanned);
					Plan->SetNumberField(TEXT("totalBlueprints"), TotalBlueprints);
					Plan->SetBoolField(TEXT("truncated"), bStoppedAtMax);
					if (SweepFailedToLoad.Num() > 0)
					{
						Plan->SetArrayField(TEXT("failedToLoad"), SweepFailedToLoad);
					}
					if (bStoppedAtMax)
					{
						// A plan is what the caller decides on, so it has to say
						// that the list is a cap and not the whole damage.
						PlanSummary += FString::Printf(
							TEXT(" - the sweep stopped at max=%d after %d of %d Blueprints under %s, so this plan is not the whole project"),
							Max, Scanned, TotalBlueprints, *PathPrefix);
					}
					PlanSummary += FailedToLoadClause;
				}
				return FUplinkToolResult::Ok(Plan, PlanSummary);
			}

			TArray<TSharedPtr<FJsonValue>> Healed;
			TArray<TSharedPtr<FJsonValue>> StillBroken;
			TArray<TSharedPtr<FJsonValue>> AlreadyClean;
			TArray<UPackage*> ToSave;

			for (UBlueprint* Blueprint : Targets)
			{
				const FCompileOutcome Before = CompileAndCollect(Blueprint);

				// Naming an asset that was never broken should say so, not
				// claim a repair that did not happen.
				if (Before.Errors == 0)
				{
					AlreadyClean.Add(MakeShared<FJsonValueString>(Blueprint->GetOutermost()->GetName()));
					continue;
				}

				// The editor's own repair, in the order it applies them.
				FBlueprintEditorUtils::ReplaceDeprecatedNodes(Blueprint);
				FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				const FCompileOutcome After = CompileAndCollect(Blueprint);

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("blueprint"), Blueprint->GetOutermost()->GetName());
				Row->SetNumberField(TEXT("errorsBefore"), Before.Errors);
				Row->SetNumberField(TEXT("errorsAfter"), After.Errors);

				if (After.Errors == 0)
				{
					Healed.Add(MakeShared<FJsonValueObject>(Row));
					if (bSave)
					{
						ToSave.Add(Blueprint->GetOutermost());
					}
				}
				else
				{
					// Say why it is still broken: refreshing cannot bring back
					// a function that no longer exists.
					Row->SetStringField(TEXT("reason"),
						After.ErrorTexts.Num() ? CauseOf(After.ErrorTexts[0]) : TEXT("unknown"));
					Row->SetStringField(TEXT("firstError"),
						After.ErrorTexts.Num() ? After.ErrorTexts[0].Left(160) : FString());
					StillBroken.Add(MakeShared<FJsonValueObject>(Row));
				}
			}

			int32 Saved = 0;
			TArray<TSharedPtr<FJsonValue>> NotSaved;
			if (bSave && ToSave.Num() > 0)
			{
				// PromptForCheckoutAndSave takes an unattended shortcut that
				// answers all-or-nothing and never touches OutFailedPackages,
				// so counting ToSave minus Failed reported every package as
				// written even when a read-only file or a missing checkout
				// meant none of them reached disk. The only per-package
				// evidence left is the dirty flag: the engine clears it on the
				// packages it actually wrote.
				TArray<UPackage*> Failed;
				const bool bAllWritten = SavePackagesUnattended(ToSave, /*bCheckDirty=*/false, &Failed);
				for (UPackage* Package : ToSave)
				{
					const bool bWritten = !Failed.Contains(Package)
						&& (bAllWritten || !Package->IsDirty());
					if (bWritten)
					{
						++Saved;
					}
					else
					{
						NotSaved.Add(MakeShared<FJsonValueString>(Package->GetName()));
					}
				}
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("healed"), Healed);
			Data->SetArrayField(TEXT("stillBroken"), StillBroken);
			Data->SetArrayField(TEXT("alreadyClean"), AlreadyClean);
			Data->SetNumberField(TEXT("attempted"), Targets.Num());
			Data->SetNumberField(TEXT("healedCount"), Healed.Num());
			Data->SetNumberField(TEXT("stillBrokenCount"), StillBroken.Num());
			if (bSave)
			{
				Data->SetNumberField(TEXT("saved"), Saved);
				if (NotSaved.Num() > 0)
				{
					Data->SetArrayField(TEXT("notSaved"), NotSaved);
				}
			}
			if (bSweptPath)
			{
				Data->SetNumberField(TEXT("checked"), Scanned);
				Data->SetNumberField(TEXT("totalBlueprints"), TotalBlueprints);
				Data->SetBoolField(TEXT("truncated"), bStoppedAtMax);
				if (SweepFailedToLoad.Num() > 0)
				{
					Data->SetArrayField(TEXT("failedToLoad"), SweepFailedToLoad);
				}
			}

			const int32 WasBroken = Healed.Num() + StillBroken.Num();
			FString Summary;
			if (WasBroken == 0)
			{
				Summary = FString::Printf(TEXT("nothing was broken (%d already clean)"), AlreadyClean.Num());
			}
			else if (StillBroken.Num() == 0)
			{
				Summary = FString::Printf(TEXT("repaired all %d"), Healed.Num());
			}
			else
			{
				Summary = FString::Printf(
					TEXT("repaired %d of %d broken; %d need a real decision (see stillBroken - refreshing cannot restore a function that no longer exists)"),
					Healed.Num(), WasBroken, StillBroken.Num());
			}
			if (bStoppedAtMax)
			{
				Summary += FString::Printf(
					TEXT(" - the sweep stopped at max=%d after %d of %d Blueprints under %s, so the rest of the project was never examined"),
					Max, Scanned, TotalBlueprints, *PathPrefix);
			}
			Summary += FailedToLoadClause;
			if (NotSaved.Num() > 0)
			{
				Summary += FString::Printf(
					TEXT("; %d repaired asset(s) did NOT reach disk (see notSaved - check the files are writable and checked out), so the fix is in memory only"),
					NotSaved.Num());
			}
			return FUplinkToolResult::Ok(Data, Summary);
		});
}
