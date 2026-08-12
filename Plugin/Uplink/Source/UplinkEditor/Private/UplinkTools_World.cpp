// Copyright (c) 2026 Low Sze Hao. MIT License.
// World tools: level_actors, spawn_actor, delete_actors, move_actor.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "LevelEditorViewport.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

using namespace UplinkToolUtil;

void UplinkTools::RegisterWorld(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("level_actors"),
		TEXT("List actors in the current world (PIE world when a session is active, else the editor world). Supports substring filters."),
		TEXT(R"json({"type":"object","properties":{"world":{"type":"string","enum":["editor","pie"]},"class_contains":{"type":"string"},"name_contains":{"type":"string","description":"Matches actor name OR editor label"},"max":{"type":"number","default":200}}})json"),
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
		TEXT(R"json({"type":"object","properties":{"class_path":{"type":"string"},"location":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"rotation":{"type":"object","properties":{"pitch":{"type":"number"},"yaw":{"type":"number"},"roll":{"type":"number"}}},"label":{"type":"string","description":"Editor label (editor world only)"},"world":{"type":"string","enum":["editor","pie"]}},"required":["class_path"]})json"),
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
		TEXT(R"json({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"world":{"type":"string","enum":["editor","pie"]}},"required":["names"]})json"),
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
		TEXT(R"json({"type":"object","properties":{"actor":{"type":"string"},"location":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"rotation":{"type":"object","properties":{"pitch":{"type":"number"},"yaw":{"type":"number"},"roll":{"type":"number"}}},"scale":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"world":{"type":"string","enum":["editor","pie"]}},"required":["actor"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString WorldError;
			UWorld* World = Ctx.ResolveWorld(WorldError);
			if (!World)
			{
				return FUplinkToolResult::Error(WorldError);
			}

			AActor* Actor = FindActor(World, GetString(Ctx.Params, TEXT("actor")));
			if (!Actor)
			{
				return FUplinkToolResult::Error(TEXT("actor not found"));
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
					return FUplinkToolResult::Error(FString::Printf(TEXT("actor not found: %s"), *FocusName));
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
}
