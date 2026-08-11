// Copyright (c) 2026 Low Sze Hao. MIT License.
// Animation tools: anim_query (read timing data - the frame truth needed to
// place gameplay events correctly instead of guessing with Delay nodes) and
// anim_modify (add/remove notifies at exact times).

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Animation/Skeleton.h"

using namespace UplinkToolUtil;

namespace
{
	UAnimSequenceBase* LoadAnim(const FUplinkToolContext& Ctx, FString& OutError)
	{
		const FString Path = GetString(Ctx.Params, TEXT("asset"));
		UAnimSequenceBase* Anim = LoadObject<UAnimSequenceBase>(nullptr, *Path);
		if (!Anim)
		{
			OutError = FString::Printf(TEXT("animation asset not found: %s (montage or sequence)"), *Path);
		}
		return Anim;
	}
}

void UplinkTools::RegisterAnim(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("anim_query"),
		TEXT("Read an animation montage or sequence: play length, frame rate (sequences), montage sections, and every notify with its exact trigger time - the data needed to hook gameplay (damage, collision, sounds) to the right frame instead of guessing with delays."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string","description":"UAnimMontage or UAnimSequence asset path"}},"required":["asset"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UAnimSequenceBase* Anim = LoadAnim(Ctx, Error);
			if (!Anim)
			{
				return FUplinkToolResult::Error(Error);
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("asset"), Anim->GetPathName());
			Data->SetStringField(TEXT("class"), Anim->GetClass()->GetName());
			Data->SetNumberField(TEXT("length_seconds"), Anim->GetPlayLength());

			if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Anim))
			{
				const double FrameRate = Sequence->GetSamplingFrameRate().AsDecimal();
				Data->SetNumberField(TEXT("frame_rate"), FrameRate);
				Data->SetNumberField(TEXT("num_frames"), Sequence->GetNumberOfSampledKeys());
			}

			if (const UAnimMontage* Montage = Cast<UAnimMontage>(Anim))
			{
				TArray<TSharedPtr<FJsonValue>> Sections;
				for (const FCompositeSection& Section : Montage->CompositeSections)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Section.SectionName.ToString());
					Row->SetNumberField(TEXT("time"), Section.GetTime());
					Sections.Add(MakeShared<FJsonValueObject>(Row));
				}
				Data->SetArrayField(TEXT("sections"), Sections);
			}

			TArray<TSharedPtr<FJsonValue>> Notifies;
			for (const FAnimNotifyEvent& Event : Anim->Notifies)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Event.NotifyName.ToString());
				Row->SetNumberField(TEXT("time"), Event.GetTriggerTime());
				if (Event.GetDuration() > 0.0f)
				{
					Row->SetNumberField(TEXT("duration"), Event.GetDuration());
				}
				Row->SetNumberField(TEXT("track"), Event.TrackIndex);
				if (Event.Notify)
				{
					Row->SetStringField(TEXT("notify_class"), Event.Notify->GetClass()->GetPathName());
				}
				else if (Event.NotifyStateClass)
				{
					Row->SetStringField(TEXT("notify_state_class"), Event.NotifyStateClass->GetClass()->GetPathName());
				}
				Notifies.Add(MakeShared<FJsonValueObject>(Row));
			}
			Data->SetArrayField(TEXT("notifies"), Notifies);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("anim_modify"),
		TEXT("Edit an animation's notifies. op add_notify {name, time (seconds) or frame (sequences only), notify_class?}: a name-only notify becomes a skeleton notify (fires AnimNotify_<Name> in the Anim Blueprint and is delivered through montage OnNotifyBegin); pass notify_class for a UAnimNotify subclass instead. op remove_notify {name or index}. The asset is marked dirty, not saved."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"op":{"type":"string","enum":["add_notify","remove_notify"]},"name":{"type":"string"},"time":{"type":"number","description":"Trigger time in seconds"},"frame":{"type":"number","description":"Alternative to time, sequences only"},"track":{"type":"number","default":0},"notify_class":{"type":"string","description":"Optional UAnimNotify subclass path"},"index":{"type":"number","description":"remove_notify: notify index from anim_query order"}},"required":["asset","op"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UAnimSequenceBase* Anim = LoadAnim(Ctx, Error);
			if (!Anim)
			{
				return FUplinkToolResult::Error(Error);
			}
			const FString Op = GetString(Ctx.Params, TEXT("op"));
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

			if (Op == TEXT("add_notify"))
			{
				const FName NotifyName(*GetString(Ctx.Params, TEXT("name")));
				if (NotifyName.IsNone())
				{
					return FUplinkToolResult::Error(TEXT("'name' is required"));
				}

				double Time = GetNumber(Ctx.Params, TEXT("time"), -1.0);
				if (Time < 0.0 && Ctx.Params->HasField(FStringView(TEXT("frame"))))
				{
					const UAnimSequence* Sequence = Cast<UAnimSequence>(Anim);
					if (!Sequence)
					{
						return FUplinkToolResult::Error(TEXT("'frame' only works on sequences; use 'time' for montages"));
					}
					Time = GetNumber(Ctx.Params, TEXT("frame"), 0.0) / Sequence->GetSamplingFrameRate().AsDecimal();
				}
				if (Time < 0.0)
				{
					return FUplinkToolResult::Error(TEXT("provide 'time' (seconds) or 'frame'"));
				}
				Time = FMath::Clamp(Time, 0.0, static_cast<double>(Anim->GetPlayLength()));

				FAnimNotifyEvent NewEvent;
				NewEvent.NotifyName = NotifyName;
				NewEvent.Link(Anim, static_cast<float>(Time));
				NewEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(
					Anim->CalculateOffsetForNotify(static_cast<float>(Time)));
				NewEvent.TrackIndex = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("track"), 0)), 0,
					FMath::Max(0, Anim->AnimNotifyTracks.Num() - 1));

				const FString NotifyClassPath = GetString(Ctx.Params, TEXT("notify_class"));
				if (!NotifyClassPath.IsEmpty())
				{
					UClass* NotifyClass = StaticLoadClass(UAnimNotify::StaticClass(), nullptr, *NotifyClassPath);
					if (!NotifyClass)
					{
						return FUplinkToolResult::Error(TEXT("notify_class not found or not a UAnimNotify subclass"));
					}
					NewEvent.Notify = NewObject<UAnimNotify>(Anim, NotifyClass);
				}
				else if (USkeleton* Skeleton = Anim->GetSkeleton())
				{
					// Register the skeleton notify name so it shows up in editor UI.
					Skeleton->AddNewAnimationNotify(NotifyName);
				}

				Anim->Notifies.Add(NewEvent);
				Anim->RefreshCacheData();
				Anim->MarkPackageDirty();

				Data->SetStringField(TEXT("notify"), NotifyName.ToString());
				Data->SetNumberField(TEXT("time"), Time);
				return FUplinkToolResult::Ok(Data, TEXT("notify added (asset dirty, not saved)"));
			}

			if (Op == TEXT("remove_notify"))
			{
				int32 Removed = 0;
				if (Ctx.Params->HasField(FStringView(TEXT("index"))))
				{
					const int32 Index = static_cast<int32>(GetNumber(Ctx.Params, TEXT("index"), -1));
					if (!Anim->Notifies.IsValidIndex(Index))
					{
						return FUplinkToolResult::Error(TEXT("notify index out of range"));
					}
					Anim->Notifies.RemoveAt(Index);
					Removed = 1;
				}
				else
				{
					const FName NotifyName(*GetString(Ctx.Params, TEXT("name")));
					Removed = Anim->Notifies.RemoveAll([&NotifyName](const FAnimNotifyEvent& Event)
					{
						return Event.NotifyName == NotifyName;
					});
				}
				Anim->RefreshCacheData();
				Anim->MarkPackageDirty();
				Data->SetNumberField(TEXT("removed"), Removed);
				return FUplinkToolResult::Ok(Data);
			}

			return FUplinkToolResult::Error(TEXT("op must be add_notify or remove_notify"));
		});
}
