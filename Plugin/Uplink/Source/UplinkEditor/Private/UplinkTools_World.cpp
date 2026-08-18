// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// World tools: worlds, level_actors, spawn_actor, delete_actors, move_actor,
// spawn_batch, viewport_camera, trace.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "LevelEditorViewport.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/Actor.h"
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
}

void UplinkTools::RegisterWorld(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("worlds"),
		TEXT("List every live world and the id that addresses it: the level being edited ('editor'), one per PIE instance ('pie:0', 'pie:1' when playing as several clients), and the preview world behind an open asset editor ('preview:World_3'). Pass an id as the 'world' parameter of any world tool to hit exactly that one - 'pie' on its own only ever means the instance currently in play. 'is_default' marks the world an omitted 'world' goes to right now."),
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
		TEXT(R"json({"type":"object","properties":{"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"},"class_contains":{"type":"string"},"name_contains":{"type":"string","description":"Matches actor name OR editor label"},"max":{"type":"number","default":200}}})json"),
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
				return FUplinkToolResult::Error(TEXT("SpawnActor failed"));
			}

#if WITH_EDITOR
			const FString Label = GetString(Ctx.Params, TEXT("label"));
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
			Data->SetObjectField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("delete_actors"),
		TEXT("Destroy actors by exact name or editor label."),
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
			for (const TSharedPtr<FJsonValue>& NameValue : *Names)
			{
				const FString Name = NameValue->AsString();
				if (AActor* Actor = FindActor(World, Name))
				{
					World->DestroyActor(Actor);
					++Destroyed;
				}
				else
				{
					Missing.Add(Name);
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
			return FUplinkToolResult::Ok(Data);
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
			if (TryGetVector(Ctx.Params, TEXT("location"), Location))
			{
				Actor->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
			}
			FRotator Rotation;
			if (TryGetRotator(Ctx.Params, TEXT("rotation"), Rotation))
			{
				Actor->SetActorRotation(Rotation, ETeleportType::TeleportPhysics);
			}
			FVector Scale;
			if (TryGetVector(Ctx.Params, TEXT("scale"), Scale))
			{
				Actor->SetActorScale3D(Scale);
			}
			if (!Ctx.IsPieWorld())
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
		TEXT("Spawn many actors in one call - the scene-assembly workhorse. Each entry: {mesh: static mesh path (spawns a StaticMeshActor with it) OR class_path, location, rotation?, scale?, label?, material?}. A city block, a prop set, or a whole layout lands in a single request. Returns each spawned actor's name."),
		TEXT(R"json({"type":"object","properties":{"actors":{"type":"array","items":{"type":"object"}},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["actors"]})json"),
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
				return FUplinkToolResult::Error(TEXT("max 1000 actors per call"));
			}

			UClass* StaticMeshActorClass = StaticLoadClass(AActor::StaticClass(), nullptr, TEXT("/Script/Engine.StaticMeshActor"));
			TArray<TSharedPtr<FJsonValue>> Spawned;
			for (int32 Index = 0; Index < Specs->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* Spec = nullptr;
				if (!(*Specs)[Index]->TryGetObject(Spec) || !Spec->IsValid())
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("actors[%d] is not an object - %d spawned before the failure"), Index, Spawned.Num()));
				}

				FVector Location = FVector::ZeroVector;
				TryGetVector(*Spec, TEXT("location"), Location);
				FRotator Rotation = FRotator::ZeroRotator;
				TryGetRotator(*Spec, TEXT("rotation"), Rotation);

				const FString MeshPath = GetString(*Spec, TEXT("mesh"));
				UClass* Class = StaticMeshActorClass;
				if (MeshPath.IsEmpty())
				{
					Class = StaticLoadClass(AActor::StaticClass(), nullptr, *GetString(*Spec, TEXT("class_path")));
					if (!Class)
					{
						return FUplinkToolResult::Error(FString::Printf(TEXT("actors[%d]: provide 'mesh' or a valid 'class_path' - %d spawned before the failure"), Index, Spawned.Num()));
					}
				}

				FActorSpawnParameters SpawnParams;
				SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				AActor* Actor = World->SpawnActor<AActor>(Class, Location, Rotation, SpawnParams);
				if (!Actor)
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("actors[%d]: spawn failed - %d spawned before the failure"), Index, Spawned.Num()));
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
					if (UStaticMeshComponent* MeshComponent = Actor->FindComponentByClass<UStaticMeshComponent>())
					{
						UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
						if (!Mesh)
						{
							return FUplinkToolResult::Error(FString::Printf(TEXT("actors[%d]: mesh not found: %s"), Index, *MeshPath));
						}
						MeshComponent->SetMobility(EComponentMobility::Movable);
						MeshComponent->SetStaticMesh(Mesh);
						const FString MaterialPath = GetString(*Spec, TEXT("material"));
						if (!MaterialPath.IsEmpty())
						{
							UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
							if (!Material)
							{
								// Same policy as the mesh above. Skipping this
								// quietly produced a batch of default-grey actors
								// reported as spawned, which is exactly the kind
								// of success-for-work-not-done this refuses to do.
								return FUplinkToolResult::Error(FString::Printf(
									TEXT("actors[%d]: material not found: %s (%d spawned before the failure)"),
									Index, *MaterialPath, Spawned.Num()));
							}
							MeshComponent->SetMaterial(0, Material);
						}
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
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("trace"),
		TEXT("Ask the physics scene what is there. Casts a ray (or a swept sphere/box/capsule with 'radius'/'half_height') and reports what it hit: actor, component, impact point, normal, distance, physical material. 'to' or 'direction'+'distance' set the end point. 'profile' traces by collision profile (e.g. 'Azr_Collision'), otherwise 'channel' names a trace channel ('Visibility', 'Camera', or a project channel). 'multi' returns every hit along the ray instead of the first. Use this instead of guessing geometry - a downward trace is how you find ground height under a point."),
		TEXT(R"json({"type":"object","properties":{"from":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"to":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"direction":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"distance":{"type":"number","default":10000},"shape":{"type":"string","enum":["line","sphere","box","capsule"],"default":"line"},"radius":{"type":"number","default":50},"half_height":{"type":"number","default":100},"channel":{"type":"string","default":"Visibility"},"profile":{"type":"string"},"multi":{"type":"boolean","default":false},"complex":{"type":"boolean","default":false},"ignore_actors":{"type":"array","items":{"type":"string"}},"draw_seconds":{"type":"number","default":0,"description":"Draw the trace in the world for this many seconds"},"world":{"type":"string","description":"'editor', 'pie', or an id from the worlds tool (e.g. 'pie:1')"}},"required":["from"]})json"),
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
					if (Entry.IsValid() && Entry->TryGetString(ActorName))
					{
						if (AActor* Found = FindActor(World, ActorName))
						{
							Query.AddIgnoredActor(Found);
						}
					}
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
			if (Profile.IsEmpty())
			{
				// Accept both the friendly names and the raw ECC_ enum names.
				const UEnum* ChannelEnum = StaticEnum<ECollisionChannel>();
				int64 Value = ChannelEnum ? ChannelEnum->GetValueByNameString(FString::Printf(TEXT("ECC_%s"), *ChannelName)) : INDEX_NONE;
				if (Value == INDEX_NONE && ChannelEnum)
				{
					Value = ChannelEnum->GetValueByNameString(ChannelName);
				}
				if (Value == INDEX_NONE)
				{
					Value = UEngineTypes::ConvertToCollisionChannel(
						ChannelName == TEXT("Camera") ? ETraceTypeQuery::TraceTypeQuery2 : ETraceTypeQuery::TraceTypeQuery1);
				}
				Channel = static_cast<ECollisionChannel>(Value);
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
}
