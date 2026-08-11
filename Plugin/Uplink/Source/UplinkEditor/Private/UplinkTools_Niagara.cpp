// Copyright (c) 2026 Low Sze Hao. MIT License.
// Niagara authoring tools, built on UNiagaraExternalEditUtilities (new in
// UE 5.8, exported but marked experimental by Epic - expect churn in 5.9).
// On UE 5.7 the stack-editing tools report unsupported; user-parameter
// setting works on both versions.

#include "UplinkTools.h"
#include "UplinkCompat.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "JsonObjectConverter.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

#if UPLINK_UE_AT_LEAST(5, 8)
#include "NiagaraExternalSystemEditorUtilities.h"
#endif

using namespace UplinkToolUtil;

namespace
{
	UNiagaraSystem* LoadSystem(const FUplinkToolContext& Ctx, FString& OutError)
	{
		const FString Path = GetString(Ctx.Params, TEXT("asset"));
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Path);
		if (!System)
		{
			OutError = FString::Printf(TEXT("Niagara system not found: %s"), *Path);
		}
		return System;
	}

	template <typename TStruct>
	TSharedRef<FJsonObject> StructToJson(const TStruct& Value)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		FJsonObjectConverter::UStructToJsonObject(TStruct::StaticStruct(), &Value, Json, 0, 0);
		return Json;
	}

#if UPLINK_UE_AT_LEAST(5, 8)
	FUplinkToolResult ContextErrors(const FNiagaraExternalEditContext& Context, const TCHAR* What)
	{
		TArray<FString> Messages;
		for (const FText& ErrorText : Context.Errors)
		{
			Messages.Add(ErrorText.ToString());
		}
		return FUplinkToolResult::Error(FString::Printf(TEXT("%s failed: %s"), What, *FString::Join(Messages, TEXT(" | "))));
	}

	FNiagaraExt_StackItemReference MakeRef(UNiagaraSystem* System, const FUplinkToolContext& Ctx)
	{
		FNiagaraExt_StackItemReference Ref;
		Ref.System = System;
		Ref.EmitterName = FName(*GetString(Ctx.Params, TEXT("emitter")));
		Ref.ScriptName = FName(*GetString(Ctx.Params, TEXT("script"), TEXT("ParticleUpdateScript")));
		const FString ModuleName = GetString(Ctx.Params, TEXT("module"));
		if (!ModuleName.IsEmpty())
		{
			Ref.ModuleName = FName(*ModuleName);
		}
		return Ref;
	}

	bool BuildInputValue(const FUplinkToolContext& Ctx, FNiagaraExt_StackInputValue& Out, FString& OutError)
	{
		const FString Type = GetString(Ctx.Params, TEXT("type"), TEXT("float"));
		const TSharedPtr<FJsonValue> Raw = Ctx.Params->TryGetField(FStringView(TEXT("value")));
		if (!Raw.IsValid())
		{
			OutError = TEXT("'value' is required");
			return false;
		}

		double Number = 0.0;
		bool bBool = false;
		const TSharedPtr<FJsonObject>* Object = nullptr;

		if (Type == TEXT("float"))
		{
			if (!Raw->TryGetNumber(Number)) { OutError = TEXT("float value must be a number"); return false; }
			Out.InitializeAs<FNiagaraFloat>();
			Out.GetMutable<FNiagaraFloat>().Value = static_cast<float>(Number);
		}
		else if (Type == TEXT("int"))
		{
			if (!Raw->TryGetNumber(Number)) { OutError = TEXT("int value must be a number"); return false; }
			Out.InitializeAs<FNiagaraInt32>();
			Out.GetMutable<FNiagaraInt32>().Value = static_cast<int32>(Number);
		}
		else if (Type == TEXT("bool"))
		{
			if (!Raw->TryGetBool(bBool)) { OutError = TEXT("bool value must be true/false"); return false; }
			Out.InitializeAs<FNiagaraBool>();
			Out.GetMutable<FNiagaraBool>().SetValue(bBool);
		}
		else if (Type == TEXT("vec3"))
		{
			if (!Raw->TryGetObject(Object) || !Object->IsValid()) { OutError = TEXT("vec3 value must be {x,y,z}"); return false; }
			FVector3f Vector = FVector3f::ZeroVector;
			double Component = 0.0;
			if ((*Object)->TryGetNumberField(FStringView(TEXT("x")), Component)) { Vector.X = Component; }
			if ((*Object)->TryGetNumberField(FStringView(TEXT("y")), Component)) { Vector.Y = Component; }
			if ((*Object)->TryGetNumberField(FStringView(TEXT("z")), Component)) { Vector.Z = Component; }
			// FVector3f registers through TVariantStructure, not TBaseStructure,
			// so the templated InitializeAs<T> cannot be used here.
			Out.InitializeAs(TVariantStructure<FVector3f>::Get(), reinterpret_cast<const uint8*>(&Vector));
		}
		else if (Type == TEXT("color"))
		{
			if (!Raw->TryGetObject(Object) || !Object->IsValid()) { OutError = TEXT("color value must be {r,g,b,a}"); return false; }
			FLinearColor Color = FLinearColor::White;
			double Component = 0.0;
			if ((*Object)->TryGetNumberField(FStringView(TEXT("r")), Component)) { Color.R = Component; }
			if ((*Object)->TryGetNumberField(FStringView(TEXT("g")), Component)) { Color.G = Component; }
			if ((*Object)->TryGetNumberField(FStringView(TEXT("b")), Component)) { Color.B = Component; }
			if ((*Object)->TryGetNumberField(FStringView(TEXT("a")), Component)) { Color.A = Component; }
			Out.InitializeAs<FLinearColor>(Color);
		}
		else
		{
			OutError = TEXT("type must be float, int, bool, vec3, or color");
			return false;
		}
		return true;
	}
#endif // UPLINK_UE_AT_LEAST(5, 8)

	constexpr const TCHAR* NotSupportedMessage =
		TEXT("Niagara stack authoring needs UE 5.8+ (UNiagaraExternalEditUtilities does not exist in this engine version)");
}

void UplinkTools::RegisterNiagara(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("niagara_create"),
		TEXT("Create a Niagara System asset. Optionally start from a template system, and/or add an emitter from a template emitter (e.g. /Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain - engine templates are always mounted). UE 5.8+ only."),
		TEXT(R"json({"type":"object","properties":{"path":{"type":"string","description":"New asset path, e.g. /Game/FX/NS_Tornado"},"template_system":{"type":"string","description":"Optional system to copy"},"emitter_template":{"type":"string","description":"Optional emitter template asset to add"},"emitter_name":{"type":"string"}},"required":["path"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
#if UPLINK_UE_AT_LEAST(5, 8)
			const FString FullPath = GetString(Ctx.Params, TEXT("path"));
			if (!FullPath.StartsWith(TEXT("/Game/")))
			{
				return FUplinkToolResult::Error(TEXT("'path' must start with /Game/"));
			}
			FString Folder, AssetName;
			FullPath.Split(TEXT("/"), &Folder, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

			UNiagaraSystem* Template = nullptr;
			const FString TemplatePath = GetString(Ctx.Params, TEXT("template_system"));
			if (!TemplatePath.IsEmpty())
			{
				Template = LoadObject<UNiagaraSystem>(nullptr, *TemplatePath);
				if (!Template)
				{
					return FUplinkToolResult::Error(TEXT("template_system not found"));
				}
			}

			FNiagaraExternalEditContext CreateContext;
			UNiagaraSystem* System = UNiagaraExternalEditUtilities::CreateNiagaraSystem(AssetName, Folder, Template, CreateContext);
			if (!System || CreateContext.HasErrors())
			{
				return ContextErrors(CreateContext, TEXT("niagara_create"));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("system"), System->GetPathName());

			const FString EmitterTemplatePath = GetString(Ctx.Params, TEXT("emitter_template"));
			if (!EmitterTemplatePath.IsEmpty())
			{
				UNiagaraEmitter* EmitterTemplate = LoadObject<UNiagaraEmitter>(nullptr, *EmitterTemplatePath);
				if (!EmitterTemplate)
				{
					return FUplinkToolResult::Error(TEXT("emitter_template not found"));
				}
				FNiagaraExternalEditContext EmitterContext(System);
				FNiagaraExt_EmitterTopology Topology;
				UNiagaraExternalEditUtilities::AddEmitter(EmitterTemplate,
					FName(*GetString(Ctx.Params, TEXT("emitter_name"), EmitterTemplate->GetName())), Topology, EmitterContext);
				if (EmitterContext.HasErrors())
				{
					return ContextErrors(EmitterContext, TEXT("AddEmitter"));
				}
				Data->SetObjectField(TEXT("emitter"), StructToJson(Topology));
			}

			System->MarkPackageDirty();
			return FUplinkToolResult::Ok(Data, TEXT("created (dirty, not saved)"));
#else
			return FUplinkToolResult::Error(NotSupportedMessage);
#endif
		});

	Registry.RegisterQuick(
		TEXT("niagara_query"),
		TEXT("Summarize a Niagara System: emitters, scripts, modules, renderers, plus compile state and stack issues. UE 5.8+ only."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"}},"required":["asset"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
#if UPLINK_UE_AT_LEAST(5, 8)
			FString Error;
			UNiagaraSystem* System = LoadSystem(Ctx, Error);
			if (!System)
			{
				return FUplinkToolResult::Error(Error);
			}

			FNiagaraExternalEditContext Context(System);
			FNiagaraExt_SystemSummary Summary;
			UNiagaraExternalEditUtilities::GetSystemSummary(System, Summary, Context);
			FNiagaraExt_SystemCompileState CompileState;
			UNiagaraExternalEditUtilities::GetSystemCompileState(System, CompileState, Context);
			FNiagaraExt_StackIssues Issues;
			UNiagaraExternalEditUtilities::GetStackIssues(System, Issues, Context);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetObjectField(TEXT("summary"), StructToJson(Summary));
			Data->SetObjectField(TEXT("compile_state"), StructToJson(CompileState));
			Data->SetObjectField(TEXT("issues"), StructToJson(Issues));
			return FUplinkToolResult::Ok(Data);
#else
			return FUplinkToolResult::Error(NotSupportedMessage);
#endif
		});

	Registry.RegisterQuick(
		TEXT("niagara_add_module"),
		TEXT("Add a module script to an emitter or system stack, e.g. /Niagara/Modules/Update/Velocity/VortexVelocity.VortexVelocity into ParticleUpdateScript. script is an ENiagaraScriptUsage name (ParticleSpawnScript, ParticleUpdateScript, EmitterSpawnScript, EmitterUpdateScript, SystemSpawnScript, SystemUpdateScript). Pass 'module' to insert after an existing module. UE 5.8+ only."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"emitter":{"type":"string"},"script":{"type":"string","default":"ParticleUpdateScript"},"module_asset":{"type":"string","description":"UNiagaraScript module asset path"},"module":{"type":"string","description":"Optional existing module name to insert after"}},"required":["asset","module_asset"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
#if UPLINK_UE_AT_LEAST(5, 8)
			FString Error;
			UNiagaraSystem* System = LoadSystem(Ctx, Error);
			if (!System)
			{
				return FUplinkToolResult::Error(Error);
			}
			UNiagaraScript* ModuleAsset = LoadObject<UNiagaraScript>(nullptr, *GetString(Ctx.Params, TEXT("module_asset")));
			if (!ModuleAsset)
			{
				return FUplinkToolResult::Error(TEXT("module_asset not found (see /Niagara/Modules/... paths)"));
			}

			FNiagaraExternalEditContext Context(System);
			FNiagaraExt_ModuleTopology Topology;
			UNiagaraExternalEditUtilities::AddModule(MakeRef(System, Ctx), ModuleAsset, Topology, Context);
			if (Context.HasErrors())
			{
				return ContextErrors(Context, TEXT("AddModule"));
			}
			System->MarkPackageDirty();

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetObjectField(TEXT("module"), StructToJson(Topology));
			return FUplinkToolResult::Ok(Data);
#else
			return FUplinkToolResult::Error(NotSupportedMessage);
#endif
		});

	Registry.RegisterQuick(
		TEXT("niagara_module_inputs"),
		TEXT("List a stack module's inputs with names, types, current values and editability - discover exact input names before niagara_set_input. UE 5.8+ only."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"emitter":{"type":"string"},"script":{"type":"string","default":"ParticleUpdateScript"},"module":{"type":"string"}},"required":["asset","module"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
#if UPLINK_UE_AT_LEAST(5, 8)
			FString Error;
			UNiagaraSystem* System = LoadSystem(Ctx, Error);
			if (!System)
			{
				return FUplinkToolResult::Error(Error);
			}
			FNiagaraExternalEditContext Context(System);
			FNiagaraExt_ModuleInputValues Values;
			UNiagaraExternalEditUtilities::GetModuleInputValues(MakeRef(System, Ctx), Values, Context);
			if (Context.HasErrors())
			{
				return ContextErrors(Context, TEXT("GetModuleInputValues"));
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetObjectField(TEXT("inputs"), StructToJson(Values));
			return FUplinkToolResult::Ok(Data);
#else
			return FUplinkToolResult::Error(NotSupportedMessage);
#endif
		});

	Registry.RegisterQuick(
		TEXT("niagara_set_input"),
		TEXT("Set a module input's local value (float, int, bool, vec3, color). 'input' is the input name from niagara_module_inputs (array form addresses nested dynamic-input chains). UE 5.8+ only."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"emitter":{"type":"string"},"script":{"type":"string","default":"ParticleUpdateScript"},"module":{"type":"string"},"input":{"description":"Input name (string) or nested name chain (array of strings)"},"type":{"type":"string","enum":["float","int","bool","vec3","color"],"default":"float"},"value":{"description":"Number, bool, {x,y,z}, or {r,g,b,a} to match 'type'"}},"required":["asset","module","input","value"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
#if UPLINK_UE_AT_LEAST(5, 8)
			FString Error;
			UNiagaraSystem* System = LoadSystem(Ctx, Error);
			if (!System)
			{
				return FUplinkToolResult::Error(Error);
			}

			FNiagaraExt_StackItemReference Ref = MakeRef(System, Ctx);
			const TSharedPtr<FJsonValue> InputField = Ctx.Params->TryGetField(FStringView(TEXT("input")));
			const TArray<TSharedPtr<FJsonValue>>* InputArray = nullptr;
			FString SingleInput;
			if (InputField.IsValid() && InputField->TryGetArray(InputArray))
			{
				for (const TSharedPtr<FJsonValue>& Entry : *InputArray)
				{
					Ref.InputNameStack.Add(FName(*Entry->AsString()));
				}
			}
			else if (InputField.IsValid() && InputField->TryGetString(SingleInput))
			{
				Ref.InputNameStack.Add(FName(*SingleInput));
			}
			if (Ref.InputNameStack.Num() == 0)
			{
				return FUplinkToolResult::Error(TEXT("'input' must be a name or array of names"));
			}

			FNiagaraExt_StackInputValue Value;
			if (!BuildInputValue(Ctx, Value, Error))
			{
				return FUplinkToolResult::Error(Error);
			}

			FNiagaraExternalEditContext Context(System);
			UNiagaraExternalEditUtilities::SetStackInputData(Ref, Value, Context);
			if (Context.HasErrors())
			{
				return ContextErrors(Context, TEXT("SetStackInputData"));
			}
			System->MarkPackageDirty();
			return FUplinkToolResult::Ok(nullptr, TEXT("input set"));
#else
			return FUplinkToolResult::Error(NotSupportedMessage);
#endif
		});

	Registry.RegisterQuick(
		TEXT("niagara_remove_module"),
		TEXT("Remove a module from an emitter or system stack. UE 5.8+ only."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"emitter":{"type":"string"},"script":{"type":"string","default":"ParticleUpdateScript"},"module":{"type":"string"}},"required":["asset","module"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
#if UPLINK_UE_AT_LEAST(5, 8)
			FString Error;
			UNiagaraSystem* System = LoadSystem(Ctx, Error);
			if (!System)
			{
				return FUplinkToolResult::Error(Error);
			}
			FNiagaraExternalEditContext Context(System);
			UNiagaraExternalEditUtilities::RemoveModule(MakeRef(System, Ctx), Context);
			if (Context.HasErrors())
			{
				return ContextErrors(Context, TEXT("RemoveModule"));
			}
			System->MarkPackageDirty();
			return FUplinkToolResult::Ok(nullptr, TEXT("module removed"));
#else
			return FUplinkToolResult::Error(NotSupportedMessage);
#endif
		});

	Registry.RegisterQuick(
		TEXT("niagara_set_user_param"),
		TEXT("Set an exposed User parameter's default value on the system asset (float, int, bool, vec3, color, position)."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"name":{"type":"string","description":"Parameter name, with or without the User. prefix"},"type":{"type":"string","enum":["float","int","bool","vec3","color","position"]},"value":{"description":"Number, bool, {x,y,z}, or {r,g,b,a}"}},"required":["asset","name","type","value"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UNiagaraSystem* System = LoadSystem(Ctx, Error);
			if (!System)
			{
				return FUplinkToolResult::Error(Error);
			}

			FString Name = GetString(Ctx.Params, TEXT("name"));
			if (!Name.StartsWith(TEXT("User.")))
			{
				Name = TEXT("User.") + Name;
			}
			const FString Type = GetString(Ctx.Params, TEXT("type"), TEXT("float"));
			const TSharedPtr<FJsonValue> Raw = Ctx.Params->TryGetField(FStringView(TEXT("value")));
			FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

			double Number = 0.0;
			bool bBool = false;
			const TSharedPtr<FJsonObject>* Object = nullptr;
			bool bSet = false;

			if (Type == TEXT("float") && Raw->TryGetNumber(Number))
			{
				bSet = Store.SetParameterValue<float>(static_cast<float>(Number),
					FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(*Name)));
			}
			else if (Type == TEXT("int") && Raw->TryGetNumber(Number))
			{
				bSet = Store.SetParameterValue<int32>(static_cast<int32>(Number),
					FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), FName(*Name)));
			}
			else if (Type == TEXT("bool") && Raw->TryGetBool(bBool))
			{
				FNiagaraBool BoolValue;
				BoolValue.SetValue(bBool);
				bSet = Store.SetParameterValue<FNiagaraBool>(BoolValue,
					FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), FName(*Name)));
			}
			else if ((Type == TEXT("vec3") || Type == TEXT("position")) && Raw->TryGetObject(Object) && Object->IsValid())
			{
				FVector Vector = FVector::ZeroVector;
				double Component = 0.0;
				if ((*Object)->TryGetNumberField(FStringView(TEXT("x")), Component)) { Vector.X = Component; }
				if ((*Object)->TryGetNumberField(FStringView(TEXT("y")), Component)) { Vector.Y = Component; }
				if ((*Object)->TryGetNumberField(FStringView(TEXT("z")), Component)) { Vector.Z = Component; }
				if (Type == TEXT("position"))
				{
					bSet = Store.SetPositionParameterValue(Vector, FName(*Name));
				}
				else
				{
					bSet = Store.SetParameterValue<FVector3f>(FVector3f(Vector),
						FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), FName(*Name)));
				}
			}
			else if (Type == TEXT("color") && Raw->TryGetObject(Object) && Object->IsValid())
			{
				FLinearColor Color = FLinearColor::White;
				double Component = 0.0;
				if ((*Object)->TryGetNumberField(FStringView(TEXT("r")), Component)) { Color.R = Component; }
				if ((*Object)->TryGetNumberField(FStringView(TEXT("g")), Component)) { Color.G = Component; }
				if ((*Object)->TryGetNumberField(FStringView(TEXT("b")), Component)) { Color.B = Component; }
				if ((*Object)->TryGetNumberField(FStringView(TEXT("a")), Component)) { Color.A = Component; }
				bSet = Store.SetParameterValue<FLinearColor>(Color,
					FNiagaraVariable(FNiagaraTypeDefinition::GetColorDef(), FName(*Name)));
			}
			else
			{
				return FUplinkToolResult::Error(TEXT("value does not match type (float,int,bool,vec3,color,position)"));
			}

			if (!bSet)
			{
				TArray<FNiagaraVariable> UserParameters;
				Store.GetUserParameters(UserParameters);
				TArray<FString> Names;
				for (const FNiagaraVariable& Variable : UserParameters)
				{
					Names.Add(Variable.GetName().ToString());
				}
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("parameter not found or type mismatch. User parameters: %s"),
					Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
			}
			System->MarkPackageDirty();
			return FUplinkToolResult::Ok(nullptr, TEXT("user parameter set"));
		});

	Registry.RegisterQuick(
		TEXT("niagara_compile"),
		TEXT("Compile a Niagara system and wait for completion, returning the compile state and readiness. Compiles are forced by default - newly added emitters only get their compiled data on a forced pass (a non-forced request can skip them, leaving the system not ready to run)."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"force":{"type":"boolean","default":true}},"required":["asset"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UNiagaraSystem* System = LoadSystem(Ctx, Error);
			if (!System)
			{
				return FUplinkToolResult::Error(Error);
			}
			bool bForce = true;
			Ctx.Params->TryGetBoolField(FStringView(TEXT("force")), bForce);
			System->RequestCompile(bForce);
			System->WaitForCompilationComplete(false, false);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
#if UPLINK_UE_AT_LEAST(5, 8)
			FNiagaraExternalEditContext Context(System);
			FNiagaraExt_SystemCompileState CompileState;
			UNiagaraExternalEditUtilities::GetSystemCompileState(System, CompileState, Context);
			Data->SetObjectField(TEXT("compile_state"), StructToJson(CompileState));
#endif
			Data->SetBoolField(TEXT("ready_to_run"), System->IsReadyToRun());
			return FUplinkToolResult::Ok(Data);
		});
}
