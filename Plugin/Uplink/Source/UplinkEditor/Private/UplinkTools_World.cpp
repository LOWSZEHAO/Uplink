// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// World tools: worlds, level_actors, spawn_actor, delete_actors, move_actor,
// spawn_batch, viewport_camera, trace.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "LevelEditorSubsystem.h"
#include "LevelEditorViewport.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/Actor.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Engine/Brush.h"
#include "Engine/Polys.h"
#include "Model.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

using namespace UplinkToolUtil;

namespace
{
	/**
	 * "actor not found" leaves the caller nowhere to go. Name what is
	 * actually in the level - the usual causes are a label/name mismatch or
	 * looking in the wrong world, and both are obvious from the list.
	 */
	FString DescribeActorsNearby(UWorld* World, const FString& Wanted)
	{
		TArray<FString> Names;
		for (TActorIterator<AActor> It(World); It && Names.Num() < 30; ++It)
		{
			if (It->IsA<AWorldSettings>())
			{
				continue;
			}
			Names.Add(It->GetActorLabel());
		}
		return FString::Printf(
			TEXT("no actor named '%s' in this world. Actors here: %s"),
			*Wanted, Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)"));
	}

	/**
	 * Exact name or exact label, and nothing else.
	 *
	 * The shared FindActor falls back to a label substring, which is a helpful
	 * guess for a tool that reads and a loaded gun for one that destroys:
	 * asking to delete "Cube" in a level that only has "Cube_Wall_01" would
	 * take the wall and report it as the actor asked for. The near miss is
	 * handed back instead, so the caller can name it deliberately.
	 */
	AActor* FindActorExact(UWorld* World, const FString& NameOrLabel, FString& OutNearMiss)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor->GetName() == NameOrLabel)
			{
				return Actor;
			}
#if WITH_EDITOR
			const FString Label = Actor->GetActorLabel();
			if (Label == NameOrLabel)
			{
				return Actor;
			}
			if (OutNearMiss.IsEmpty() && Label.Contains(NameOrLabel))
			{
				OutNearMiss = Label;
			}
#endif
		}
		return nullptr;
	}

	/** Every collision profile the project defines, so a refusal says what to type. */
	FString DescribeCollisionProfiles()
	{
		TArray<TSharedPtr<FName>> Names;
		UCollisionProfile::GetProfileNames(Names);
		TArray<FString> Flat;
		for (const TSharedPtr<FName>& Name : Names)
		{
			if (Name.IsValid())
			{
				Flat.Add(Name->ToString());
			}
		}
		return Flat.Num() ? FString::Join(Flat, TEXT(", ")) : TEXT("(none)");
	}

	/** Channel names as this project spells them - engine channels plus any it renamed. */
	FString DescribeCollisionChannels()
	{
		TArray<FString> Names;
		if (const UCollisionProfile* Profiles = UCollisionProfile::Get())
		{
			for (int32 Index = 0; Index < ECC_MAX; ++Index)
			{
				const FString Name = Profiles->ReturnChannelNameFromContainerIndex(Index).ToString();
				// A channel the project has not claimed keeps its placeholder
				// enum name, and the engine's own six are reserved. Listing two
				// dozen of those buries the handful that are real.
				if (Name.IsEmpty() || Name == TEXT("None") || Name.Contains(TEXT("Deprecated"))
					|| Name.StartsWith(TEXT("GameTraceChannel")) || Name.StartsWith(TEXT("EngineTraceChannel")))
				{
					continue;
				}
				Names.Add(Name);
			}
		}
		return Names.Num() ? FString::Join(Names, TEXT(", ")) : TEXT("(none)");
	}

	/**
	 * A trace channel by whatever name the caller has actually seen.
	 *
	 * StaticEnum only knows ECC_GameTraceChannel2. A project that renames that
	 * channel in DefaultEngine.ini gets a name that is the only one appearing
	 * in the .ini, in the details panel, or in anyone's head - and it never
	 * reaches the enum. The display-name table is where the two meet.
	 */
	bool ResolveCollisionChannel(const FString& Name, ECollisionChannel& OutChannel)
	{
		if (const UEnum* ChannelEnum = StaticEnum<ECollisionChannel>())
		{
			// Accept both the friendly names and the raw ECC_ enum names.
			int64 Value = ChannelEnum->GetValueByNameString(FString::Printf(TEXT("ECC_%s"), *Name));
			if (Value == INDEX_NONE)
			{
				Value = ChannelEnum->GetValueByNameString(Name);
			}
			if (Value >= 0 && Value < static_cast<int64>(ECC_MAX))
			{
				OutChannel = static_cast<ECollisionChannel>(Value);
				return true;
			}
		}
		if (const UCollisionProfile* Profiles = UCollisionProfile::Get())
		{
			for (int32 Index = 0; Index < ECC_MAX; ++Index)
			{
				if (Profiles->ReturnChannelNameFromContainerIndex(Index).ToString() == Name)
				{
					OutChannel = static_cast<ECollisionChannel>(Index);
					return true;
				}
			}
		}
		return false;
	}
}

void UplinkTools::RegisterWorld(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("worlds"),
		TEXT("List every live world and the id that addresses it: the level being edited ('editor'), one per PIE instance ('pie:0', 'pie:1' when playing as several clients), the preview world behind an open asset editor ('preview:World_3'), and anything else the engine is holding a world context for ('game:', 'inactive:'). Pass an id as the 'world' parameter of any world tool to hit exactly that one - 'pie' on its own only ever means the instance currently in play. 'is_default' marks the world an omitted 'world' goes to right now."),
		TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			FString DefaultId;
			for (const FUplinkWorldEntry& Entry : UplinkWorlds::Enumerate())
			{
				// Counted the same way level_actors lists them, so the two
				// tools cannot disagree about what is in a world.
				int32 ActorCount = 0;
				for (TActorIterator<AActor> It(Entry.World); It; ++It)
				{
					++ActorCount;
				}

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("id"), Entry.Id);
				Row->SetStringField(TEXT("type"), Entry.Type);
				Row->SetStringField(TEXT("net_mode"), Entry.NetMode);
				Row->SetStringField(TEXT("map"), Entry.Map);
				if (Entry.PieInstance != INDEX_NONE)
				{
					Row->SetNumberField(TEXT("pie_instance"), Entry.PieInstance);
				}
				Row->SetBoolField(TEXT("simulating"), Entry.bSimulating);
				Row->SetBoolField(TEXT("is_default"), Entry.bDefault);
				Row->SetNumberField(TEXT("actor_count"), ActorCount);
				Rows.Add(MakeShared<FJsonValueObject>(Row));

				if (Entry.bDefault)
				{
					DefaultId = Entry.Id;
				}
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("worlds"), Rows);
			Data->SetNumberField(TEXT("count"), Rows.Num());
			Data->SetStringField(TEXT("default"), DefaultId);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("level_actors"),
		TEXT("List actors in the current world (PIE world when a session is active, else the editor world). Supports substring filters."),
		TEXT(R"json({"type":"object","properties":{"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"},"class_contains":{"type":"string","description":"Substring of the class PATH, e.g. 'StaticMeshActor' or '/Game/BP_'"},"name_contains":{"type":"string","description":"Matches actor name OR editor label"},"max":{"type":"number","default":200,"description":"Rows returned, capped at 2000; 'total_matching' keeps counting past it and 'truncated' says the list was cut"}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return FUplinkToolResult::Error(WorldError);
			}

			const FString ClassFilter = GetString(Ctx.Params, TEXT("class_contains"));
			const FString NameFilter = GetString(Ctx.Params, TEXT("name_contains"));
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 200)), 1, 2000);

			TArray<TSharedPtr<FJsonValue>> Actors;
			int32 Total = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				const FString ClassName = Actor->GetClass()->GetPathName();
				if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter))
				{
					continue;
				}
				FString Label;
#if WITH_EDITOR
				Label = Actor->GetActorLabel();
#endif
				if (!NameFilter.IsEmpty() && !Actor->GetName().Contains(NameFilter) && !Label.Contains(NameFilter))
				{
					continue;
				}

				++Total;
				if (Actors.Num() >= Max)
				{
					continue; // keep counting, stop collecting
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Actor->GetName());
				Row->SetStringField(TEXT("label"), Label);
				Row->SetStringField(TEXT("class"), ClassName);
				Row->SetObjectField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
				Actors.Add(MakeShared<FJsonValueObject>(Row));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("actors"), Actors);
			Data->SetNumberField(TEXT("total_matching"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Actors.Num());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("spawn_actor"),
		TEXT("Spawn an actor. class_path accepts a native class (/Script/Engine.PointLight) or a Blueprint generated class (/Game/Path/BP_Thing.BP_Thing_C)."),
		TEXT(R"json({"type":"object","properties":{"class_path":{"type":"string"},"location":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"rotation":{"type":"object","properties":{"pitch":{"type":"number"},"yaw":{"type":"number"},"roll":{"type":"number"}}},"label":{"type":"string","description":"Editor label (editor world only)"},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["class_path"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return FUplinkToolResult::Error(WorldError);
			}

			const FString ClassPath = GetString(Ctx.Params, TEXT("class_path"));
			UClass* Class = StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath);
			if (!Class)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("class not found: %s (Blueprint classes need the _C suffix, e.g. /Game/BP_X.BP_X_C)"), *ClassPath));
			}

			FVector Location = FVector::ZeroVector;
			TryGetVector(Ctx.Params, TEXT("location"), Location);
			FRotator Rotation = FRotator::ZeroRotator;
			TryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AActor* Actor = World->SpawnActor<AActor>(Class, Location, Rotation, SpawnParams);
			if (!Actor)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("the engine would not spawn a %s. An abstract class, an editor-only class, or one whose Blueprint does not compile cannot be placed - check it opens in the editor first."),
					*Class->GetName()));
			}

			const FString Label = GetString(Ctx.Params, TEXT("label"));
			const bool bLabelDropped = !Label.IsEmpty() && Ctx.IsPieWorld();
#if WITH_EDITOR
			if (!Label.IsEmpty() && !Ctx.IsPieWorld())
			{
				Actor->SetActorLabel(Label);
			}
#endif
			if (!Ctx.IsPieWorld())
			{
				World->MarkPackageDirty();
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("name"), Actor->GetName());
#if WITH_EDITOR
			// The label as it ended up, not as it was asked for. A caller that
			// then looks the actor up by the label it passed needs to see that
			// the play world kept the spawn name instead.
			Data->SetStringField(TEXT("label"), Actor->GetActorLabel());
#endif
			Data->SetObjectField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
			return FUplinkToolResult::Ok(Data, bLabelDropped
				? FString::Printf(TEXT("spawned as '%s'; 'label' was ignored because labels exist only in the editor world - address this actor by its name"), *Actor->GetName())
				: FString());
		});

	Registry.RegisterQuick(
		TEXT("delete_actors"),
		TEXT("Destroy actors by exact name or editor label - a partial match is never guessed at, because the guess is unrecoverable. 'destroyed' counts the actors actually destroyed; a name that matched nothing comes back in 'not_found', and an actor the engine would not destroy in 'refused'."),
		TEXT(R"json({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["names"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return FUplinkToolResult::Error(WorldError);
			}

			const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
			if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(FStringView(TEXT("names")), Names))
			{
				return FUplinkToolResult::Error(TEXT("'names' array is required"));
			}

			int32 Destroyed = 0;
			TArray<FString> Missing;
			TArray<FString> NearMisses;
			TArray<FString> Refused;
			for (const TSharedPtr<FJsonValue>& NameValue : *Names)
			{
				const FString Name = NameValue->AsString();
				FString NearMiss;
				AActor* Actor = FindActorExact(World, Name, NearMiss);
				if (!Actor)
				{
					Missing.Add(Name);
					if (!NearMiss.IsEmpty())
					{
						NearMisses.Add(FString::Printf(TEXT("'%s' -> '%s'"), *Name, *NearMiss));
					}
					continue;
				}
				// The engine refuses some destroys outright - the WorldSettings
				// actor always, and in a play world anything this instance has
				// no authority over. Counting the attempt as a destruction is
				// how "destroyed 300" comes back from a level that still holds
				// them.
				if (World->DestroyActor(Actor))
				{
					++Destroyed;
				}
				else
				{
					Refused.Add(Name);
				}
			}
			if (Destroyed > 0 && !Ctx.IsPieWorld())
			{
				World->MarkPackageDirty();
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("destroyed"), Destroyed);
			if (Missing.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> MissingJson;
				for (const FString& Name : Missing)
				{
					MissingJson.Add(MakeShared<FJsonValueString>(Name));
				}
				Data->SetArrayField(TEXT("not_found"), MissingJson);
			}
			if (Refused.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> RefusedJson;
				for (const FString& Name : Refused)
				{
					RefusedJson.Add(MakeShared<FJsonValueString>(Name));
				}
				Data->SetArrayField(TEXT("refused"), RefusedJson);
			}

			TArray<FString> Notes;
			if (Refused.Num() > 0)
			{
				Notes.Add(FString::Printf(
					TEXT("the engine refused to destroy %s - the world settings actor cannot be deleted, and in a play world neither can an actor this instance has no authority over"),
					*FString::Join(Refused, TEXT(", "))));
			}
			if (NearMisses.Num() > 0)
			{
				Notes.Add(FString::Printf(
					TEXT("names must match an actor name or editor label exactly; the closest thing in this world was %s"),
					*FString::Join(NearMisses, TEXT(", "))));
			}
			return FUplinkToolResult::Ok(Data, Notes.Num() ? FString::Join(Notes, TEXT("; ")) : FString());
		});

	Registry.RegisterQuick(
		TEXT("move_actor"),
		TEXT("Set an actor's location / rotation / scale (any subset). Uses physics-safe teleportation in PIE."),
		TEXT(R"json({"type":"object","properties":{"actor":{"type":"string"},"location":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"rotation":{"type":"object","properties":{"pitch":{"type":"number"},"yaw":{"type":"number"},"roll":{"type":"number"}}},"scale":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["actor"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return FUplinkToolResult::Error(WorldError);
			}

			const FString WantedActor = GetString(Ctx.Params, TEXT("actor"));
			AActor* Actor = FindActor(World, WantedActor);
			if (!Actor)
			{
				return FUplinkToolResult::Error(DescribeActorsNearby(World, WantedActor));
			}

			FVector Location;
			FRotator Rotation;
			FVector Scale;
			const bool bSetLocation = TryGetVector(Ctx.Params, TEXT("location"), Location);
			const bool bSetRotation = TryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);
			const bool bSetScale = TryGetVector(Ctx.Params, TEXT("scale"), Scale);
			const bool bMoving = bSetLocation || bSetRotation || bSetScale;
			if (bMoving && !Actor->GetRootComponent())
			{
				// SetActorLocation on a rootless actor returns false and does
				// nothing, which used to read back as a successful move to the
				// place it never went.
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' has no root component, so it has no transform to move. Move the actor that owns it, or add a scene component as its root."),
					*Actor->GetName()));
			}

			const bool bEditorWorld = !Ctx.IsPieWorld();
#if WITH_EDITOR
			// What a drag in the viewport does first. Without it the change
			// sits outside the open transaction, so edit_history cannot walk it
			// back, and in a one-file-per-actor level the actor's own package
			// is never marked dirty - so save writes the level and loses the
			// move.
			if (bMoving && bEditorWorld)
			{
				Actor->Modify();
			}
#endif
			if (bSetLocation)
			{
				Actor->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
			}
			if (bSetRotation)
			{
				Actor->SetActorRotation(Rotation, ETeleportType::TeleportPhysics);
			}
			if (bSetScale)
			{
				Actor->SetActorScale3D(Scale);
			}
#if WITH_EDITOR
			// The other half of the viewport's move: a Blueprint actor that
			// builds its components from its transform is otherwise left
			// showing the old one until something else rebuilds it.
			if (bMoving && bEditorWorld)
			{
				Actor->PostEditMove(/*bFinished=*/true);
			}
#endif
			if (bEditorWorld)
			{
				World->MarkPackageDirty();
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("name"), Actor->GetName());
			Data->SetObjectField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("spawn_batch"),
		TEXT("Spawn up to 1000 actors in one call - the scene-assembly workhorse. Each entry: {mesh: static mesh path (spawns a movable StaticMeshActor carrying it) OR class_path, never both, location, rotation?, scale?, label?, material? (slot 0 only)}. A city block, a prop set, or a whole layout lands in a single request. Returns every spawned actor's name in 'spawned'. A bad entry stops the batch rather than being skipped, and the refusal still carries 'spawned' plus 'failed_index', so the actors already placed can be named and cleaned up with delete_actors."),
		TEXT(R"json({"type":"object","properties":{"actors":{"type":"array","maxItems":1000,"items":{"type":"object"}},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["actors"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			if (!World)
			{
				return FUplinkToolResult::Error(Error);
			}
			const TArray<TSharedPtr<FJsonValue>>* Specs = nullptr;
			if (!Ctx.Params->TryGetArrayField(FStringView(TEXT("actors")), Specs) || Specs->Num() == 0)
			{
				return FUplinkToolResult::Error(TEXT("'actors' must be a non-empty array"));
			}
			if (Specs->Num() > 1000)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("%d actors in one call; the ceiling is 1000. Split the layout across several calls."), Specs->Num()));
			}

			UClass* StaticMeshActorClass = StaticLoadClass(AActor::StaticClass(), nullptr, TEXT("/Script/Engine.StaticMeshActor"));
			if (!StaticMeshActorClass)
			{
				// Nothing below can honour a 'mesh' entry without it, and
				// SpawnActor(nullptr) would come back as a per-entry failure
				// blaming the caller's data for an engine problem.
				return FUplinkToolResult::Error(TEXT("StaticMeshActor class is not loadable in this editor, so 'mesh' entries cannot be spawned - use 'class_path' instead"));
			}
			TArray<TSharedPtr<FJsonValue>> Spawned;

			// A batch that stops halfway has already changed the level. The
			// count alone leaves those actors in the world with nothing able to
			// address them, so the failure carries the same 'spawned' list a
			// success would - delete_actors takes it as-is.
			auto Abandon = [&](int32 Index, const FString& Reason) -> FUplinkToolResult
			{
				if (Spawned.Num() > 0 && !Ctx.IsPieWorld())
				{
					World->MarkPackageDirty();
				}
				TSharedRef<FJsonObject> Partial = MakeShared<FJsonObject>();
				Partial->SetArrayField(TEXT("spawned"), Spawned);
				Partial->SetNumberField(TEXT("count"), Spawned.Num());
				Partial->SetNumberField(TEXT("failed_index"), Index);
				FUplinkToolResult Failure = FUplinkToolResult::Error(FString::Printf(
					TEXT("actors[%d]: %s - %d spawned before the failure and left in the world, named in 'spawned'; pass that list to delete_actors to undo the half-batch"),
					Index, *Reason, Spawned.Num()));
				Failure.Data = Partial;
				return Failure;
			};

			// The half-configured actor for the failing entry goes back out, so
			// 'spawned' is exactly what is left behind. When the engine will not
			// take it, it is still standing in the level, so it joins the list
			// rather than disappearing from the caller's cleanup as well.
			auto AbandonEntry = [&](AActor* Placed, int32 Index, const FString& Reason) -> FUplinkToolResult
			{
				if (!World->DestroyActor(Placed))
				{
					Spawned.Add(MakeShared<FJsonValueString>(Placed->GetName()));
				}
				return Abandon(Index, Reason);
			};

			for (int32 Index = 0; Index < Specs->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* Spec = nullptr;
				if (!(*Specs)[Index]->TryGetObject(Spec) || !Spec->IsValid())
				{
					return Abandon(Index, TEXT("entry is not an object"));
				}

				FVector Location = FVector::ZeroVector;
				TryGetVector(*Spec, TEXT("location"), Location);
				FRotator Rotation = FRotator::ZeroRotator;
				TryGetRotator(*Spec, TEXT("rotation"), Rotation);

				const FString MeshPath = GetString(*Spec, TEXT("mesh"));
				const FString ClassPath = GetString(*Spec, TEXT("class_path"));
				UClass* Class = StaticMeshActorClass;
				if (MeshPath.IsEmpty())
				{
					Class = StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath);
					if (!Class)
					{
						return Abandon(Index, ClassPath.IsEmpty()
							? FString(TEXT("provide 'mesh' or 'class_path'"))
							: FString::Printf(TEXT("class not found: %s (a Blueprint class needs the _C suffix)"), *ClassPath));
					}
				}
				else if (!ClassPath.IsEmpty())
				{
					// 'mesh' used to win in silence, so a batch asking for
					// BP_Barrel actors carrying a mesh override came back as a
					// pile of plain StaticMeshActors reported as spawned.
					return Abandon(Index, FString::Printf(
						TEXT("'mesh' and 'class_path' both given ('%s' and '%s'); 'mesh' spawns a StaticMeshActor, so pick one - to put a mesh on your own class, spawn it and set the mesh with set_property"),
						*MeshPath, *ClassPath));
				}

				FActorSpawnParameters SpawnParams;
				SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				AActor* Actor = World->SpawnActor<AActor>(Class, Location, Rotation, SpawnParams);
				if (!Actor)
				{
					return Abandon(Index, FString::Printf(
						TEXT("the engine would not spawn a %s (an abstract or editor-only class cannot be placed)"), *Class->GetName()));
				}

				FVector Scale;
				if (TryGetVector(*Spec, TEXT("scale"), Scale))
				{
					Actor->SetActorScale3D(Scale);
				}
#if WITH_EDITOR
				const FString Label = GetString(*Spec, TEXT("label"));
				if (!Label.IsEmpty())
				{
					Actor->SetActorLabel(Label);
				}
#endif
				if (!MeshPath.IsEmpty())
				{
					UStaticMeshComponent* MeshComponent = Actor->FindComponentByClass<UStaticMeshComponent>();
					if (!MeshComponent)
					{
						return AbandonEntry(Actor, Index, FString::Printf(
							TEXT("a %s has no static mesh component to put '%s' on"), *Class->GetName(), *MeshPath));
					}

					UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
					if (!Mesh)
					{
						// The actor for this entry exists already, so it goes
						// with the refusal: an empty StaticMeshActor left behind
						// and absent from 'spawned' is invisible to the caller
						// and to the cleanup it would run.
						return AbandonEntry(Actor, Index, FString::Printf(TEXT("mesh not found: %s"), *MeshPath));
					}
					MeshComponent->SetMobility(EComponentMobility::Movable);
					MeshComponent->SetStaticMesh(Mesh);
					const FString MaterialPath = GetString(*Spec, TEXT("material"));
					if (!MaterialPath.IsEmpty())
					{
						UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
						if (!Material)
						{
							// Same policy as the mesh above: skipping this
							// quietly produced a batch of default-grey actors
							// reported as spawned.
							return AbandonEntry(Actor, Index, FString::Printf(TEXT("material not found: %s"), *MaterialPath));
						}
						MeshComponent->SetMaterial(0, Material);
					}
				}
				Spawned.Add(MakeShared<FJsonValueString>(Actor->GetName()));
			}
			if (!Ctx.IsPieWorld())
			{
				World->MarkPackageDirty();
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("spawned"), Spawned);
			Data->SetNumberField(TEXT("count"), Spawned.Num());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("viewport_camera"),
		TEXT("Move the editor viewport camera: 'focus_actor' frames an actor (like pressing F), or set 'location'/'rotation' directly. Pair with viewport_screenshot or capture_widget to look at a specific thing. Editor viewport only - during PIE use player_teleport for the game camera."),
		TEXT(R"json({"type":"object","properties":{"focus_actor":{"type":"string","description":"Actor name/label to frame"},"location":{"type":"object"},"rotation":{"type":"object"}}})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			if (!GEditor)
			{
				return FUplinkToolResult::Error(TEXT("no editor"));
			}

			const FString FocusName = GetString(Ctx.Params, TEXT("focus_actor"));
			if (!FocusName.IsEmpty())
			{
				UWorld* World = GEditor->GetEditorWorldContext().World();
				AActor* Actor = World ? FindActor(World, FocusName) : nullptr;
				if (!Actor)
				{
					return FUplinkToolResult::Error(World
						? DescribeActorsNearby(World, FocusName)
						: TEXT("no editor world available"));
				}
				GEditor->MoveViewportCamerasToActor(*Actor, /*bActiveViewportOnly=*/false);
			}

			FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
			if (!Client)
			{
				for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
				{
					if (Candidate && Candidate->IsPerspective())
					{
						Client = Candidate;
						break;
					}
				}
			}
			if (!Client)
			{
				return FUplinkToolResult::Error(TEXT("no level viewport available"));
			}

			FVector Location;
			if (TryGetVector(Ctx.Params, TEXT("location"), Location))
			{
				Client->SetViewLocation(Location);
			}
			FRotator Rotation;
			if (TryGetRotator(Ctx.Params, TEXT("rotation"), Rotation))
			{
				Client->SetViewRotation(Rotation);
			}
			Client->Invalidate();

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetObjectField(TEXT("location"), VectorToJson(Client->GetViewLocation()));
			const FRotator ViewRotation = Client->GetViewRotation();
			TSharedRef<FJsonObject> RotJson = MakeShared<FJsonObject>();
			RotJson->SetNumberField(TEXT("pitch"), ViewRotation.Pitch);
			RotJson->SetNumberField(TEXT("yaw"), ViewRotation.Yaw);
			RotJson->SetNumberField(TEXT("roll"), ViewRotation.Roll);
			Data->SetObjectField(TEXT("rotation"), RotJson);

			// The transform below is the editor camera's, and while the game is
			// playing in that viewport the view comes from the player instead.
			// The numbers are real and take effect when play stops; a
			// screenshot taken now will not show them.
			const bool bPlaying = GEditor->PlayWorld != nullptr;
			return FUplinkToolResult::Ok(Data, bPlaying
				? TEXT("a PIE session is running, so the level viewport is showing the game camera - this move applies to the editor camera and will not be visible until play stops. Use player_teleport to move the game camera.")
				: FString());
		});

	Registry.RegisterQuick(
		TEXT("trace"),
		TEXT("Ask the physics scene what is there. Casts a ray (or a swept sphere/box/capsule with 'radius'/'half_height') and reports what it hit: actor, component, impact point, normal, distance, physical material. 'to' or 'direction'+'distance' set the end point. 'profile' traces by collision profile (a stock one like 'BlockAll', or one your project defines), otherwise 'channel' names a trace channel ('Visibility', 'Camera', or a project channel by the name the project gave it). A profile, channel or ignore_actors name this project does not have is refused and listed back, never swapped for a default - a trace that answers on the wrong channel is worse than no answer. 'multi' returns every hit along the ray instead of the first. Use this instead of guessing geometry - a downward trace is how you find ground height under a point."),
		TEXT(R"json({"type":"object","properties":{"from":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"to":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"direction":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"distance":{"type":"number","default":10000},"shape":{"type":"string","enum":["line","sphere","box","capsule"],"default":"line"},"radius":{"type":"number","default":50,"description":"Sphere/capsule radius; for a box, the X and Y half-extent"},"half_height":{"type":"number","default":100,"description":"Capsule half-height; for a box, the Z half-extent (defaults to 'radius')"},"channel":{"type":"string","default":"Visibility"},"profile":{"type":"string"},"multi":{"type":"boolean","default":false},"complex":{"type":"boolean","default":false},"ignore_actors":{"type":"array","items":{"type":"string"}},"draw_seconds":{"type":"number","default":0,"description":"Draw the trace in the world for this many seconds (needs a realtime viewport to be visible)"},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["from"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			if (!World)
			{
				return FUplinkToolResult::Error(Error);
			}

			FVector Start;
			if (!TryGetVector(Ctx.Params, TEXT("from"), Start))
			{
				return FUplinkToolResult::Error(TEXT("'from' is required, as {x,y,z}"));
			}

			FVector End;
			if (!TryGetVector(Ctx.Params, TEXT("to"), End))
			{
				FVector Direction(0, 0, -1); // straight down: the common "what is below" case
				TryGetVector(Ctx.Params, TEXT("direction"), Direction);
				if (Direction.IsNearlyZero())
				{
					return FUplinkToolResult::Error(TEXT("'direction' cannot be zero; pass 'to' instead"));
				}
				End = Start + Direction.GetSafeNormal() * GetNumber(Ctx.Params, TEXT("distance"), 10000.0);
			}

			FCollisionQueryParams Query(TEXT("UplinkTrace"), /*bTraceComplex=*/false);
			bool bComplex = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("complex")), bComplex);
			Query.bTraceComplex = bComplex;
			Query.bReturnPhysicalMaterial = true;

			const TArray<TSharedPtr<FJsonValue>>* IgnoreList = nullptr;
			if (Ctx.Params->TryGetArrayField(FStringView(TEXT("ignore_actors")), IgnoreList))
			{
				for (const TSharedPtr<FJsonValue>& Entry : *IgnoreList)
				{
					FString ActorName;
					if (!Entry.IsValid() || !Entry->TryGetString(ActorName) || ActorName.IsEmpty())
					{
						continue;
					}
					AActor* Found = FindActor(World, ActorName);
					if (!Found)
					{
						// A name that resolves to nothing used to be dropped,
						// and the trace then answered a different question from
						// the one asked - reporting a hit on the very actor the
						// caller had excluded.
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("ignore_actors: %s"), *DescribeActorsNearby(World, ActorName)));
					}
					Query.AddIgnoredActor(Found);
				}
			}

			// Shape: a swept shape answers "does this fit / would a character
			// stand here", which a bare line cannot.
			const FString Shape = GetString(Ctx.Params, TEXT("shape"), TEXT("line"));
			FCollisionShape SweepShape;
			const bool bSweep = Shape != TEXT("line");
			if (Shape == TEXT("sphere"))
			{
				SweepShape = FCollisionShape::MakeSphere(GetNumber(Ctx.Params, TEXT("radius"), 50.0));
			}
			else if (Shape == TEXT("capsule"))
			{
				SweepShape = FCollisionShape::MakeCapsule(
					GetNumber(Ctx.Params, TEXT("radius"), 50.0), GetNumber(Ctx.Params, TEXT("half_height"), 100.0));
			}
			else if (Shape == TEXT("box"))
			{
				const double R = GetNumber(Ctx.Params, TEXT("radius"), 50.0);
				SweepShape = FCollisionShape::MakeBox(FVector3f(
					static_cast<float>(R), static_cast<float>(R),
					static_cast<float>(GetNumber(Ctx.Params, TEXT("half_height"), R))));
			}
			else if (bSweep)
			{
				return FUplinkToolResult::Error(TEXT("unknown shape (line, sphere, box, capsule)"));
			}

			bool bMulti = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("multi")), bMulti);

			const FString Profile = GetString(Ctx.Params, TEXT("profile"));
			const FString ChannelName = GetString(Ctx.Params, TEXT("channel"), TEXT("Visibility"));
			ECollisionChannel Channel = ECC_Visibility;
			if (!Profile.IsEmpty())
			{
				// The engine does not refuse a profile it has never heard of:
				// UWorld logs a warning, quietly traces on WorldStatic with
				// default responses, and hands back an answer that looks like
				// the project's collision setup and is not.
				ECollisionChannel ProfileChannel = ECC_Visibility;
				FCollisionResponseParams ProfileResponses;
				if (!UCollisionProfile::GetChannelAndResponseParams(FName(*Profile), ProfileChannel, ProfileResponses))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no collision profile named '%s' in this project. Profiles here: %s. Pass 'channel' instead to trace by channel."),
						*Profile, *DescribeCollisionProfiles()));
				}
			}
			else if (!ResolveCollisionChannel(ChannelName, Channel))
			{
				// Falling back to Visibility here was the same failure in
				// another costume: every trace on a project channel whose name
				// the enum does not carry silently ran on Visibility and
				// reported its hits as that channel's.
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("no collision channel named '%s' in this project. Channels here: %s. A renamed project channel also answers to its raw name, e.g. GameTraceChannel1."),
					*ChannelName, *DescribeCollisionChannels()));
			}

			TArray<FHitResult> Hits;
			bool bAnyHit = false;
			if (!Profile.IsEmpty())
			{
				bAnyHit = bMulti
					? (bSweep
						? World->SweepMultiByProfile(Hits, Start, End, FQuat::Identity, FName(*Profile), SweepShape, Query)
						: World->LineTraceMultiByProfile(Hits, Start, End, FName(*Profile), Query))
					: [&]
					{
						FHitResult One;
						const bool bOk = bSweep
							? World->SweepSingleByProfile(One, Start, End, FQuat::Identity, FName(*Profile), SweepShape, Query)
							: World->LineTraceSingleByProfile(One, Start, End, FName(*Profile), Query);
						if (bOk) { Hits.Add(One); }
						return bOk;
					}();
			}
			else
			{
				bAnyHit = bMulti
					? (bSweep
						? World->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, Channel, SweepShape, Query)
						: World->LineTraceMultiByChannel(Hits, Start, End, Channel, Query))
					: [&]
					{
						FHitResult One;
						const bool bOk = bSweep
							? World->SweepSingleByChannel(One, Start, End, FQuat::Identity, Channel, SweepShape, Query)
							: World->LineTraceSingleByChannel(One, Start, End, Channel, Query);
						if (bOk) { Hits.Add(One); }
						return bOk;
					}();
			}

			auto VectorJson = [](const FVector& V)
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetNumberField(TEXT("x"), V.X);
				J->SetNumberField(TEXT("y"), V.Y);
				J->SetNumberField(TEXT("z"), V.Z);
				return J;
			};

			TArray<TSharedPtr<FJsonValue>> HitJson;
			for (const FHitResult& Hit : Hits)
			{
				TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
				const AActor* HitActor = Hit.GetActor();
				H->SetStringField(TEXT("actor"), HitActor ? HitActor->GetName() : FString());
				if (HitActor)
				{
					H->SetStringField(TEXT("actorLabel"), HitActor->GetActorLabel());
					H->SetStringField(TEXT("actorClass"), HitActor->GetClass()->GetName());
				}
				H->SetStringField(TEXT("component"), Hit.GetComponent() ? Hit.GetComponent()->GetName() : FString());
				H->SetObjectField(TEXT("location"), VectorJson(Hit.ImpactPoint));
				H->SetObjectField(TEXT("normal"), VectorJson(Hit.ImpactNormal));
				H->SetNumberField(TEXT("distance"), Hit.Distance);
				H->SetBoolField(TEXT("startPenetrating"), Hit.bStartPenetrating);
				if (Hit.PhysMaterial.IsValid())
				{
					H->SetStringField(TEXT("physicalMaterial"), Hit.PhysMaterial->GetName());
				}
				if (!Hit.BoneName.IsNone())
				{
					H->SetStringField(TEXT("bone"), Hit.BoneName.ToString());
				}
				HitJson.Add(MakeShared<FJsonValueObject>(H));
			}

			if (const double DrawSeconds = GetNumber(Ctx.Params, TEXT("draw_seconds"), 0.0); DrawSeconds > 0.0)
			{
				DrawDebugLine(World, Start, End, bAnyHit ? FColor::Green : FColor::Red,
					/*bPersistentLines=*/false, static_cast<float>(DrawSeconds), 0, 2.0f);
				for (const FHitResult& Hit : Hits)
				{
					DrawDebugPoint(World, Hit.ImpactPoint, 12.0f, FColor::Yellow, false, static_cast<float>(DrawSeconds));
				}
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("hit"), bAnyHit);
			Data->SetArrayField(TEXT("hits"), HitJson);
			Data->SetObjectField(TEXT("start"), VectorJson(Start));
			Data->SetObjectField(TEXT("end"), VectorJson(End));
			return FUplinkToolResult::Ok(Data, bAnyHit
				? FString::Printf(TEXT("%d hit(s)"), HitJson.Num())
				: TEXT("no hit"));
		});

	Registry.RegisterQuick(
		TEXT("level_diff"),
		TEXT("Every property on a placed actor that differs from what its class says it should be - the details panel's yellow revert-arrow, as data. A level is mostly defaults; the overrides are the short list of decisions somebody actually made in it, and when a level worked yesterday and does not today the cause is nearly always one of them. Each row names the actor, the component when the override sits on one, the property, the value now, and the value it would otherwise have. Components are compared against their own archetype - the Blueprint's SCS template - rather than the class default, so a value the Blueprint itself sets is not reported as a level override. Placement transforms are excluded by default because nearly every placed actor overrides them and they bury everything else; pass include_transforms to see them. Editor-only components - billboards, arrows, sprites - are skipped entirely: their native constructors diverge from the bare component default on every actor of a type, so they are the same noise in every level and none of it is a decision anybody made. Long struct values are truncated. Read-only."),
		TEXT(R"json({"type":"object","properties":{"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"},"name_contains":{"type":"string","description":"Only actors whose name or label contains this"},"class_contains":{"type":"string","description":"Only actors whose class path contains this"},"include_transforms":{"type":"boolean","default":false,"description":"Include RelativeLocation/Rotation/Scale3D, which almost every placed actor overrides"},"max":{"type":"number","default":200}},"additionalProperties":false})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return FUplinkToolResult::Error(WorldError);
			}

			const FString NameFilter = GetString(Ctx.Params, TEXT("name_contains"));
			const FString ClassFilter = GetString(Ctx.Params, TEXT("class_contains"));
			bool bTransforms = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("include_transforms")), bTransforms);
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 200)), 1, 2000);

			// Placement is an override on essentially every actor ever dragged
			// into a level, so left in it is the only thing you can see.
			static const TSet<FName> TransformProps = {
				FName(TEXT("RelativeLocation")), FName(TEXT("RelativeRotation")), FName(TEXT("RelativeScale3D"))
			};

			int32 Total = 0;
			TArray<TSharedPtr<FJsonValue>> Rows;

			auto Collect = [&](UObject* Object, const FString& ActorName, const FString& ComponentName)
			{
				UObject* Archetype = Object ? Object->GetArchetype() : nullptr;
				if (!Object || !Archetype || Archetype->GetClass() != Object->GetClass())
				{
					return;
				}
				for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
				{
					const FProperty* Prop = *It;
					// Only what a person could have set: an editable, saved
					// property. Transient state differs constantly and says
					// nothing about how the level was authored.
					if (!Prop->HasAnyPropertyFlags(CPF_Edit) || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_EditConst))
					{
						continue;
					}
					if (!bTransforms && TransformProps.Contains(Prop->GetFName()))
					{
						continue;
					}
					if (Prop->Identical_InContainer(Object, Archetype))
					{
						continue;
					}

					++Total;
					if (Rows.Num() >= Max)
					{
						continue;
					}

					FString Now, Was;
					Prop->ExportTextItem_Direct(Now, Prop->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
					Prop->ExportTextItem_Direct(Was, Prop->ContainerPtrToValuePtr<void>(Archetype), nullptr, Archetype, PPF_None);

					auto Shorten = [](FString& Text)
					{
						const int32 Limit = 240;
						if (Text.Len() > Limit)
						{
							Text = Text.Left(Limit) + TEXT("...");
						}
					};
					Shorten(Now);
					Shorten(Was);

					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("actor"), ActorName);
					if (!ComponentName.IsEmpty())
					{
						Row->SetStringField(TEXT("component"), ComponentName);
					}
					Row->SetStringField(TEXT("property"), Prop->GetName());
					Row->SetStringField(TEXT("value"), Now);
					Row->SetStringField(TEXT("default"), Was);
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}
			};

			int32 ActorsScanned = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor)
				{
					continue;
				}
				const FString Name = Actor->GetName();
				const FString Label = Actor->GetActorNameOrLabel();
				if (!NameFilter.IsEmpty()
					&& !Name.Contains(NameFilter, ESearchCase::IgnoreCase)
					&& !Label.Contains(NameFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (!ClassFilter.IsEmpty() && !Actor->GetClass()->GetPathName().Contains(ClassFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}

				++ActorsScanned;
				Collect(Actor, Name, FString());
				for (UActorComponent* Component : Actor->GetComponents())
				{
					if (Component && !Component->IsEditorOnly())
					{
						Collect(Component, Name, Component->GetName());
					}
				}
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("overrides"), Rows);
			Data->SetNumberField(TEXT("total_matching"), Total);
			Data->SetNumberField(TEXT("actors_scanned"), ActorsScanned);
			if (Total > Rows.Num())
			{
				Data->SetBoolField(TEXT("truncated"), true);
			}
			return FUplinkToolResult::Ok(Data, Total == 0
				? FString::Printf(TEXT("%d actor(s) scanned, none override anything"), ActorsScanned)
				: FString::Printf(TEXT("%d override(s) across %d actor(s)"), Total, ActorsScanned));
		});

	// spawn_actor reaches a volume class and produces a volume that bounds
	// nothing: SpawnActor never runs the brush-builder step the editor's place
	// flow does, so BrushBuilder, Brush and BrushComponent.Brush all come back
	// empty. A NavMeshBoundsVolume built that way generates no navmesh, a
	// TriggerVolume overlaps nothing, and neither reports a problem - the actor
	// is there, correctly named and positioned, and simply has no shape.
	Registry.RegisterQuick(
		TEXT("spawn_volume"),
		TEXT("Spawn a volume actor with real brush geometry - a trigger volume, nav mesh bounds, blocking volume, post-process volume, anything deriving from ABrush. spawn_actor cannot do this: it calls SpawnActor, which never runs the brush-builder step the editor's placement flow does, so the volume arrives with no Brush at all. It bounds nothing, generates no navmesh, overlaps nobody, and nothing anywhere reports a fault - the actor exists and is simply shapeless. This builds a box brush of the requested size and registers it, so the volume behaves the way one dragged in from the place-actors panel does. 'size' is the full extent on each axis, defaulting to 200."),
		TEXT(R"json({"type":"object","properties":{"class_path":{"type":"string","description":"Volume class, e.g. /Script/Engine.TriggerVolume, /Script/NavigationSystem.NavMeshBoundsVolume, /Script/Engine.BlockingVolume"},"location":{"type":"object"},"rotation":{"type":"object"},"size":{"type":"object","description":"{x,y,z} full extents in world units; defaults to 200 on each axis"},"label":{"type":"string"},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["class_path"],"additionalProperties":false})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return FUplinkToolResult::Error(WorldError);
			}

			const FString ClassPath = GetString(Ctx.Params, TEXT("class_path"));
			UClass* Class = LoadClass<AActor>(nullptr, *ClassPath);
			if (!Class)
			{
				Class = FindObject<UClass>(nullptr, *ClassPath);
			}
			if (!Class)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("class not found: %s"), *ClassPath));
			}
			if (!Class->IsChildOf(ABrush::StaticClass()))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("%s is not a brush-based volume - use spawn_actor for it"), *ClassPath));
			}

			FVector Location = FVector::ZeroVector;
			TryGetVector(Ctx.Params, TEXT("location"), Location);
			FRotator Rotation = FRotator::ZeroRotator;
			TryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);

			FVector Size(200.0, 200.0, 200.0);
			TryGetVector(Ctx.Params, TEXT("size"), Size);
			// A zero on any axis makes a degenerate brush that silently bounds
			// nothing, which is the very failure this tool exists to avoid.
			Size.X = FMath::Max(1.0, FMath::Abs(Size.X));
			Size.Y = FMath::Max(1.0, FMath::Abs(Size.Y));
			Size.Z = FMath::Max(1.0, FMath::Abs(Size.Z));

			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ABrush* Volume = World->SpawnActor<ABrush>(Class, Location, Rotation, Params);
			if (!Volume)
			{
				return FUplinkToolResult::Error(TEXT("spawn failed"));
			}

			// The order matters: the model and its polygon list have to exist
			// and be hooked to the brush component before the builder runs, or
			// Build writes into nothing.
			Volume->PreEditChange(nullptr);
			Volume->Brush = NewObject<UModel>(Volume, NAME_None, RF_Transactional);
			Volume->Brush->Initialize(nullptr, true);
			Volume->Brush->Polys = NewObject<UPolys>(Volume->Brush, NAME_None, RF_Transactional);
			if (UBrushComponent* BrushComponent = Volume->GetBrushComponent())
			{
				BrushComponent->Brush = Volume->Brush;
			}

			UCubeBuilder* Builder = NewObject<UCubeBuilder>(Volume);
			Builder->X = Size.X;
			Builder->Y = Size.Y;
			Builder->Z = Size.Z;
			Volume->BrushBuilder = Builder;
			Builder->Build(World, Volume);
			Volume->PostEditChange();
			Volume->MarkPackageDirty();

			const FString Label = GetString(Ctx.Params, TEXT("label"));
			if (!Label.IsEmpty())
			{
				Volume->SetActorLabel(Label);
			}

			// Say whether the geometry actually arrived rather than assuming
			// it: a volume that reports success and holds no polygons is the
			// exact silent failure being fixed here.
			const int32 PolyCount = (Volume->Brush && Volume->Brush->Polys)
				? Volume->Brush->Polys->Element.Num() : 0;

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("name"), Volume->GetName());
			Data->SetStringField(TEXT("label"), Volume->GetActorNameOrLabel());
			Data->SetStringField(TEXT("class"), Class->GetPathName());
			TSharedRef<FJsonObject> SizeJson = MakeShared<FJsonObject>();
			SizeJson->SetNumberField(TEXT("x"), Size.X);
			SizeJson->SetNumberField(TEXT("y"), Size.Y);
			SizeJson->SetNumberField(TEXT("z"), Size.Z);
			Data->SetObjectField(TEXT("size"), SizeJson);
			Data->SetNumberField(TEXT("polygons"), PolyCount);
			if (PolyCount == 0)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("%s spawned but the brush builder produced no geometry, so it would bound nothing - the actor has been left in place for inspection"),
					*Volume->GetName()));
			}
			return FUplinkToolResult::Ok(Data, FString::Printf(
				TEXT("%s spawned with a %g x %g x %g box brush (%d polygons)"),
				*Volume->GetName(), Size.X, Size.Y, Size.Z, PolyCount));
		});

	// Opening and creating levels went through call_function on
	// LevelEditorSubsystem, and the raw calls have a trap in them: NewLevel
	// creates the asset and returns true WITHOUT making it the world the editor
	// is editing. Every spawn_actor afterwards then lands in whatever level was
	// already open, the save writes an empty level, and not one call reports a
	// problem. That cost an afternoon; these two exist so it cannot happen
	// again, because they check.
	auto CurrentMap = []() -> FString
	{
		const UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		return EditorWorld ? EditorWorld->GetOutermost()->GetName() : FString();
	};

	Registry.RegisterQuick(
		TEXT("level_open"),
		TEXT("Open a level for editing, and confirm the editor is actually on it. The confirmation is the point: the underlying engine call can report success while the editor keeps editing the level it already had, and everything placed afterwards then goes into the wrong world without a single call complaining. Returns the map the editor ended up on."),
		TEXT(R"json({"type":"object","properties":{"path":{"type":"string","description":"Level asset path, e.g. /Game/Maps/Arena"}},"required":["path"],"additionalProperties":false})json"),
		/*bReadOnly=*/false,
		[CurrentMap](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Path = GetString(Ctx.Params, TEXT("path"));
			ULevelEditorSubsystem* Levels = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
			if (!Levels)
			{
				return FUplinkToolResult::Error(TEXT("no level editor subsystem - is this a commandlet or a cooked build?"));
			}
			if (!Levels->LoadLevel(Path))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("could not open '%s' - check the path names a level asset that exists"), *Path));
			}
			const FString Now = CurrentMap();
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("map"), Now);
			if (Now != Path)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("asked to open '%s' and the editor is on '%s' - anything placed now would go into the wrong level"),
					*Path, *Now));
			}
			return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("editing %s"), *Now));
		},
		/*bTransactional=*/false);

	Registry.RegisterQuick(
		TEXT("level_new"),
		TEXT("Create an empty level AND switch the editor to it. The engine's own NewLevel does the first half only: it writes the asset, answers true, and leaves the editor editing whatever it was editing before - so actors spawned next land somewhere else entirely and the new level saves empty, silently. This creates it, opens it, and verifies the editor arrived, so a later spawn cannot go astray. It does not save; call save when the level has something in it."),
		TEXT(R"json({"type":"object","properties":{"path":{"type":"string","description":"Where to create it, e.g. /Game/Maps/Arena"},"partitioned":{"type":"boolean","default":false,"description":"World Partition. Leave false for a small fixture level."}},"required":["path"],"additionalProperties":false})json"),
		/*bReadOnly=*/false,
		[CurrentMap](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Path = GetString(Ctx.Params, TEXT("path"));
			bool bPartitioned = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("partitioned")), bPartitioned);

			ULevelEditorSubsystem* Levels = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
			if (!Levels)
			{
				return FUplinkToolResult::Error(TEXT("no level editor subsystem - is this a commandlet or a cooked build?"));
			}
			if (!Levels->NewLevel(Path, bPartitioned))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("could not create '%s' - a level may already exist there, or the path may not be writable"), *Path));
			}
			// The half the engine call leaves out.
			Levels->LoadLevel(Path);

			const FString Now = CurrentMap();
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("map"), Now);
			Data->SetBoolField(TEXT("partitioned"), bPartitioned);
			if (Now != Path)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("created '%s' but the editor is still on '%s' - actors spawned now would go into that level instead, and this one would save empty"),
					*Path, *Now));
			}
			return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("created and editing %s"), *Now));
		},
		/*bTransactional=*/false);
}
