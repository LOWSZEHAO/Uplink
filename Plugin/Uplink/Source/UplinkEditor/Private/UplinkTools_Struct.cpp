// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// User Defined Struct tools: read a struct's members and add, remove, rename
// or retype them. asset_create makes the struct asset itself.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"
#include "Blueprint/UplinkBlueprintCommon.h"

#include "EdGraphSchema_K2.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Kismet2/StructureEditorUtils.h"

using namespace UplinkToolUtil;
using UplinkBlueprint::MakePinType;

namespace
{
	UUserDefinedStruct* LoadUserStruct(const FUplinkToolContext& Ctx, FString& OutError)
	{
		const FString Path = GetString(Ctx.Params, TEXT("asset"));
		UUserDefinedStruct* Struct = LoadObject<UUserDefinedStruct>(nullptr, *Path);
		if (!Struct)
		{
			OutError = FString::Printf(
				TEXT("no User Defined Struct at '%s' - pass the full asset path (asset_search finds it), ")
				TEXT("e.g. /Game/Data/S_Item. asset_create with class 'UserDefinedStruct' makes one."),
				*Path);
		}
		return Struct;
	}

	/**
	 * Members are addressed by the name the struct editor shows. The stored
	 * VarName is Damage_2_<32 hex digits> - unique, but nothing a caller can
	 * type or predict, so it is never the address.
	 */
	FStructVariableDescription* FindMemberByName(UUserDefinedStruct* Struct, const FString& Name)
	{
		for (FStructVariableDescription& Desc : FStructureEditorUtils::GetVarDesc(Struct))
		{
			if (Desc.FriendlyName.Equals(Name, ESearchCase::IgnoreCase))
			{
				return &Desc;
			}
		}
		return nullptr;
	}

	FString MemberNames(UUserDefinedStruct* Struct)
	{
		TArray<FString> Names;
		for (const FStructVariableDescription& Desc : FStructureEditorUtils::GetVarDesc(Struct))
		{
			Names.Add(Desc.FriendlyName);
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("(none)");
	}

	/**
	 * AddVariable names each new member MemberVar_<n> from a counter kept on
	 * the asset, and then check()s that the name it just generated is unused.
	 * A member already sitting under one of those names is therefore not a
	 * naming annoyance - it is a check() failure, which takes the editor down
	 * with it once the counter reaches that number. The struct editor cannot
	 * produce the collision, so the only way in is a tool renaming a member
	 * into the generated pattern; refuse that instead of arming it.
	 */
	bool IsGeneratedMemberName(const FString& Name)
	{
		FString Suffix;
		if (!Name.Split(TEXT("MemberVar_"), nullptr, &Suffix) || !Name.StartsWith(TEXT("MemberVar_")))
		{
			return false;
		}
		return !Suffix.IsEmpty() && Suffix.IsNumeric();
	}

	TSharedRef<FJsonObject> MemberToJson(const FStructVariableDescription& Desc)
	{
		const FEdGraphPinType PinType = Desc.ToPinType();
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Desc.FriendlyName);
		Row->SetStringField(TEXT("type"), UEdGraphSchema_K2::TypeToText(PinType).ToString());
		Row->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
		if (!PinType.PinSubCategory.IsNone())
		{
			Row->SetStringField(TEXT("sub_category"), PinType.PinSubCategory.ToString());
		}
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Row->SetStringField(TEXT("sub_category_object"), PinType.PinSubCategoryObject->GetPathName());
		}
		if (PinType.ContainerType != EPinContainerType::None)
		{
			Row->SetStringField(TEXT("container"),
				PinType.ContainerType == EPinContainerType::Array ? TEXT("array")
					: PinType.ContainerType == EPinContainerType::Set ? TEXT("set") : TEXT("map"));
		}
		if (!Desc.DefaultValue.IsEmpty())
		{
			Row->SetStringField(TEXT("default"), Desc.DefaultValue);
		}
		// The real field name, for anything that reads the struct through
		// reflection rather than through this tool.
		Row->SetStringField(TEXT("var_name"), Desc.VarName.ToString());
		return Row;
	}
}

void UplinkTools::RegisterStruct(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("struct_query"),
		TEXT("Read a User Defined Struct's members: display name, type, and the underlying field name. 'name' is what every other tool addresses a member by - the stored field name carries a generated guid suffix (Damage_2_<hex>) and is reported only as 'var_name'."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string","description":"User Defined Struct asset path"}},"required":["asset"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UUserDefinedStruct* Struct = LoadUserStruct(Ctx, Error);
			if (!Struct)
			{
				return FUplinkToolResult::Error(Error);
			}

			TArray<TSharedPtr<FJsonValue>> Members;
			for (const FStructVariableDescription& Desc : FStructureEditorUtils::GetVarDesc(Struct))
			{
				Members.Add(MakeShared<FJsonValueObject>(MemberToJson(Desc)));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("asset"), Struct->GetPathName());
			Data->SetArrayField(TEXT("members"), Members);
			Data->SetNumberField(TEXT("count"), Members.Num());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("struct_modify"),
		TEXT("Edit a User Defined Struct's members. op: add {name, type} | remove {name} | rename {name, new_name} | retype {name, type} | set_default {name, default}. 'type' takes the same vocabulary as bp_modify's variables: bool, int, int64, float, string, name, text, byte, vector, rotator, transform, object:/Script/..., struct:/Game/..., enum:..., and array:/set:/map:<key>:<value> wrappers. A freshly created struct already carries one MemberVar_0 member from the factory: rename it, or add the real members first and remove it afterwards - a struct cannot be left with no members at all. Marked dirty, not saved."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"op":{"type":"string","enum":["add","remove","rename","retype","set_default"]},"name":{"type":"string"},"new_name":{"type":"string"},"type":{"type":"string"},"default":{"type":"string"}},"required":["asset","op","name"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UUserDefinedStruct* Struct = LoadUserStruct(Ctx, Error);
			if (!Struct)
			{
				return FUplinkToolResult::Error(Error);
			}
			const FString Op = GetString(Ctx.Params, TEXT("op"));
			const FString Name = GetString(Ctx.Params, TEXT("name"));
			if (Name.IsEmpty())
			{
				return FUplinkToolResult::Error(
					TEXT("'name' is required - the member's display name, as struct_query reports it"));
			}
			// Checked for every op that takes a new name, before anything is
			// written. See IsGeneratedMemberName: this one crashes the editor
			// later rather than failing now.
			const FString ProposedName = Op == TEXT("rename") ? GetString(Ctx.Params, TEXT("new_name")) : Name;
			if ((Op == TEXT("add") || Op == TEXT("rename")) && IsGeneratedMemberName(ProposedName))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' is the pattern the struct editor generates for unnamed members. ")
					TEXT("A member under that name collides with a later one and takes the editor ")
					TEXT("down on a check(), so it is refused here - pick another name."),
					*ProposedName));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("asset"), Struct->GetPathName());
			Data->SetStringField(TEXT("op"), Op);

			if (Op == TEXT("add"))
			{
				FEdGraphPinType PinType;
				if (!MakePinType(GetString(Ctx.Params, TEXT("type"), TEXT("float")), PinType, Error))
				{
					return FUplinkToolResult::Error(Error);
				}

				// Both checks run before AddVariable, because AddVariable names
				// the member itself and the rename is a second call: a name that
				// turns out to be taken would otherwise leave a MemberVar_N
				// behind that nobody asked for.
				if (!FStructureEditorUtils::IsUniqueVariableFriendlyName(Struct, Name))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' already exists on %s. Members: %s"),
						*Name, *Struct->GetName(), *MemberNames(Struct)));
				}
				// CanHaveAMemberVariableOfType is what AddVariable consults, but
				// it only writes its reason to the log and hands back false - so
				// ask it here, where the reason can reach the caller. A struct
				// asked to contain itself lands exactly on this.
				FString Refusal;
				if (!FStructureEditorUtils::CanHaveAMemberVariableOfType(Struct, PinType, &Refusal))
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("'%s': %s"), *Name, *Refusal));
				}

				const int32 CountBefore = FStructureEditorUtils::GetVarDesc(Struct).Num();
				if (!FStructureEditorUtils::AddVariable(Struct, PinType))
				{
					return FUplinkToolResult::Error(
						TEXT("the struct refused a member of that type (see LogBlueprint in output_log)"));
				}

				// AddVariable takes no name - it generates MemberVar_N and a
				// matching display name, so the requested name is a rename of
				// what it just made. Without this the member is created and
				// silently called something else.
				TArray<FStructVariableDescription>& Vars = FStructureEditorUtils::GetVarDesc(Struct);
				if (Vars.Num() != CountBefore + 1)
				{
					return FUplinkToolResult::Error(TEXT("AddVariable reported success but added no member"));
				}
				const FGuid NewGuid = Vars.Last().VarGuid;
				const FString GeneratedName = Vars.Last().FriendlyName;
				if (!FStructureEditorUtils::RenameVariable(Struct, NewGuid, Name))
				{
					FStructureEditorUtils::RemoveVariable(Struct, NewGuid);
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("member was added as '%s' but could not be renamed to '%s', so it was removed again"),
						*GeneratedName, *Name));
				}

				const FStructVariableDescription* Added = FStructureEditorUtils::GetVarDescByGuid(Struct, NewGuid);
				if (Added)
				{
					Data->SetObjectField(TEXT("member"), MemberToJson(*Added));
				}
				return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("added '%s'"), *Name));
			}

			// Every other op addresses a member that must already be there.
			FStructVariableDescription* Desc = FindMemberByName(Struct, Name);
			if (!Desc)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("no member '%s' on %s. Members: %s"),
					*Name, *Struct->GetName(), *MemberNames(Struct)));
			}
			// Taken by value: the ops below reallocate the description array.
			const FGuid Guid = Desc->VarGuid;

			if (Op == TEXT("remove"))
			{
				// RemoveVariable will not leave a struct with no members: it
				// early-outs at a count of one and says so only at Log
				// verbosity, so the bare false below would read as a mystery.
				if (FStructureEditorUtils::GetVarDesc(Struct).Num() <= 1)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' is the only member of %s and a struct cannot be left empty. ")
						TEXT("Add the replacement member first, then remove this one."),
						*Name, *Struct->GetName()));
				}
				if (!FStructureEditorUtils::RemoveVariable(Struct, Guid))
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("could not remove '%s'"), *Name));
				}
				return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("removed '%s'"), *Name));
			}
			if (Op == TEXT("rename"))
			{
				const FString NewName = GetString(Ctx.Params, TEXT("new_name"));
				if (NewName.IsEmpty())
				{
					return FUplinkToolResult::Error(TEXT("rename needs 'new_name'"));
				}
				// RenameVariable answers false for an empty name and for one
				// already in use, with no way to tell those apart afterwards.
				if (!FStructureEditorUtils::IsUniqueVariableFriendlyName(Struct, NewName))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' is already a member of %s"), *NewName, *Struct->GetName()));
				}
				if (!FStructureEditorUtils::RenameVariable(Struct, Guid, NewName))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("could not rename '%s' to '%s'"), *Name, *NewName));
				}
				Data->SetStringField(TEXT("new_name"), NewName);
				return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("renamed '%s' to '%s'"), *Name, *NewName));
			}
			if (Op == TEXT("retype"))
			{
				FEdGraphPinType PinType;
				if (!MakePinType(GetString(Ctx.Params, TEXT("type")), PinType, Error))
				{
					return FUplinkToolResult::Error(Error);
				}
				FString Refusal;
				if (!FStructureEditorUtils::CanHaveAMemberVariableOfType(Struct, PinType, &Refusal))
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("'%s': %s"), *Name, *Refusal));
				}
				if (!FStructureEditorUtils::ChangeVariableType(Struct, Guid, PinType))
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("could not retype '%s'"), *Name));
				}
				// Read it back: retyping drops a default the old type owned.
				const FStructVariableDescription* Changed = FStructureEditorUtils::GetVarDescByGuid(Struct, Guid);
				if (Changed)
				{
					Data->SetObjectField(TEXT("member"), MemberToJson(*Changed));
				}
				return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("retyped '%s'"), *Name));
			}
			if (Op == TEXT("set_default"))
			{
				const FString Default = GetString(Ctx.Params, TEXT("default"));
				if (!FStructureEditorUtils::ChangeVariableDefaultValue(Struct, Guid, Default))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' would not take the default '%s' - it is refused unless it parses as that member's type"),
						*Name, *Default));
				}
				Data->SetStringField(TEXT("default"), Default);
				return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("set default on '%s'"), *Name));
			}

			return FUplinkToolResult::Error(FString::Printf(
				TEXT("unknown op '%s' - add, remove, rename, retype, set_default"), *Op));
		});
}
