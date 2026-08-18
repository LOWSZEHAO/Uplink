// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Environment tools: lighting_setup (one-call scene lighting stack),
// landscape_create (heightmap file -> Landscape actor), foliage_scatter
// (instanced meshes scattered onto the ground).

#include "UplinkContentTools.h"
#include "UplinkCompat.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "JsonObjectConverter.h"
#include "Landscape.h"
#include "Materials/MaterialInterface.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

using namespace UplinkToolUtil;

namespace
{
	UWorld* EditorWorld(FString& OutError)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			OutError = TEXT("no editor world");
		}
		return World;
	}

	/** Apply a name->JSON map to an actor, falling back to its components. */
	void ApplyActorProperties(AActor* Actor, const TSharedPtr<FJsonObject>& Properties,
		TArray<FString>& OutApplied, TArray<FString>& OutFailed)
	{
		for (const auto& Pair : Properties->Values)
		{
			const FString Name = UplinkCompat::JsonKeyToString(Pair.Key);
			bool bDone = false;

			if (FProperty* Property = FindFProperty<FProperty>(Actor->GetClass(), *Name))
			{
				Actor->Modify();
				bDone = FJsonObjectConverter::JsonValueToUProperty(
					Pair.Value, Property, Property->ContainerPtrToValuePtr<void>(Actor));
			}
			if (!bDone)
			{
				for (UActorComponent* Component : Actor->GetComponents())
				{
					FProperty* Property = Component ? FindFProperty<FProperty>(Component->GetClass(), *Name) : nullptr;
					if (Property)
					{
						Component->Modify();
						if (FJsonObjectConverter::JsonValueToUProperty(
							Pair.Value, Property, Property->ContainerPtrToValuePtr<void>(Component)))
						{
							bDone = true;
							Component->MarkRenderStateDirty();
						}
						break;
					}
				}
			}
			(bDone ? OutApplied : OutFailed).Add(Name);
		}
		Actor->MarkComponentsRenderStateDirty();
	}

	/** Load a heightmap file as uint16 samples: 8/16-bit grayscale PNG, or raw .r16. */
	bool LoadHeightmap(const FString& File, int32& OutWidth, int32& OutHeight,
		TArray<uint16>& OutSamples, FString& OutError)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File))
		{
			OutError = FString::Printf(TEXT("could not read %s"), *File);
			return false;
		}

		if (File.EndsWith(TEXT(".r16")))
		{
			const int64 Samples = Bytes.Num() / 2;
			const int32 Side = FMath::RoundToInt(FMath::Sqrt(static_cast<double>(Samples)));
			if (static_cast<int64>(Side) * Side != Samples)
			{
				OutError = TEXT(".r16 heightmaps must be square (raw little-endian uint16)");
				return false;
			}
			OutWidth = OutHeight = Side;
			OutSamples.SetNumUninitialized(Samples);
			FMemory::Memcpy(OutSamples.GetData(), Bytes.GetData(), Samples * 2);
			return true;
		}

		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const EImageFormat Format = Module.DetectImageFormat(Bytes.GetData(), Bytes.Num());
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(Format);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
		{
			OutError = TEXT("unsupported image format (use 8/16-bit grayscale PNG or raw .r16)");
			return false;
		}
		OutWidth = Wrapper->GetWidth();
		OutHeight = Wrapper->GetHeight();

		TArray64<uint8> Raw;
		if (Wrapper->GetBitDepth() >= 16 && Wrapper->GetRaw(ERGBFormat::Gray, 16, Raw))
		{
			OutSamples.SetNumUninitialized(OutWidth * OutHeight);
			FMemory::Memcpy(OutSamples.GetData(), Raw.GetData(), OutSamples.Num() * 2);
			return true;
		}
		if (Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
		{
			OutSamples.SetNumUninitialized(OutWidth * OutHeight);
			for (int32 Index = 0; Index < OutSamples.Num(); ++Index)
			{
				OutSamples[Index] = static_cast<uint16>(Raw[Index * 4 + 2]) << 8; // red channel
			}
			return true;
		}
		OutError = TEXT("could not decode heightmap pixels");
		return false;
	}

	/** Bilinear-resample a uint16 grid to new dimensions. */
	void ResampleHeights(const TArray<uint16>& In, int32 InW, int32 InH,
		int32 OutW, int32 OutH, TArray<uint16>& Out)
	{
		Out.SetNumUninitialized(OutW * OutH);
		for (int32 Y = 0; Y < OutH; ++Y)
		{
			const double SrcY = (OutH > 1) ? static_cast<double>(Y) * (InH - 1) / (OutH - 1) : 0.0;
			const int32 Y0 = FMath::Clamp(static_cast<int32>(SrcY), 0, InH - 1);
			const int32 Y1 = FMath::Min(Y0 + 1, InH - 1);
			const double FY = SrcY - Y0;
			for (int32 X = 0; X < OutW; ++X)
			{
				const double SrcX = (OutW > 1) ? static_cast<double>(X) * (InW - 1) / (OutW - 1) : 0.0;
				const int32 X0 = FMath::Clamp(static_cast<int32>(SrcX), 0, InW - 1);
				const int32 X1 = FMath::Min(X0 + 1, InW - 1);
				const double FX = SrcX - X0;
				const double Top = In[Y0 * InW + X0] * (1.0 - FX) + In[Y0 * InW + X1] * FX;
				const double Bottom = In[Y1 * InW + X0] * (1.0 - FX) + In[Y1 * InW + X1] * FX;
				Out[Y * OutW + X] = static_cast<uint16>(FMath::Clamp(Top * (1.0 - FY) + Bottom * FY, 0.0, 65535.0));
			}
		}
	}
}

void UplinkContentTools::RegisterEnvironment(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("lighting_setup"),
		TEXT("Set up a scene's lighting stack in one call. Ensures the standard actors exist (spawning any that are missing) and applies your settings to each: sections 'sun' (DirectionalLight), 'sky_light', 'sky_atmosphere', 'fog' (ExponentialHeightFog), 'clouds' (VolumetricCloud), 'post_process' (unbound PostProcessVolume). Each section takes {rotation?: actor rotation, properties?: name->JSON applied to the actor or its components - e.g. sun {\"Intensity\":3,\"LightColor\":{\"R\":255,\"G\":180,\"B\":120,\"A\":255}}, post_process {\"Settings\":{\"bOverride_AutoExposureBias\":true,\"AutoExposureBias\":0.5},\"bUnbound\":true}}. The style knowledge is yours; this is the atomic apply."),
		TEXT(R"json({"type":"object","properties":{"sun":{"type":"object"},"sky_light":{"type":"object"},"sky_atmosphere":{"type":"object"},"fog":{"type":"object"},"clouds":{"type":"object"},"post_process":{"type":"object"}}})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = EditorWorld(Error);
			if (!World)
			{
				return FUplinkToolResult::Error(Error);
			}

			struct FSection { const TCHAR* Key; const TCHAR* ClassPath; };
			static const FSection Sections[] =
			{
				{ TEXT("sun"), TEXT("/Script/Engine.DirectionalLight") },
				{ TEXT("sky_light"), TEXT("/Script/Engine.SkyLight") },
				{ TEXT("sky_atmosphere"), TEXT("/Script/Engine.SkyAtmosphere") },
				{ TEXT("fog"), TEXT("/Script/Engine.ExponentialHeightFog") },
				{ TEXT("clouds"), TEXT("/Script/Engine.VolumetricCloud") },
				{ TEXT("post_process"), TEXT("/Script/Engine.PostProcessVolume") },
			};

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			for (const FSection& Section : Sections)
			{
				const TSharedPtr<FJsonObject>* SectionParams = nullptr;
				if (!Ctx.Params->TryGetObjectField(FStringView(Section.Key), SectionParams) || !SectionParams->IsValid())
				{
					continue;
				}
				UClass* Class = StaticLoadClass(AActor::StaticClass(), nullptr, Section.ClassPath);
				if (!Class)
				{
					continue;
				}

				AActor* Actor = nullptr;
				for (TActorIterator<AActor> It(World, Class); It; ++It)
				{
					Actor = *It;
					break;
				}
				bool bSpawned = false;
				if (!Actor)
				{
					Actor = World->SpawnActor<AActor>(Class, FVector(0, 0, 500), FRotator::ZeroRotator);
					bSpawned = Actor != nullptr;
				}
				if (!Actor)
				{
					continue;
				}

				FRotator Rotation;
				if (TryGetRotator(*SectionParams, TEXT("rotation"), Rotation))
				{
					Actor->Modify();
					Actor->SetActorRotation(Rotation);
				}
				TArray<FString> Applied, Failed;
				const TSharedPtr<FJsonObject>* Properties = nullptr;
				if ((*SectionParams)->TryGetObjectField(FStringView(TEXT("properties")), Properties) && Properties->IsValid())
				{
					ApplyActorProperties(Actor, *Properties, Applied, Failed);
				}

				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("actor"), Actor->GetName());
				Row->SetBoolField(TEXT("spawned"), bSpawned);
				if (Failed.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> FailedRows;
					for (const FString& Entry : Failed) { FailedRows.Add(MakeShared<FJsonValueString>(Entry)); }
					Row->SetArrayField(TEXT("failed"), FailedRows);
				}
				Data->SetObjectField(Section.Key, Row);
			}
			World->MarkPackageDirty();
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("landscape_create"),
		TEXT("Create a real Landscape from a heightmap file (8/16-bit grayscale PNG, or square raw .r16 little-endian; 32768 = zero height). The heightmap is resampled to the nearest valid landscape size. 'scale' defaults to 100,100,100 (x/y: cm per quad, z: 100 = +-256m over the 16-bit range). Generate the heightmap yourself (or use real DEM data) - this is the atomic import. Runs in the editor world."),
		TEXT(R"json({"type":"object","properties":{"heightmap_file":{"type":"string"},"location":{"type":"object"},"scale":{"type":"object"},"material":{"type":"string"}},"required":["heightmap_file"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = EditorWorld(Error);
			if (!World)
			{
				return FUplinkToolResult::Error(Error);
			}

			int32 Width = 0, Height = 0;
			TArray<uint16> Samples;
			if (!LoadHeightmap(GetString(Ctx.Params, TEXT("heightmap_file")), Width, Height, Samples, Error))
			{
				return FUplinkToolResult::Error(Error);
			}

			// Snap to a valid landscape layout: N components of 63 quads (1 section).
			constexpr int32 QuadsPerSection = 63;
			const int32 ComponentsX = FMath::Clamp((Width - 1 + QuadsPerSection / 2) / QuadsPerSection, 1, 32);
			const int32 ComponentsY = FMath::Clamp((Height - 1 + QuadsPerSection / 2) / QuadsPerSection, 1, 32);
			const int32 VertsX = ComponentsX * QuadsPerSection + 1;
			const int32 VertsY = ComponentsY * QuadsPerSection + 1;
			TArray<uint16> Resampled;
			ResampleHeights(Samples, Width, Height, VertsX, VertsY, Resampled);

			FVector Location = FVector::ZeroVector;
			TryGetVector(Ctx.Params, TEXT("location"), Location);
			FVector Scale(100, 100, 100);
			TryGetVector(Ctx.Params, TEXT("scale"), Scale);

			ALandscape* Landscape = World->SpawnActor<ALandscape>(ALandscape::StaticClass(), Location, FRotator::ZeroRotator);
			if (!Landscape)
			{
				return FUplinkToolResult::Error(TEXT("could not spawn the landscape actor"));
			}
			Landscape->SetActorScale3D(Scale);

			const FString MaterialPath = GetString(Ctx.Params, TEXT("material"));
			if (!MaterialPath.IsEmpty())
			{
				if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath))
				{
					Landscape->LandscapeMaterial = Material;
				}
			}

			TMap<FGuid, TArray<uint16>> HeightData;
			HeightData.Add(FGuid(), MoveTemp(Resampled));
			TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayers;
			MaterialLayers.Add(FGuid(), {});

			Landscape->Import(FGuid::NewGuid(), 0, 0, VertsX - 1, VertsY - 1,
				/*NumSubsections=*/1, QuadsPerSection, HeightData, nullptr,
				MaterialLayers, ELandscapeImportAlphamapType::Additive, {});

			Landscape->PostEditChange();
			World->MarkPackageDirty();

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("landscape"), Landscape->GetName());
			Data->SetNumberField(TEXT("verts_x"), VertsX);
			Data->SetNumberField(TEXT("verts_y"), VertsY);
			Data->SetNumberField(TEXT("size_x_cm"), (VertsX - 1) * Scale.X);
			Data->SetNumberField(TEXT("size_y_cm"), (VertsY - 1) * Scale.Y);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("foliage_scatter"),
		TEXT("Scatter instances of a static mesh over a circular area, tracing each point down onto whatever ground is there - rocks, plants, props. One instanced-mesh actor is created (cheap to render, easy to delete). {mesh, count, center, radius, min_scale?, max_scale?, random_yaw?, seed?}"),
		TEXT(R"json({"type":"object","properties":{"mesh":{"type":"string"},"count":{"type":"number","default":50},"center":{"type":"object"},"radius":{"type":"number","default":1000},"min_scale":{"type":"number","default":0.8},"max_scale":{"type":"number","default":1.4},"random_yaw":{"type":"boolean","default":true},"seed":{"type":"number","default":1}},"required":["mesh","center"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UWorld* World = Ctx.ResolveWorld(Error);
			if (!World)
			{
				return FUplinkToolResult::Error(Error);
			}
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *GetString(Ctx.Params, TEXT("mesh")));
			if (!Mesh)
			{
				return FUplinkToolResult::Error(TEXT("static mesh not found"));
			}
			FVector Center;
			if (!TryGetVector(Ctx.Params, TEXT("center"), Center))
			{
				return FUplinkToolResult::Error(TEXT("'center' {x,y,z} is required"));
			}

			const int32 Count = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("count"), 50)), 1, 5000);
			const double Radius = FMath::Max(GetNumber(Ctx.Params, TEXT("radius"), 1000.0), 10.0);
			const double MinScale = GetNumber(Ctx.Params, TEXT("min_scale"), 0.8);
			const double MaxScale = GetNumber(Ctx.Params, TEXT("max_scale"), 1.4);
			bool bRandomYaw = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("random_yaw")), bRandomYaw);
			FRandomStream Random(static_cast<int32>(GetNumber(Ctx.Params, TEXT("seed"), 1)));

			AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator);
			if (!Holder)
			{
				return FUplinkToolResult::Error(TEXT("could not spawn the holder actor"));
			}
#if WITH_EDITOR
			Holder->SetActorLabel(TEXT("UplinkFoliage"));
#endif
			UInstancedStaticMeshComponent* Instances = NewObject<UInstancedStaticMeshComponent>(Holder);
			Holder->SetRootComponent(Instances);
			Instances->SetMobility(EComponentMobility::Movable);
			Instances->RegisterComponent();
			Instances->SetStaticMesh(Mesh);
			Holder->AddInstanceComponent(Instances);

			int32 Placed = 0;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const double Angle = Random.FRand() * 2.0 * PI;
				const double Distance = FMath::Sqrt(Random.FRand()) * Radius;
				const FVector Column(Center.X + FMath::Cos(Angle) * Distance,
					Center.Y + FMath::Sin(Angle) * Distance, Center.Z);

				FHitResult Hit;
				if (!World->LineTraceSingleByChannel(Hit,
					Column + FVector(0, 0, 100000), Column - FVector(0, 0, 100000), ECC_Visibility))
				{
					continue;
				}
				FTransform Transform;
				Transform.SetLocation(Hit.ImpactPoint);
				if (bRandomYaw)
				{
					Transform.SetRotation(FQuat(FRotator(0, Random.FRandRange(0, 360), 0)));
				}
				Transform.SetScale3D(FVector(Random.FRandRange(MinScale, MaxScale)));
				Instances->AddInstance(Transform, /*bWorldSpace=*/true);
				++Placed;
			}
			if (!Ctx.IsPieWorld())
			{
				World->MarkPackageDirty();
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("actor"), Holder->GetName());
			Data->SetNumberField(TEXT("placed"), Placed);
			Data->SetNumberField(TEXT("missed_ground"), Count - Placed);
			return FUplinkToolResult::Ok(Data);
		});
}
