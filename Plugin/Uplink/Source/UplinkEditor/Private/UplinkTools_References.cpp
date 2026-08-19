// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// bp_references - who uses this. The counterpart to bp_find_broken: that one
// says what a C++ change already broke, this one says what a change WOULD break,
// before it is made.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Algo/Find.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_Event.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableSet.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

using namespace UplinkToolUtil;

namespace
{
	IAssetRegistry& ReferencesAssetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	/** Same spec forms asset_create accepts; duplicated because that one is file-local. */
	UClass* ResolveClassSpecLocal(const FString& Spec)
	{
		if (Spec.IsEmpty())
		{
			return nullptr;
		}
		if (Spec.StartsWith(TEXT("/")))
		{
			return StaticLoadClass(UObject::StaticClass(), nullptr, *Spec);
		}
		if (UClass* Found = FindFirstObject<UClass>(*Spec, EFindFirstObjectOptions::NativeFirst))
		{
			return Found;
		}
		if (Spec.Len() > 1 && (Spec[0] == TEXT('U') || Spec[0] == TEXT('A')))
		{
			return FindFirstObject<UClass>(*Spec.RightChop(1), EFindFirstObjectOptions::NativeFirst);
		}
		return nullptr;
	}

	/**
	 * The asset registry deliberately does not record hard dependencies on these
	 * four - "every package in the game depends on them" - so asking it who
	 * references /Script/Engine comes back empty. That empty answer would read as
	 * "nothing calls SetActorLocation", which is the worst possible lie for this
	 * tool to tell, so we detect it and fall back to scanning by path instead.
	 */
	bool IsUnboundedPackage(FName Package)
	{
		static const FName Unbounded[] = {
			FName(TEXT("/Script/CoreUObject")), FName(TEXT("/Script/Engine")),
			FName(TEXT("/Script/BlueprintGraph")), FName(TEXT("/Script/UnrealEd"))
		};
		return Algo::Find(Unbounded, Package) != nullptr;
	}

	/**
	 * A path_prefix names a folder, not a set of characters.
	 *
	 * A plain StartsWith puts /Game/UIOld inside a search scoped to /Game/UI,
	 * so hits came back from folders the caller had deliberately excluded and
	 * the reply still called the search complete.
	 */
	bool PackageIsUnder(const FString& PackageName, const FString& PathPrefix)
	{
		// A bare root is every package, and appending a separator to it would
		// ask for "//" - which matches nothing, so the search would come back
		// empty and still call itself complete.
		if (PathPrefix.IsEmpty() || PathPrefix == TEXT("/"))
		{
			return true;
		}
		return PackageName == PathPrefix || PackageName.StartsWith(PathPrefix + TEXT("/"));
	}

	/**
	 * Drop candidates that hold no Blueprint at all.
	 *
	 * The bounded list is package referencers, which is mostly levels, data
	 * assets and materials - things LoadObject<UBlueprint> was always going to
	 * return null for. Separating those from a Blueprint that genuinely would
	 * not open is what lets a failed load be reported instead of swallowed.
	 */
	void KeepBlueprintPackages(TArray<FName>& Candidates)
	{
		if (Candidates.Num() == 0)
		{
			return;
		}
		FARFilter Filter;
		Filter.PackageNames = Candidates;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		TArray<FAssetData> Assets;
		ReferencesAssetRegistry().GetAssets(Filter, Assets);

		TSet<FName> Keep;
		Keep.Reserve(Assets.Num());
		for (const FAssetData& Asset : Assets)
		{
			Keep.Add(Asset.PackageName);
		}
		Candidates.RemoveAll([&Keep](const FName& Package) { return !Keep.Contains(Package); });
	}
}

void UplinkTools::RegisterReferences(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("bp_references"),
		TEXT("Who uses this - the question to ask BEFORE changing something, where bp_find_broken answers it afterwards. For a function, variable, event dispatcher or whole Blueprint, reports every Blueprint containing a node that calls, reads, writes, binds or overrides it, down to the graph and node guid (feed those straight to bp_modify). Already-loaded Blueprints are scanned for free; unloaded ones are narrowed by the asset registry and only 'max_load' of them are opened, because loading is real work. Anything it did not scan is named in the reply and 'complete' says whether the search was exhaustive, so a capped answer is never mistaken for 'nothing uses this'. Blind spots it cannot see: an override implemented as a function GRAPH rather than an event (only event overrides are found), AnimGraph property access, UMG property bindings, and object values sitting in pin defaults."),
		TEXT(R"json({"type":"object","properties":{"kind":{"type":"string","enum":["blueprint","function","variable","dispatcher"],"description":"What the name refers to"},"name":{"type":"string","description":"Member name, or the package path (/Game/BP_Door) when kind=blueprint"},"class":{"type":"string","description":"Owning class: /Script/Module.Class or /Game/BP_X.BP_X_C. Omit to match on name alone - broader, and every hit reports the owner it found"},"path_prefix":{"type":"string","default":"/Game"},"max_load":{"type":"number","default":25,"description":"How many unloaded candidates to open and scan"},"loaded_only":{"type":"boolean","default":false,"description":"Scan only what is already in memory - instant, and incomplete"}},"required":["kind","name"]})json"),
		// Not read-only: it loads packages, which is real work a client should be
		// told about before it auto-approves the call. Same reasoning as
		// bp_find_broken. Nothing here is an undoable edit, so no transaction.
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Kind = GetString(Ctx.Params, TEXT("kind"));
			const FString Name = GetString(Ctx.Params, TEXT("name"));
			if (Name.IsEmpty())
			{
				return FUplinkToolResult::Error(TEXT("'name' is required"));
			}
			const FName TargetName(*Name);
			// A trailing slash would make every "under this folder" test below
			// look for a doubled separator and match nothing.
			FString PathPrefix = GetString(Ctx.Params, TEXT("path_prefix"), TEXT("/Game"));
			while (PathPrefix.Len() > 1 && PathPrefix.EndsWith(TEXT("/")))
			{
				PathPrefix.LeftChopInline(1);
			}
			const int32 MaxLoad = FMath::Clamp(
				static_cast<int32>(GetNumber(Ctx.Params, TEXT("max_load"), 25.0)), 0, 500);
			bool bLoadedOnly = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("loaded_only")), bLoadedOnly);

			const FString ClassSpec = GetString(Ctx.Params, TEXT("class"));
			UClass* TargetClass = ClassSpec.IsEmpty() ? nullptr : ResolveClassSpecLocal(ClassSpec);
			if (!ClassSpec.IsEmpty() && !TargetClass)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("class not found: '%s'. A Blueprint's class needs the _C suffix, e.g. /Game/BP_Door.BP_Door_C"),
					*ClassSpec));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

			// kind=blueprint is a package question first: who references the asset
			// at all, including levels and data assets that hold no graphs.
			FName BoundPackage = NAME_None;
			if (Kind == TEXT("blueprint"))
			{
				FString PackageName = Name;
				int32 Dot;
				if (PackageName.FindChar(TEXT('.'), Dot))
				{
					PackageName.LeftInline(Dot);
				}
				BoundPackage = FName(*PackageName);
				TArray<FName> Referencers;
				ReferencesAssetRegistry().GetReferencers(BoundPackage, Referencers,
					UE::AssetRegistry::EDependencyCategory::Package);
				TArray<TSharedPtr<FJsonValue>> RefRows;
				for (const FName& Ref : Referencers)
				{
					RefRows.Add(MakeShared<FJsonValueString>(Ref.ToString()));
				}
				Data->SetArrayField(TEXT("package_referencers"), RefRows);
				if (!TargetClass)
				{
					TargetClass = LoadObject<UClass>(nullptr, *(PackageName + TEXT(".") +
						FPackageName::GetShortName(PackageName) + TEXT("_C")));
				}
				if (!TargetClass)
				{
					// kind=blueprint matches on class alone - the name is never
					// compared - so with no class to compare against, the scan
					// below marks EVERY call, variable and delegate node it
					// walks as a hit. A path that resolves to nothing came back
					// as "the entire project references this".
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no Blueprint class at '%s' - either it is not a Blueprint, or its class would not load. kind=blueprint wants a Blueprint's package path (e.g. /Game/BP_Door); ask asset_referencers for the package-level answer, which works for any asset"),
						*PackageName));
				}
			}
			else if (TargetClass)
			{
				BoundPackage = TargetClass->GetOutermost()->GetFName();
			}

			// Can the registry narrow the search, or must we sweep the path?
			const bool bBounded = !BoundPackage.IsNone() && !IsUnboundedPackage(BoundPackage);

			// Everything already in memory is free to scan.
			TSet<UBlueprint*> ToScan;
			for (TObjectIterator<UBlueprint> It; It; ++It)
			{
				UBlueprint* Bp = *It;
				if (Bp && !Bp->HasAnyFlags(RF_Transient)
					&& PackageIsUnder(Bp->GetOutermost()->GetName(), PathPrefix))
				{
					ToScan.Add(Bp);
				}
			}
			const int32 FreeCount = ToScan.Num();

			// Then the ones worth opening, narrowed by the registry when it can.
			TArray<FString> NotScanned;
			TArray<FString> LoadFailed;
			int32 Loaded = 0;
			if (!bLoadedOnly)
			{
				TArray<FName> Candidates;
				if (bBounded)
				{
					ReferencesAssetRegistry().GetReferencers(BoundPackage, Candidates,
						UE::AssetRegistry::EDependencyCategory::Package);
					// Referencers are packages of every kind; the sweep below
					// asks the registry for Blueprints and needs no such pass.
					KeepBlueprintPackages(Candidates);
				}
				else
				{
					FARFilter Filter;
					Filter.PackagePaths.Add(FName(*PathPrefix));
					Filter.bRecursivePaths = true;
					Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
					// Widget and Animation Blueprints derive from UBlueprint, so
					// without this the path sweep quietly excluded every WBP and
					// ABP in the project while still reporting itself complete.
					Filter.bRecursiveClasses = true;
					TArray<FAssetData> Assets;
					ReferencesAssetRegistry().GetAssets(Filter, Assets);
					for (const FAssetData& Asset : Assets)
					{
						Candidates.Add(Asset.PackageName);
					}
				}

				for (const FName& PackageName : Candidates)
				{
					const FString PackageString = PackageName.ToString();
					if (!PackageIsUnder(PackageString, PathPrefix) || PackageString.StartsWith(TEXT("/Script")))
					{
						continue;
					}
					if (FindObject<UBlueprint>(nullptr, *(PackageString + TEXT(".") +
						FPackageName::GetShortName(PackageString))))
					{
						continue; // already counted as free
					}
					if (Loaded >= MaxLoad)
					{
						NotScanned.Add(PackageString);
						continue;
					}
					if (UBlueprint* Bp = LoadObject<UBlueprint>(nullptr, *(PackageString + TEXT(".") +
						FPackageName::GetShortName(PackageString))))
					{
						ToScan.Add(Bp);
						++Loaded;
					}
					else
					{
						// The registry says this package holds a Blueprint and
						// it would not open. Dropping that silently let a search
						// that never looked at the asset still call itself
						// complete.
						LoadFailed.Add(PackageString);
					}
				}
			}

			// A blueprint has two live classes - SKEL_Foo_C and Foo_C - and they are
			// SIBLINGS, not parent and child, so IsChildOf fails in both directions
			// between them. Collapse either one onto the blueprint's real generated
			// class before comparing, or a variable used in its own graph (the
			// commonest case of "what uses this?") reports zero hits.
			auto Authoritative = [](UClass* Class) -> UClass*
			{
				if (!Class)
				{
					return nullptr;
				}
				if (UBlueprint* OwningBp = UBlueprint::GetBlueprintFromClass(Class))
				{
					if (OwningBp->GeneratedClass)
					{
						return OwningBp->GeneratedClass.Get();
					}
				}
				return Class;
			};
			UClass* const TargetAuth = Authoritative(TargetClass);

			// Does a member reference point at the target? A self-context member
			// carries no class, so resolve it against the blueprint being scanned.
			auto ClassMatches = [TargetAuth, &Authoritative](UClass* MemberParent, UBlueprint* ScannedBp) -> bool
			{
				if (!TargetAuth)
				{
					return true;
				}
				if (!MemberParent)
				{
					UClass* SelfClass = Authoritative(ScannedBp->SkeletonGeneratedClass
						? ScannedBp->SkeletonGeneratedClass.Get()
						: ScannedBp->GeneratedClass.Get());
					return SelfClass && SelfClass->IsChildOf(TargetAuth);
				}
				// Tolerate SKEL_/redirector drift in both directions; the row
				// reports the owner it actually found either way.
				UClass* const ParentAuth = Authoritative(MemberParent);
				return ParentAuth->IsChildOf(TargetAuth) || TargetAuth->IsChildOf(ParentAuth);
			};

			TArray<TSharedPtr<FJsonValue>> Hits;
			TSet<FString> HitAssets;
			for (UBlueprint* Bp : ToScan)
			{
				TArray<UEdGraph*> Graphs;
				Bp->GetAllGraphs(Graphs);
				for (UEdGraph* Graph : Graphs)
				{
					if (!Graph)
					{
						continue;
					}
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (!Node)
						{
							continue;
						}
						FString HitKind;
						// The schema promises that omitting 'class' still tells you
						// which class each hit actually landed on, so every branch
						// below has to hand back the owner it matched.
						UClass* HitOwner = nullptr;

						if (Kind == TEXT("function") || Kind == TEXT("blueprint"))
						{
							if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
							{
								const bool bNameOk = (Kind == TEXT("blueprint"))
									|| Call->FunctionReference.GetMemberName() == TargetName;
								if (bNameOk && ClassMatches(Call->FunctionReference.GetMemberParentClass(), Bp))
								{
									HitKind = Node->IsA<UK2Node_CallParentFunction>() ? TEXT("call_parent") : TEXT("call");
									HitOwner = Call->FunctionReference.GetMemberParentClass();
								}
							}
						}
						if (HitKind.IsEmpty() && (Kind == TEXT("variable") || Kind == TEXT("blueprint")))
						{
							if (UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node))
							{
								const bool bNameOk = (Kind == TEXT("blueprint"))
									|| Var->GetVarName() == TargetName;
								if (bNameOk && ClassMatches(Var->VariableReference.GetMemberParentClass(), Bp))
								{
									HitKind = Node->IsA<UK2Node_VariableSet>() ? TEXT("write") : TEXT("read");
									HitOwner = Var->VariableReference.GetMemberParentClass();
								}
							}
						}
						if (HitKind.IsEmpty() && (Kind == TEXT("dispatcher") || Kind == TEXT("blueprint")))
						{
							// ComponentBoundEvent first: it derives from Event, and
							// most real bindings (a button's OnClicked) are these
							// with no AddDelegate node anywhere in the graph.
							if (UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node))
							{
								const bool bNameOk = (Kind == TEXT("blueprint"))
									|| Bound->DelegatePropertyName == TargetName;
								// Same skeleton/generated collapse as ClassMatches.
								UClass* const OwnerAuth = Authoritative(Bound->DelegateOwnerClass);
								if (bNameOk && (!TargetAuth || (OwnerAuth
									&& (OwnerAuth->IsChildOf(TargetAuth)
										|| TargetAuth->IsChildOf(OwnerAuth)))))
								{
									HitKind = TEXT("bound_event");
									HitOwner = Bound->DelegateOwnerClass;
								}
							}
							else if (UK2Node_BaseMCDelegate* MC = Cast<UK2Node_BaseMCDelegate>(Node))
							{
								const bool bNameOk = (Kind == TEXT("blueprint"))
									|| MC->GetPropertyName() == TargetName;
								if (bNameOk && ClassMatches(MC->DelegateReference.GetMemberParentClass(), Bp))
								{
									HitOwner = MC->DelegateReference.GetMemberParentClass();
									// Assign derives from Add, so test it first.
									if (Node->IsA<UK2Node_AssignDelegate>()) { HitKind = TEXT("assign"); }
									else if (Node->IsA<UK2Node_AddDelegate>()) { HitKind = TEXT("bind"); }
									else if (Node->IsA<UK2Node_RemoveDelegate>()) { HitKind = TEXT("unbind"); }
									else if (Node->IsA<UK2Node_CallDelegate>()) { HitKind = TEXT("broadcast"); }
									else if (Node->IsA<UK2Node_ClearDelegate>()) { HitKind = TEXT("clear"); }
									else { HitKind = TEXT("delegate"); }
								}
							}
						}
						if (HitKind.IsEmpty() && Kind == TEXT("function"))
						{
							// Who IMPLEMENTS it, not just who calls it.
							if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
							{
								if (Event->EventReference.GetMemberName() == TargetName
									&& ClassMatches(Event->EventReference.GetMemberParentClass(), Bp))
								{
									HitKind = Event->bOverrideFunction ? TEXT("override") : TEXT("event");
									HitOwner = Event->EventReference.GetMemberParentClass();
								}
							}
							else if (UK2Node_CreateDelegate* Create = Cast<UK2Node_CreateDelegate>(Node))
							{
								// Bound as a handler - invisible to any call-site scan.
								// This was the one hit kind that matched on name
								// alone, so a 'class' the caller passed to narrow
								// the search was ignored here and a same-named
								// function on an unrelated class came back as a
								// reference to theirs.
								if (Create->GetFunctionName() == TargetName
									&& ClassMatches(Create->GetScopeClass(), Bp))
								{
									HitKind = TEXT("used_as_handler");
									HitOwner = Create->GetScopeClass();
								}
							}
						}

						if (HitKind.IsEmpty())
						{
							continue;
						}
						TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
						Row->SetStringField(TEXT("blueprint"), Bp->GetPathName());
						Row->SetStringField(TEXT("graph"), Graph->GetName());
						Row->SetStringField(TEXT("node"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						Row->SetStringField(TEXT("kind"), HitKind);
						Row->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
						// A self-context member carries no class of its own - it
						// belongs to the blueprint the node sits in, which is the
						// same resolution ClassMatches used to accept the hit.
						if (UClass* const OwnerAuth = Authoritative(HitOwner
							? HitOwner
							: (Bp->GeneratedClass ? Bp->GeneratedClass.Get() : Bp->SkeletonGeneratedClass.Get())))
						{
							Row->SetStringField(TEXT("owner"), OwnerAuth->GetPathName());
						}
						Hits.Add(MakeShared<FJsonValueObject>(Row));
						HitAssets.Add(Bp->GetPathName());
					}
				}
			}

			Data->SetArrayField(TEXT("hits"), Hits);
			Data->SetNumberField(TEXT("hit_count"), Hits.Num());
			Data->SetNumberField(TEXT("blueprints_with_hits"), HitAssets.Num());
			Data->SetNumberField(TEXT("scanned"), ToScan.Num());
			Data->SetNumberField(TEXT("already_loaded"), FreeCount);
			Data->SetNumberField(TEXT("loaded_for_this_call"), Loaded);
			// Whether the registry could narrow the search matters to how much the
			// answer is worth: for /Script/Engine members it could not, so this
			// was a path sweep bounded by max_load.
			Data->SetBoolField(TEXT("registry_bounded"), bBounded);
			if (NotScanned.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Skipped;
				for (const FString& Package : NotScanned)
				{
					Skipped.Add(MakeShared<FJsonValueString>(Package));
				}
				Data->SetArrayField(TEXT("not_scanned"), Skipped);
			}
			if (LoadFailed.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Broken;
				for (const FString& Package : LoadFailed)
				{
					Broken.Add(MakeShared<FJsonValueString>(Package));
				}
				Data->SetArrayField(TEXT("load_failed"), Broken);
			}
			Data->SetBoolField(TEXT("complete"),
				NotScanned.Num() == 0 && LoadFailed.Num() == 0 && !bLoadedOnly);

			// Every way this search can be bounded has to reach the summary
			// line, not just the fields beside it: a bare "0 hit(s)" from a
			// loaded_only pass is indistinguishable from "nothing uses this",
			// which is the one answer this tool must never fake.
			FString Summary = FString::Printf(TEXT("%d hit(s) across %d blueprint(s)"),
				Hits.Num(), HitAssets.Num());
			if (bLoadedOnly)
			{
				Summary += FString::Printf(
					TEXT(" - loaded_only: searched the %d Blueprint(s) already in memory and opened nothing, so this is not the whole project; drop loaded_only for that"),
					FreeCount);
			}
			if (NotScanned.Num() > 0)
			{
				Summary += FString::Printf(
					TEXT(" - %d candidate(s) not scanned, raise max_load above %d for the rest"),
					NotScanned.Num(), MaxLoad);
			}
			if (LoadFailed.Num() > 0)
			{
				Summary += FString::Printf(
					TEXT("; %d Blueprint(s) would not open and were not searched - see load_failed"),
					LoadFailed.Num());
			}
			return FUplinkToolResult::Ok(Data, Summary);
		},
		/*bTransactional=*/false);
}
