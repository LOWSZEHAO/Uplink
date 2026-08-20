// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Material tools: material_query - read a material's graph and, above all, its
// compile errors. Authoring a material blind is how a missing texture input
// turns into a black surface with nothing in the log to explain it.

#include "UplinkContentTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "RHIDefinitions.h"

using namespace UplinkToolUtil;

namespace
{
	UMaterialInterface* LoadMaterialInterface(const FString& Path, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = TEXT("'material' is required");
			return nullptr;
		}
		UObject* Found = StaticFindObject(UObject::StaticClass(), nullptr, *Path);
		if (!Found)
		{
			Found = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
		}
		if (!Found)
		{
			OutError = FString::Printf(
				TEXT("no material at '%s' (an asset path looks like /Game/Folder/M_Name.M_Name)"), *Path);
			return nullptr;
		}
		UMaterialInterface* AsMaterial = Cast<UMaterialInterface>(Found);
		if (!AsMaterial)
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, not a material"),
				*Path, *Found->GetClass()->GetName());
		}
		return AsMaterial;
	}

	/** Which named inputs of the material's output node actually have something plugged in. */
	void DescribeConnections(UMaterial* Material, TSharedRef<FJsonObject> Out)
	{
		if (!Material)
		{
			return;
		}
		// Ask the editing library rather than reaching into the property chain,
		// so this keeps working as the material output set changes per version.
		static const EMaterialProperty Properties[] = {
			MP_BaseColor, MP_Metallic, MP_Specular, MP_Roughness, MP_Anisotropy,
			MP_EmissiveColor, MP_Opacity, MP_OpacityMask, MP_Normal, MP_Tangent,
			MP_WorldPositionOffset, MP_SubsurfaceColor, MP_AmbientOcclusion, MP_Refraction,
		};
		const UEnum* PropertyEnum = StaticEnum<EMaterialProperty>();

		TArray<TSharedPtr<FJsonValue>> Connected;
		TArray<TSharedPtr<FJsonValue>> Unconnected;
		for (const EMaterialProperty Property : Properties)
		{
			FString Name = PropertyEnum ? PropertyEnum->GetNameStringByValue(Property) : FString();
			Name.RemoveFromStart(TEXT("MP_"));
			const bool bHasInput = Material->GetExpressionInputForProperty(Property)
				&& Material->GetExpressionInputForProperty(Property)->IsConnected();
			(bHasInput ? Connected : Unconnected).Add(MakeShared<FJsonValueString>(Name));
		}
		Out->SetArrayField(TEXT("connectedInputs"), Connected);
		Out->SetArrayField(TEXT("unconnectedInputs"), Unconnected);
	}
}

void UplinkContentTools::RegisterMaterial(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("material_query"),
		TEXT("Read a material: its expressions, which output inputs are connected, its parameters, and - the reason this exists - its compile errors. A material that fails to compile renders black or default-grey with nothing useful in the log, so 'errors' is the first thing to check when a surface looks wrong. Works on a Material or a Material Instance; an instance reports its parent, and every parameter row carries \"overridden\" saying whether this instance sets it or inherits it. 'recompile' forces a rebuild first so errors are current."),
		TEXT(R"json({"type":"object","properties":{"material":{"type":"string","description":"Asset path, e.g. /Game/Mats/M_Rock.M_Rock"},"expressions":{"type":"boolean","default":true},"parameters":{"type":"boolean","default":true},"recompile":{"type":"boolean","default":false},"max":{"type":"number","default":80}},"required":["material"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UMaterialInterface* MaterialInterface = LoadMaterialInterface(GetString(Ctx.Params, TEXT("material")), Error);
			if (!MaterialInterface)
			{
				return FUplinkToolResult::Error(Error);
			}

			bool bRecompile = false;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("recompile")), bRecompile);
			if (bRecompile)
			{
				UMaterialEditingLibrary::RecompileMaterial(MaterialInterface->GetMaterial());
			}

			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 80.0)), 1, 400);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("material"), MaterialInterface->GetPathName());
			Data->SetStringField(TEXT("class"), MaterialInterface->GetClass()->GetName());

			UMaterial* BaseMaterial = MaterialInterface->GetMaterial();

			// Compile errors first: this is the answer to "why is it black".
			// They live on the compiled resource, not the asset, so go through
			// GetMaterialResource for the platform actually being rendered.
			TArray<TSharedPtr<FJsonValue>> Errors;
			if (BaseMaterial)
			{
				if (const FMaterialResource* Resource = BaseMaterial->GetMaterialResource(GMaxRHIShaderPlatform))
				{
					for (const FString& Message : Resource->GetCompileErrors())
					{
						Errors.Add(MakeShared<FJsonValueString>(Message));
					}
				}
			}
			Data->SetArrayField(TEXT("errors"), Errors);
			Data->SetNumberField(TEXT("errorCount"), Errors.Num());

			if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(MaterialInterface))
			{
				// A null parent is the classic cause of a grey instance: the
				// asset loads fine and simply has nothing to inherit from.
				Data->SetStringField(TEXT("parent"),
					Instance->Parent ? Instance->Parent->GetPathName() : TEXT(""));
				if (!Instance->Parent)
				{
					Data->SetStringField(TEXT("warning"),
						TEXT("this instance has no parent, so it renders as default material - set Parent to a real material"));
				}
			}

			bool bWantExpressions = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("expressions")), bWantExpressions);
			if (bWantExpressions && BaseMaterial)
			{
				DescribeConnections(BaseMaterial, Data);

				TArray<TSharedPtr<FJsonValue>> Expressions;
				const TConstArrayView<TObjectPtr<UMaterialExpression>> All = BaseMaterial->GetExpressions();
				for (const TObjectPtr<UMaterialExpression>& Expression : All)
				{
					if (Expressions.Num() >= Max)
					{
						break;
					}
					if (!Expression)
					{
						continue;
					}
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Expression->GetName());
					Row->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
					const FString Description = Expression->GetDescription();
					if (!Description.IsEmpty())
					{
						Row->SetStringField(TEXT("description"), Description);
					}
					Expressions.Add(MakeShared<FJsonValueObject>(Row));
				}
				Data->SetArrayField(TEXT("expressions"), Expressions);
				Data->SetNumberField(TEXT("expressionCount"), All.Num());
				Data->SetBoolField(TEXT("expressionsTruncated"), All.Num() > Expressions.Num());
			}

			bool bWantParameters = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("parameters")), bWantParameters);
			if (bWantParameters)
			{
				TArray<TSharedPtr<FJsonValue>> Parameters;

				// The list is the PARENT's declared parameters - every one an
				// instance could set - and the value read back is the effective
				// one, inherited or not. Which of them this instance actually
				// overrides lives only in its own *ParameterValues arrays, so
				// without marking them the reply cannot answer the question the
				// description promises: an instance overriding one of a parent's
				// two dozen parameters looked identical to one overriding all of
				// them.
				UMaterialInstance* AsInstance = Cast<UMaterialInstance>(MaterialInterface);
				auto IsOverridden = [AsInstance](const TCHAR* Kind, const FMaterialParameterInfo& Info) -> bool
				{
					if (!AsInstance)
					{
						return false;
					}
					if (FCString::Strcmp(Kind, TEXT("scalar")) == 0)
					{
						return AsInstance->ScalarParameterValues.ContainsByPredicate(
							[&Info](const FScalarParameterValue& V) { return V.ParameterInfo == Info; });
					}
					if (FCString::Strcmp(Kind, TEXT("vector")) == 0)
					{
						return AsInstance->VectorParameterValues.ContainsByPredicate(
							[&Info](const FVectorParameterValue& V) { return V.ParameterInfo == Info; });
					}
					return AsInstance->TextureParameterValues.ContainsByPredicate(
						[&Info](const FTextureParameterValue& V) { return V.ParameterInfo == Info; });
				};

				auto AddParameters = [&Parameters, MaterialInterface, Max, AsInstance, IsOverridden](const TCHAR* Kind, const TArray<FMaterialParameterInfo>& Infos)
				{
					for (const FMaterialParameterInfo& Info : Infos)
					{
						if (Parameters.Num() >= Max)
						{
							return;
						}
						TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
						Row->SetStringField(TEXT("name"), Info.Name.ToString());
						Row->SetStringField(TEXT("kind"), Kind);
						if (AsInstance)
						{
							Row->SetBoolField(TEXT("overridden"), IsOverridden(Kind, Info));
						}

						if (FLinearColor AsColor; FCString::Strcmp(Kind, TEXT("vector")) == 0
							&& MaterialInterface->GetVectorParameterValue(Info, AsColor))
						{
							TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
							Value->SetNumberField(TEXT("r"), AsColor.R);
							Value->SetNumberField(TEXT("g"), AsColor.G);
							Value->SetNumberField(TEXT("b"), AsColor.B);
							Value->SetNumberField(TEXT("a"), AsColor.A);
							Row->SetObjectField(TEXT("value"), Value);
						}
						else if (float AsScalar = 0.0f; FCString::Strcmp(Kind, TEXT("scalar")) == 0
							&& MaterialInterface->GetScalarParameterValue(Info, AsScalar))
						{
							Row->SetNumberField(TEXT("value"), AsScalar);
						}
						else if (UTexture* AsTexture = nullptr; FCString::Strcmp(Kind, TEXT("texture")) == 0
							&& MaterialInterface->GetTextureParameterValue(Info, AsTexture))
						{
							Row->SetStringField(TEXT("value"), AsTexture ? AsTexture->GetPathName() : TEXT(""));
						}
						Parameters.Add(MakeShared<FJsonValueObject>(Row));
					}
				};

				TArray<FMaterialParameterInfo> Infos;
				TArray<FGuid> Guids;
				MaterialInterface->GetAllScalarParameterInfo(Infos, Guids);
				AddParameters(TEXT("scalar"), Infos);
				MaterialInterface->GetAllVectorParameterInfo(Infos, Guids);
				AddParameters(TEXT("vector"), Infos);
				MaterialInterface->GetAllTextureParameterInfo(Infos, Guids);
				AddParameters(TEXT("texture"), Infos);

				Data->SetArrayField(TEXT("parameters"), Parameters);
			}

			return FUplinkToolResult::Ok(Data, Errors.Num() > 0
				? FString::Printf(TEXT("%d compile error(s)"), Errors.Num())
				: FString());
		});
}
