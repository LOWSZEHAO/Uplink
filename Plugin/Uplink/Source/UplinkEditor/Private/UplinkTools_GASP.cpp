// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Motion-matching tools: Pose Search databases, Chooser tables, and the runtime
// state of a motion-matching anim node (the "why did it pick that animation?"
// question that motion matching actually generates).
//
// Reflection-only, like the PCG tools. PoseSearch and Chooser are both disabled
// by default, so linking them would stop UplinkEditor loading anywhere they are
// off. Every lookup degrades to a clear "enable the plugin" message instead.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"
#include "UplinkCompat.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "JsonObjectConverter.h"
#include "UObject/UnrealType.h"

using namespace UplinkToolUtil;

namespace
{
	UObject* LoadByPath(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Found = StaticFindObject(UObject::StaticClass(), nullptr, *Path))
		{
			return Found;
		}
		return StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
	}

	/** Explain a missing motion-matching class rather than failing cryptically. */
	FString PluginHint(const TCHAR* ClassPath, const TCHAR* PluginName)
	{
		return FString::Printf(
			TEXT("could not resolve %s - the %s plugin is not enabled in this project. ")
			TEXT("plugin_enable {\"name\":\"%s\"} then restart the editor."),
			ClassPath, PluginName, PluginName);
	}

	/** Read an object-typed UPROPERTY off a UObject. */
	UObject* GetObjectProp(UObject* Owner, const TCHAR* PropName)
	{
		if (!Owner) { return nullptr; }
		if (FObjectPropertyBase* P = CastField<FObjectPropertyBase>(Owner->GetClass()->FindPropertyByName(PropName)))
		{
			return P->GetObjectPropertyValue_InContainer(Owner);
		}
		return nullptr;
	}

	/** Export any UPROPERTY as a JSON value, so callers see real data without guessing names. */
	TSharedPtr<FJsonValue> PropToJson(UObject* Owner, FProperty* Property)
	{
		if (!Owner || !Property) { return nullptr; }
		const void* Addr = Property->ContainerPtrToValuePtr<void>(Owner);
		return FJsonObjectConverter::UPropertyToJsonValue(Property, Addr, 0, 0);
	}

	/** Walk an object's properties into a JSON object, skipping the huge ones. */
	void DumpProperties(UObject* Obj, TSharedRef<FJsonObject>& Out, int32 MaxArray)
	{
		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			FProperty* P = *It;
			const FString Name = P->GetName();
			// Cooked search indices are megabytes of floats - never useful here.
			if (Name.Contains(TEXT("SearchIndex")) || Name.Contains(TEXT("_DEPRECATED")))
			{
				continue;
			}
			if (FArrayProperty* Arr = CastField<FArrayProperty>(P))
			{
				FScriptArrayHelper Helper(Arr, Arr->ContainerPtrToValuePtr<void>(Obj));
				if (Helper.Num() > MaxArray)
				{
					Out->SetStringField(Name, FString::Printf(TEXT("<%d entries - too large to inline>"), Helper.Num()));
					continue;
				}
			}
			if (TSharedPtr<FJsonValue> V = PropToJson(Obj, P))
			{
				Out->SetField(Name, V);
			}
		}
	}
}

void UplinkTools::RegisterGASP(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("posesearch_query"),
		TEXT("Inspect a Pose Search database (motion matching): its schema (skeleton, sample rate, feature channels) and the animations it can select from. This is what a motion-matching anim graph searches - animbp_query will show almost nothing for these graphs because motion matching replaces state machines. Requires the PoseSearch plugin."),
		TEXT(R"json({"type":"object","properties":{"database":{"type":"string","description":"Object path, e.g. /Game/.../PSD_Idles.PSD_Idles"},"max_animations":{"type":"number","default":60}},"required":["database"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			UClass* DbClass = FindObject<UClass>(nullptr, TEXT("/Script/PoseSearch.PoseSearchDatabase"));
			if (!DbClass)
			{
				return FUplinkToolResult::Error(PluginHint(TEXT("/Script/PoseSearch.PoseSearchDatabase"), TEXT("PoseSearch")));
			}

			const FString Path = GetString(Ctx.Params, TEXT("database"));
			UObject* Db = LoadByPath(Path);
			if (!Db)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("database not found: %s"), *Path));
			}
			if (!Db->IsA(DbClass))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("%s is a %s, not a PoseSearchDatabase"), *Path, *Db->GetClass()->GetName()));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("database"), Db->GetPathName());

			// ---- schema: what the search actually compares on ----
			if (UObject* Schema = GetObjectProp(Db, TEXT("Schema")))
			{
				TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetStringField(TEXT("asset"), Schema->GetPathName());
				if (FIntProperty* Rate = CastField<FIntProperty>(Schema->GetClass()->FindPropertyByName(TEXT("SampleRate"))))
				{
					S->SetNumberField(TEXT("sample_rate"), Rate->GetPropertyValue_InContainer(Schema));
				}
				// feature channels describe what is matched (pose, trajectory, ...)
				if (FArrayProperty* Chan = CastField<FArrayProperty>(Schema->GetClass()->FindPropertyByName(TEXT("Channels"))))
				{
					FScriptArrayHelper Helper(Chan, Chan->ContainerPtrToValuePtr<void>(Schema));
					FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(Chan->Inner);
					TArray<TSharedPtr<FJsonValue>> Rows;
					for (int32 i = 0; Inner && i < Helper.Num(); ++i)
					{
						if (UObject* Ch = Inner->GetObjectPropertyValue(Helper.GetRawPtr(i)))
						{
							Rows.Add(MakeShared<FJsonValueString>(Ch->GetClass()->GetName()));
						}
					}
					S->SetArrayField(TEXT("channels"), Rows);
				}
				Data->SetObjectField(TEXT("schema"), S);
			}

			// ---- the animation set this database can pick from ----
			const int32 MaxAnims = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max_animations"), 60)), 1, 500);
			if (FArrayProperty* Assets = CastField<FArrayProperty>(Db->GetClass()->FindPropertyByName(TEXT("DatabaseAnimationAssets"))))
			{
				FScriptArrayHelper Helper(Assets, Assets->ContainerPtrToValuePtr<void>(Db));
				TArray<TSharedPtr<FJsonValue>> Rows;
				for (int32 i = 0; i < Helper.Num() && Rows.Num() < MaxAnims; ++i)
				{
					// each entry is an FInstancedStruct wrapping a per-asset-type record
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					if (FStructProperty* SP = CastField<FStructProperty>(Assets->Inner))
					{
						FJsonObjectConverter::UStructToJsonObject(SP->Struct, Helper.GetRawPtr(i), Row, 0, 0);
					}
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}
				Data->SetArrayField(TEXT("animations"), Rows);
				Data->SetNumberField(TEXT("animation_count"), Helper.Num());
			}

			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("chooser_query"),
		TEXT("Inspect a Chooser table - the data-driven logic a motion-matching setup uses to decide WHICH pose search database (or asset) to use for the current game state. Dumps the table's rows, columns and result objects. Requires the Chooser plugin."),
		TEXT(R"json({"type":"object","properties":{"chooser":{"type":"string"},"max_rows":{"type":"number","default":40}},"required":["chooser"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			UClass* TableClass = FindObject<UClass>(nullptr, TEXT("/Script/Chooser.ChooserTable"));
			if (!TableClass)
			{
				return FUplinkToolResult::Error(PluginHint(TEXT("/Script/Chooser.ChooserTable"), TEXT("Chooser")));
			}
			const FString Path = GetString(Ctx.Params, TEXT("chooser"));
			UObject* Table = LoadByPath(Path);
			if (!Table)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("chooser not found: %s"), *Path));
			}

			const int32 MaxRows = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max_rows"), 40)), 1, 200);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("chooser"), Table->GetPathName());
			Data->SetStringField(TEXT("class"), Table->GetClass()->GetName());
			DumpProperties(Table, Data, MaxRows);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("motionmatch_debug"),
		TEXT("Read the live motion-matching state off a character in the running game: which pose search database(s) the anim node is currently searching, plus any motion-matching values the anim blueprint exposes. Answers 'why is it playing that animation?' during a PIE session. Pair with pie_start."),
		TEXT(R"json({"type":"object","properties":{"actor":{"type":"string","description":"Actor name/label; defaults to the player pawn"},"world":{"type":"string","enum":["editor","pie"]},"max_values":{"type":"number","default":40}},"required":[]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return FUplinkToolResult::Error(WorldError);
			}

			AActor* Actor = nullptr;
			const FString ActorRef = GetString(Ctx.Params, TEXT("actor"));
			if (!ActorRef.IsEmpty())
			{
				Actor = FindActor(World, ActorRef);
				if (!Actor)
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("actor not found: %s"), *ActorRef));
				}
			}
			else
			{
				if (APlayerController* PC = World->GetFirstPlayerController())
				{
					Actor = PC->GetPawn();
				}
				if (!Actor)
				{
					return FUplinkToolResult::Error(TEXT("no player pawn - pass 'actor', or start PIE first"));
				}
			}

			USkeletalMeshComponent* Mesh = Actor->FindComponentByClass<USkeletalMeshComponent>();
			if (!Mesh)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("%s has no skeletal mesh component"), *Actor->GetName()));
			}
			UAnimInstance* Anim = Mesh->GetAnimInstance();
			if (!Anim)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("%s has no anim instance running"), *Actor->GetName()));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("actor"), Actor->GetName());
			Data->SetStringField(TEXT("anim_instance"), Anim->GetClass()->GetPathName());

			// Motion-matching anim nodes live as struct properties on the generated
			// class. Their MotionMatchingState is not a UPROPERTY, but the database
			// they are searching is - which is the most useful single fact.
			UScriptStruct* NodeStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/PoseSearch.AnimNode_MotionMatching"));
			TArray<TSharedPtr<FJsonValue>> Nodes;
			if (NodeStruct)
			{
				for (TFieldIterator<FStructProperty> It(Anim->GetClass()); It; ++It)
				{
					FStructProperty* SP = *It;
					if (!SP->Struct || !SP->Struct->IsChildOf(NodeStruct))
					{
						continue;
					}
					const void* NodeAddr = SP->ContainerPtrToValuePtr<void>(Anim);
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("node"), SP->GetName());
					if (FObjectPropertyBase* DbProp = CastField<FObjectPropertyBase>(SP->Struct->FindPropertyByName(TEXT("Database"))))
					{
						if (UObject* Db = DbProp->GetObjectPropertyValue(DbProp->ContainerPtrToValuePtr<void>(NodeAddr)))
						{
							Row->SetStringField(TEXT("database"), Db->GetPathName());
						}
					}
					for (const TCHAR* Field : { TEXT("BlendTime"), TEXT("PlayRateMultiplier"), TEXT("PoseReselectHistory"), TEXT("SearchThrottleTime") })
					{
						if (FFloatProperty* FP = CastField<FFloatProperty>(SP->Struct->FindPropertyByName(Field)))
						{
							Row->SetNumberField(Field, FP->GetPropertyValue(FP->ContainerPtrToValuePtr<void>(NodeAddr)));
						}
					}
					Nodes.Add(MakeShared<FJsonValueObject>(Row));
				}
			}
			Data->SetArrayField(TEXT("motion_matching_nodes"), Nodes);

			// Whatever the anim blueprint itself surfaces - GASP-style graphs store
			// the chosen animation and trajectory here, which is the richest source.
			const int32 MaxValues = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max_values"), 40)), 1, 200);
			TSharedRef<FJsonObject> Vars = MakeShared<FJsonObject>();
			int32 Count = 0;
			for (TFieldIterator<FProperty> It(Anim->GetClass()); It && Count < MaxValues; ++It)
			{
				FProperty* P = *It;
				// Blueprint-exposed values only; skip the anim node structs themselves.
				if (!P->HasAnyPropertyFlags(CPF_BlueprintVisible) || CastField<FStructProperty>(P) == nullptr)
				{
					if (!P->HasAnyPropertyFlags(CPF_BlueprintVisible))
					{
						continue;
					}
				}
				if (TSharedPtr<FJsonValue> V = PropToJson(Anim, P))
				{
					Vars->SetField(P->GetName(), V);
					++Count;
				}
			}
			Data->SetObjectField(TEXT("anim_values"), Vars);

			if (!NodeStruct)
			{
				return FUplinkToolResult::Ok(Data, TEXT("PoseSearch is not loaded, so no motion-matching nodes could be identified; anim values are still reported."));
			}
			return FUplinkToolResult::Ok(Data);
		});
}
