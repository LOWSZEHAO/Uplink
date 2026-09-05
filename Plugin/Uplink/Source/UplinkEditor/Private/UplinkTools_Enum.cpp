// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// User Defined Enum tools: read an enum's entries and add, remove, rename or
// reorder them. asset_create makes the enum asset itself - and makes it empty.

#include "UplinkTools.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Engine/UserDefinedEnum.h"
#include "Kismet2/EnumEditorUtils.h"

using namespace UplinkToolUtil;

namespace
{
	UUserDefinedEnum* LoadUserEnum(const FUplinkToolContext& Ctx, FString& OutError)
	{
		const FString Path = GetString(Ctx.Params, TEXT("asset"));
		UUserDefinedEnum* Enum = LoadObject<UUserDefinedEnum>(nullptr, *Path);
		if (!Enum)
		{
			OutError = FString::Printf(
				TEXT("no User Defined Enum at '%s' - pass the full asset path (asset_search finds it), ")
				TEXT("e.g. /Game/Data/E_State. asset_create with class 'UserDefinedEnum' makes one."),
				*Path);
		}
		return Enum;
	}

	/**
	 * How many entries a person would say the enum has.
	 *
	 * Every User Defined Enum carries a trailing _MAX that the enum editor never
	 * shows, and NumEnums() counts it. Subtracting one is the engine's own
	 * convention, not a guess: FEnumEditorUtils::CopyEnumeratorsWithoutMax does
	 * exactly this, and so does every details panel that lists an enum. Loops
	 * that miss it report a phantom last entry and let an index through that
	 * addresses the _MAX slot.
	 */
	int32 VisibleCount(const UUserDefinedEnum* Enum)
	{
		return FMath::Max(0, Enum->NumEnums() - 1);
	}

	/** Entries are addressed by the name the enum editor shows. */
	int32 FindEntryByName(const UUserDefinedEnum* Enum, const FString& Name)
	{
		for (int32 Index = 0; Index < VisibleCount(Enum); ++Index)
		{
			if (Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	FString EntryNames(const UUserDefinedEnum* Enum)
	{
		TArray<FString> Names;
		for (int32 Index = 0; Index < VisibleCount(Enum); ++Index)
		{
			Names.Add(Enum->GetDisplayNameTextByIndex(Index).ToString());
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("(none)");
	}

	TSharedRef<FJsonObject> EntryToJson(const UUserDefinedEnum* Enum, int32 Index)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("index"), Index);
		Row->SetStringField(TEXT("name"), Enum->GetDisplayNameTextByIndex(Index).ToString());
		Row->SetNumberField(TEXT("value"), Enum->GetValueByIndex(Index));
		// The stored name, which is what a saved asset and a C++ lookup see.
		// It keeps the NewEnumerator<n> the entry was born with: renaming an
		// entry changes only the display name, so the two drift apart on
		// purpose and both are worth reporting.
		Row->SetStringField(TEXT("raw_name"), Enum->GetNameStringByIndex(Index));
		return Row;
	}
}

void UplinkTools::RegisterEnum(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("enum_query"),
		TEXT("Read a User Defined Enum's entries: display name, value, index, and the stored raw name. 'name' is what enum_modify addresses an entry by, and what set_property and a Blueprint show. The trailing _MAX entry every enum carries is not reported, because it is not one of yours. There is no other way to read this - UEnum's name table is not a UPROPERTY, so get_property cannot reach it."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string","description":"User Defined Enum asset path"}},"required":["asset"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UUserDefinedEnum* Enum = LoadUserEnum(Ctx, Error);
			if (!Enum)
			{
				return FUplinkToolResult::Error(Error);
			}

			TArray<TSharedPtr<FJsonValue>> Entries;
			for (int32 Index = 0; Index < VisibleCount(Enum); ++Index)
			{
				Entries.Add(MakeShared<FJsonValueObject>(EntryToJson(Enum, Index)));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("asset"), Enum->GetPathName());
			Data->SetArrayField(TEXT("entries"), Entries);
			Data->SetNumberField(TEXT("count"), Entries.Num());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("enum_modify"),
		TEXT("Edit a User Defined Enum's entries. op: add {name} | remove {name} | rename {name, new_name} | move {name, index}. A newly created enum has NO entries at all, so a switch on it has no cases and a variable of its type has nothing to hold - add them here first. WARNING: remove and move renumber every entry, because the engine re-sequences values to 0..n-1 after either. Anything already storing one of those values - a placed actor's property, a saved game - keeps the old number and so means a different entry afterwards. Adding is safe; it only appends. Marked dirty, not saved."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"op":{"type":"string","enum":["add","remove","rename","move"]},"name":{"type":"string"},"new_name":{"type":"string"},"index":{"type":"number","description":"move: target position, 0-based"}},"required":["asset","op","name"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UUserDefinedEnum* Enum = LoadUserEnum(Ctx, Error);
			if (!Enum)
			{
				return FUplinkToolResult::Error(Error);
			}
			const FString Op = GetString(Ctx.Params, TEXT("op"));
			const FString Name = GetString(Ctx.Params, TEXT("name"));
			if (Name.IsEmpty())
			{
				return FUplinkToolResult::Error(
					TEXT("'name' is required - the entry's display name, as enum_query reports it"));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("asset"), Enum->GetPathName());
			Data->SetStringField(TEXT("op"), Op);

			if (Op == TEXT("add"))
			{
				if (FindEntryByName(Enum, Name) != INDEX_NONE)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' is already an entry of %s. Entries: %s"),
						*Name, *Enum->GetName(), *EntryNames(Enum)));
				}

				// AddNewEnumeratorForUserDefinedEnum names the entry itself -
				// NewEnumerator<n> - and returns void, so a named entry is
				// add-then-rename and there is no return value to check. The
				// count is the only evidence the add happened.
				const int32 Before = VisibleCount(Enum);
				FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
				const int32 After = VisibleCount(Enum);
				if (After != Before + 1)
				{
					return FUplinkToolResult::Error(
						TEXT("the enum did not take a new entry (see LogBlueprint in output_log)"));
				}

				const int32 NewIndex = After - 1;
				if (!FEnumEditorUtils::SetEnumeratorDisplayName(Enum, NewIndex, FText::FromString(Name)))
				{
					// Leaving it would strand an entry called NewEnumerator<n>
					// that nobody asked for, in an asset the caller believes
					// was not changed.
					FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(Enum, NewIndex);
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("entry was added but '%s' was refused as its name, so it was removed again. ")
						TEXT("A name must not be blank and must not match another entry."), *Name));
				}

				Data->SetObjectField(TEXT("entry"), EntryToJson(Enum, NewIndex));
				return FUplinkToolResult::Ok(Data, FString::Printf(TEXT("added '%s'"), *Name));
			}

			// Every other op addresses an entry that must already be there.
			const int32 Index = FindEntryByName(Enum, Name);
			if (Index == INDEX_NONE)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("no entry '%s' on %s. Entries: %s"), *Name, *Enum->GetName(), *EntryNames(Enum)));
			}

			if (Op == TEXT("remove"))
			{
				const int32 Before = VisibleCount(Enum);
				FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(Enum, Index);
				if (VisibleCount(Enum) != Before - 1)
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("could not remove '%s'"), *Name));
				}
				Data->SetNumberField(TEXT("count"), VisibleCount(Enum));
				return FUplinkToolResult::Ok(Data, FString::Printf(
					TEXT("removed '%s' - the remaining entries were renumbered, so stored values now mean different entries"),
					*Name));
			}
			if (Op == TEXT("rename"))
			{
				const FString NewName = GetString(Ctx.Params, TEXT("new_name"));
				if (NewName.IsEmpty())
				{
					return FUplinkToolResult::Error(TEXT("rename needs 'new_name'"));
				}
				if (!FEnumEditorUtils::SetEnumeratorDisplayName(Enum, Index, FText::FromString(NewName)))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' was refused as a name for '%s' - it must not be blank and must not match another entry. Entries: %s"),
						*NewName, *Name, *EntryNames(Enum)));
				}
				Data->SetObjectField(TEXT("entry"), EntryToJson(Enum, Index));
				return FUplinkToolResult::Ok(Data, FString::Printf(
					TEXT("renamed '%s' to '%s' - the value is unchanged, only the name it shows under"),
					*Name, *NewName));
			}
			if (Op == TEXT("move"))
			{
				if (!Ctx.Params->HasField(FStringView(TEXT("index"))))
				{
					return FUplinkToolResult::Error(TEXT("move needs 'index' - the target position, 0-based"));
				}
				const int32 Target = static_cast<int32>(GetNumber(Ctx.Params, TEXT("index"), 0));
				const int32 Last = VisibleCount(Enum) - 1;
				// Bounded here rather than left to the engine.
				// MoveEnumeratorInUserDefinedEnum tests TargetIndex against
				// NumEnums(), which counts the hidden _MAX, but then inserts
				// into a list that excludes it - so the one index its own
				// check admits and its array cannot hold walks off the end.
				if (Target < 0 || Target > Last)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("index %d is outside %s - it has %d entries, so 0 to %d"),
						Target, *Enum->GetName(), VisibleCount(Enum), Last));
				}
				FEnumEditorUtils::MoveEnumeratorInUserDefinedEnum(Enum, Index, Target);
				if (FindEntryByName(Enum, Name) != Target)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("'%s' did not move to %d"), *Name, Target));
				}
				Data->SetNumberField(TEXT("index"), Target);
				return FUplinkToolResult::Ok(Data, FString::Printf(
					TEXT("moved '%s' to %d - the entries were renumbered, so stored values now mean different entries"),
					*Name, Target));
			}

			return FUplinkToolResult::Error(FString::Printf(
				TEXT("unknown op '%s' - add, remove, rename, move"), *Op));
		});
}
