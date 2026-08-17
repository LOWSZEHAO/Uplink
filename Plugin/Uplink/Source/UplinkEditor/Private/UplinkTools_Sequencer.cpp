// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// sequence_query - read a Level Sequence: playback range, bindings, tracks
// and section timings in seconds, so gameplay can be aligned to cinematics.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"

using namespace UplinkToolUtil;

namespace
{
	double FrameToSeconds(FFrameNumber Frame, const FFrameRate& TickResolution)
	{
		return TickResolution.AsSeconds(FFrameTime(Frame));
	}

	TSharedRef<FJsonObject> TrackToJson(const UMovieSceneTrack* Track, const FFrameRate& TickResolution)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("class"), Track->GetClass()->GetName());
		TArray<TSharedPtr<FJsonValue>> Sections;
		for (const UMovieSceneSection* Section : Track->GetAllSections())
		{
			if (!Section)
			{
				continue;
			}
			TSharedRef<FJsonObject> SectionRow = MakeShared<FJsonObject>();
			const TRange<FFrameNumber> Range = Section->GetRange();
			if (Range.GetLowerBound().IsClosed())
			{
				SectionRow->SetNumberField(TEXT("start"), FrameToSeconds(Range.GetLowerBoundValue(), TickResolution));
			}
			if (Range.GetUpperBound().IsClosed())
			{
				SectionRow->SetNumberField(TEXT("end"), FrameToSeconds(Range.GetUpperBoundValue(), TickResolution));
			}
			Sections.Add(MakeShared<FJsonValueObject>(SectionRow));
		}
		Row->SetArrayField(TEXT("sections"), Sections);
		return Row;
	}
}

void UplinkTools::RegisterSequencer(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("sequence_query"),
		TEXT("Read a Level Sequence: playback range and frame rate, every binding (possessable/spawnable) with its tracks, and each track's section timings in SECONDS - the timing truth for aligning gameplay events to cinematics. Playback at runtime needs no dedicated tool: call_function LevelSequencePlayer.CreateLevelSequencePlayer, then Play on the returned player."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"max":{"type":"number","default":100,"description":"Maximum bindings returned; the reply carries total_bindings and truncated"}},"required":["asset"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Path = GetString(Ctx.Params, TEXT("asset"));
			ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *Path);
			if (!Sequence)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("level sequence not found: %s"), *Path));
			}
			UMovieScene* MovieScene = Sequence->GetMovieScene();
			if (!MovieScene)
			{
				return FUplinkToolResult::Error(TEXT("sequence has no movie scene"));
			}

			const FFrameRate TickResolution = MovieScene->GetTickResolution();
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("sequence"), Sequence->GetPathName());
			Data->SetNumberField(TEXT("display_rate_fps"), MovieScene->GetDisplayRate().AsDecimal());

			const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
			if (Playback.GetLowerBound().IsClosed())
			{
				Data->SetNumberField(TEXT("start"), FrameToSeconds(Playback.GetLowerBoundValue(), TickResolution));
			}
			if (Playback.GetUpperBound().IsClosed())
			{
				Data->SetNumberField(TEXT("end"), FrameToSeconds(Playback.GetUpperBoundValue(), TickResolution));
			}

			// A cinematic can carry hundreds of bindings, each with its own
			// tracks and sections, so the reply grows faster than the sequence.
			const int32 Max = FMath::Clamp(
				static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 100.0)), 1, 1000);

			// GetBindings() has a const overload and a deprecated non-const one;
			// reading through a const pointer picks the one that survives.
			const UMovieScene* ReadOnlyScene = MovieScene;

			TArray<TSharedPtr<FJsonValue>> Bindings;
			for (const FMovieSceneBinding& Binding : ReadOnlyScene->GetBindings())
			{
				if (Bindings.Num() >= Max)
				{
					break;
				}
				const FGuid ObjectGuid = Binding.GetObjectGuid();

				// The binding no longer carries its own name; it belongs to
				// whichever of the two owns this guid, and that also tells us
				// which kind it is, so both come out of one lookup.
				FString Name;
				FString Kind;
				if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(ObjectGuid))
				{
					Name = Possessable->GetName();
					Kind = TEXT("possessable");
				}
				else if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(ObjectGuid))
				{
					Name = Spawnable->GetName();
					Kind = TEXT("spawnable");
				}

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Name);
				Row->SetStringField(TEXT("guid"), ObjectGuid.ToString(EGuidFormats::DigitsWithHyphens));
				if (!Kind.IsEmpty())
				{
					Row->SetStringField(TEXT("kind"), Kind);
				}
				TArray<TSharedPtr<FJsonValue>> Tracks;
				for (const UMovieSceneTrack* Track : Binding.GetTracks())
				{
					if (Track)
					{
						Tracks.Add(MakeShared<FJsonValueObject>(TrackToJson(Track, TickResolution)));
					}
				}
				Row->SetArrayField(TEXT("tracks"), Tracks);
				Bindings.Add(MakeShared<FJsonValueObject>(Row));
			}
			Data->SetArrayField(TEXT("bindings"), Bindings);
			Data->SetNumberField(TEXT("total_bindings"), ReadOnlyScene->GetBindings().Num());
			Data->SetBoolField(TEXT("truncated"), ReadOnlyScene->GetBindings().Num() > Bindings.Num());

			TArray<TSharedPtr<FJsonValue>> RootTracks;
			for (const UMovieSceneTrack* Track : MovieScene->GetTracks())
			{
				if (Track)
				{
					RootTracks.Add(MakeShared<FJsonValueObject>(TrackToJson(Track, TickResolution)));
				}
			}
			if (const UMovieSceneTrack* CameraCut = MovieScene->GetCameraCutTrack())
			{
				RootTracks.Add(MakeShared<FJsonValueObject>(TrackToJson(CameraCut, TickResolution)));
			}
			Data->SetArrayField(TEXT("root_tracks"), RootTracks);
			return FUplinkToolResult::Ok(Data);
		});
}
