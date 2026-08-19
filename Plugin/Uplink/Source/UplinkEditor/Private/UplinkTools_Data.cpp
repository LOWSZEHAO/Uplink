// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// DataTable tools: create tables, read rows as JSON, add/update/remove rows.

#include "UplinkTools.h"
#include "UplinkCompat.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "DataTableEditorUtils.h"
#include "Engine/DataTable.h"
#include "JsonObjectConverter.h"
#include "UObject/Package.h"

using namespace UplinkToolUtil;

namespace
{
	UDataTable* LoadTable(const FUplinkToolContext& Ctx, FString& OutError)
	{
		const FString Path = GetString(Ctx.Params, TEXT("asset"));
		UObject* Asset = LoadObject<UObject>(nullptr, *Path);
		if (!Asset)
		{
			OutError = FString::Printf(
				TEXT("no asset at '%s' - pass the full asset path (asset_search finds it), e.g. /Game/Data/DT_Weapons"),
				*Path);
			return nullptr;
		}

		UDataTable* Table = Cast<UDataTable>(Asset);
		if (!Table)
		{
			// Reporting "not found" for an asset that is plainly there sends the
			// caller hunting for a path problem they do not have.
			OutError = FString::Printf(TEXT("'%s' is a %s, not a DataTable"), *Path, *Asset->GetClass()->GetName());
			return nullptr;
		}
		if (!Table->GetRowStruct())
		{
			OutError = FString::Printf(
				TEXT("'%s' has no row struct, so it has no columns to read or write - set one in the DataTable editor first"),
				*Path);
			return nullptr;
		}
		return Table;
	}

	/** One column write the caller asked for, resolved to a real property. */
	struct FColumnWrite
	{
		FString Name;
		FProperty* Property = nullptr;
		TSharedPtr<FJsonValue> Value;
	};

	/**
	 * Match every key to a column before anything is written.
	 *
	 * A name matching no column used to be collected and handed back beside a
	 * success, so an update where every key was wrong still read as done. A
	 * Blueprint row struct made that the ordinary case rather than a typo: the
	 * column names datatable_query reports are the authored ones, while the
	 * lookup here was on the mangled Name_2_<guid> field name, so nothing a
	 * caller read back could be written again. FindTableProperty accepts both
	 * spellings, which is what the engine's own CSV import does.
	 */
	bool ResolveColumns(
		UDataTable* Table,
		const TSharedPtr<FJsonObject>& Values,
		TArray<FColumnWrite>& OutWrites,
		FString& OutError)
	{
		TArray<FString> Unknown;
		for (const auto& Pair : Values->Values)
		{
			FColumnWrite Write;
			Write.Name = UplinkCompat::JsonKeyToString(Pair.Key);
			Write.Property = Table->FindTableProperty(FName(*Write.Name));
			Write.Value = Pair.Value;
			if (!Write.Property)
			{
				Unknown.Add(Write.Name);
				continue;
			}
			OutWrites.Add(MoveTemp(Write));
		}

		if (Unknown.Num() == 0)
		{
			return true;
		}

		TArray<FString> Columns;
		for (TFieldIterator<FProperty> It(Table->GetRowStruct()); It; ++It)
		{
			Columns.Add(It->GetAuthoredName());
		}
		OutError = FString::Printf(
			TEXT("no column named %s on this row struct. Its columns are: %s - datatable_query lists them with their current values."),
			*FString::Join(Unknown, TEXT(", ")), *FString::Join(Columns, TEXT(", ")));
		return false;
	}

	/** Writes resolved columns into a row; returns how many landed. */
	int32 WriteColumns(const TArray<FColumnWrite>& Writes, uint8* RowData, TArray<FString>& OutFailed)
	{
		int32 Applied = 0;
		for (const FColumnWrite& Write : Writes)
		{
			void* ValueAddr = Write.Property->ContainerPtrToValuePtr<void>(RowData);
			if (!FJsonObjectConverter::JsonValueToUProperty(Write.Value, Write.Property, ValueAddr))
			{
				OutFailed.Add(FString::Printf(TEXT("%s: JSON conversion failed"), *Write.Name));
				continue;
			}

			// An asset path that resolves to nothing converts happily and writes
			// a null, which is indistinguishable from a cell the caller meant to
			// clear - and a row full of quiet nulls is found much later.
			FString Unresolved;
			if (!NamedObjectResolved(Write.Property, Write.Value, ValueAddr, Unresolved))
			{
				// The shared guard names the property by its raw field name. On
				// a Blueprint row struct that is Damage_2_<guid> rather than the
				// spelling the caller sent, so lead with theirs.
				OutFailed.Add(Write.Name == Write.Property->GetName()
					? Unresolved
					: FString::Printf(TEXT("%s: %s"), *Write.Name, *Unresolved));
				continue;
			}
			++Applied;
		}
		return Applied;
	}
}

void UplinkTools::RegisterData(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("datatable_create"),
		TEXT("Create a DataTable asset for a row struct: a C++ USTRUCT deriving from FTableRowBase, or a Blueprint struct asset. A C++ struct drops the F in its path - '/Script/MyGame.WeaponRow' for FWeaponRow - and a Blueprint one is '/Game/Data/S_MyRow.S_MyRow'. In memory, marked dirty - write it out with the 'save' tool."),
		TEXT(R"json({"type":"object","properties":{"path":{"type":"string","description":"e.g. /Game/Data/DT_Weapons"},"row_struct":{"type":"string"}},"required":["path","row_struct"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Path = GetString(Ctx.Params, TEXT("path"));
			if (!Path.StartsWith(TEXT("/Game/")))
			{
				return FUplinkToolResult::Error(TEXT("'path' must start with /Game/"));
			}
			if (!FPackageName::IsValidLongPackageName(Path))
			{
				// CreatePackage would take the name anyway and the asset would
				// only fail much later, at save time, far from the call that
				// named it.
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' is not a usable package path - give the folder and asset name only, with no '.Asset' suffix and no spaces, e.g. /Game/Data/DT_Weapons"),
					*Path));
			}
			if (LoadObject<UObject>(nullptr, *Path))
			{
				return FUplinkToolResult::Error(TEXT("an asset already exists at that path"));
			}
			const FString StructPath = GetString(Ctx.Params, TEXT("row_struct"));
			UScriptStruct* RowStruct = LoadObject<UScriptStruct>(nullptr, *StructPath);
			if (!RowStruct)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("row struct not found: %s. A C++ USTRUCT drops the F in its path (/Script/MyGame.WeaponRow, not FWeaponRow); a Blueprint struct is /Game/Path/S_Row.S_Row."),
					*StructPath));
			}
			if (!FDataTableEditorUtils::IsValidTableStruct(RowStruct))
			{
				// The engine does not stop a table being pointed at any struct,
				// it just misbehaves later - so the asset would be created,
				// reported as made, and be useless in the DataTable editor.
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("'%s' cannot be a DataTable row struct - a C++ row struct must derive from FTableRowBase (and FTableRowBase itself does not count); a Blueprint struct asset needs no parent."),
					*StructPath));
			}

			const FString AssetName = FPackageName::GetShortName(Path);
			UPackage* Package = CreatePackage(*Path);
			UDataTable* Table = NewObject<UDataTable>(Package, FName(*AssetName), RF_Public | RF_Standalone);
			Table->RowStruct = RowStruct;
			FAssetRegistryModule::AssetCreated(Table);
			Package->MarkPackageDirty();

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("asset"), Table->GetPathName());
			Data->SetStringField(TEXT("row_struct"), RowStruct->GetPathName());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("datatable_query"),
		TEXT("Read a DataTable: row struct, columns, and rows as JSON. 'row' returns just that row and is refused if no row has that name. Columns are the authored names the DataTable editor shows, and each row's 'values' is keyed by those same names - so what comes out of here goes straight back into datatable_modify. At most 'max' rows are returned; 'truncated' says when there were more."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"row":{"type":"string"},"max":{"type":"number","default":100}},"required":["asset"]})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UDataTable* Table = LoadTable(Ctx, Error);
			if (!Table)
			{
				return FUplinkToolResult::Error(Error);
			}
			const UScriptStruct* RowStruct = Table->GetRowStruct();

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("row_struct"), RowStruct->GetPathName());

			// Authored names, because that is what the row JSON below is keyed
			// by and what datatable_modify accepts back. The raw field name of a
			// Blueprint struct column is Damage_2_<32 hex digits>, which names
			// nothing a caller can use.
			TArray<TSharedPtr<FJsonValue>> Columns;
			for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
			{
				Columns.Add(MakeShared<FJsonValueString>(It->GetAuthoredName()));
			}
			Data->SetArrayField(TEXT("columns"), Columns);

			const FString RowFilter = GetString(Ctx.Params, TEXT("row"));
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 100)), 1, 1000);
			int32 Matching = 0;
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const auto& Pair : Table->GetRowMap())
			{
				if (!RowFilter.IsEmpty() && !Pair.Key.ToString().Equals(RowFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				++Matching;
				if (Rows.Num() >= Max)
				{
					// Keep counting past the cap so 'truncated' is a fact rather
					// than a guess.
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Pair.Key.ToString());
				TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
				if (FJsonObjectConverter::UStructToJsonObject(RowStruct, Pair.Value, Values, 0, 0,
					/*ExportCb=*/nullptr, EJsonObjectConversionFlags::SkipStandardizeCase))
				{
					Row->SetObjectField(TEXT("values"), Values);
				}
				else
				{
					// A row that simply omits 'values' reads as an empty row
					// rather than as one this tool could not serialize.
					Row->SetStringField(TEXT("error"), TEXT("row could not be converted to JSON"));
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			if (!RowFilter.IsEmpty() && Matching == 0)
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("no row named '%s' - call again without 'row' to list the %d row name(s) this table does have"),
					*RowFilter, Table->GetRowMap().Num()));
			}

			Data->SetArrayField(TEXT("rows"), Rows);
			Data->SetNumberField(TEXT("total_rows"), Table->GetRowMap().Num());
			Data->SetBoolField(TEXT("truncated"), Matching > Rows.Num());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("datatable_modify"),
		TEXT("Edit a DataTable. op: add_row {row, values?} | update_row {row, values} | remove_row {row} | rename_row {row, new_name}. 'values' maps column names (as datatable_query reports them) to JSON. Column names are checked before anything is written, so a wrong name changes nothing; a value that will not convert, or an asset path that resolves to nothing, is reported in 'failed' and the call comes back as an error even though the other columns were written. Marked dirty, not saved."),
		TEXT(R"json({"type":"object","properties":{"asset":{"type":"string"},"op":{"type":"string","enum":["add_row","update_row","remove_row","rename_row"]},"row":{"type":"string"},"new_name":{"type":"string"},"values":{"type":"object"}},"required":["asset","op","row"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			FString Error;
			UDataTable* Table = LoadTable(Ctx, Error);
			if (!Table)
			{
				return FUplinkToolResult::Error(Error);
			}
			const FString Op = GetString(Ctx.Params, TEXT("op"));
			const FName RowName(*GetString(Ctx.Params, TEXT("row")));
			if (RowName.IsNone())
			{
				// An empty string and the literal "None" both land on NAME_None,
				// which AddRow refuses with a bare null and which no row map
				// lookup ever matches.
				return FUplinkToolResult::Error(
					TEXT("'row' must name a row - it is empty or 'None', and 'None' is the engine's reserved empty name"));
			}
			const TSharedPtr<FJsonObject>* Values = nullptr;
			Ctx.Params->TryGetObjectField(FStringView(TEXT("values")), Values);
			const bool bHasValues = Values != nullptr && Values->IsValid();

			const bool bWritesValues = Op == TEXT("add_row") || Op == TEXT("update_row");
			if (!bWritesValues && Op != TEXT("remove_row") && Op != TEXT("rename_row"))
			{
				// Ahead of the 'values' guard below, which would otherwise answer
				// a misspelled op by complaining about the wrong parameter.
				return FUplinkToolResult::Error(TEXT("op must be add_row, update_row, remove_row, or rename_row"));
			}
			if (bHasValues && !bWritesValues)
			{
				// Accepting and ignoring them is how a caller comes to believe a
				// removed row was updated first.
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("%s does not write values - drop 'values', or call update_row for the cells"), *Op));
			}

			// Resolve every named column before touching the table, so a wrong
			// name cannot leave a half-written row behind.
			TArray<FColumnWrite> Writes;
			if (bHasValues && !ResolveColumns(Table, *Values, Writes, Error))
			{
				return FUplinkToolResult::Error(Error);
			}

			Table->Modify();
			TArray<FString> Failed;
			int32 Applied = 0;

			if (Op == TEXT("add_row"))
			{
				if (Table->GetRowMap().Contains(RowName))
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("row '%s' already exists"), *RowName.ToString()));
				}
				uint8* RowData = FDataTableEditorUtils::AddRow(Table, RowName);
				if (!RowData)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("the engine refused to add row '%s' - check the name is a usable FName"), *RowName.ToString()));
				}
				if (bHasValues)
				{
					Applied = WriteColumns(Writes, RowData, Failed);

					// AddRow announces the new row before the values go in, so
					// an open DataTable editor shows the defaults until this.
					FDataTableEditorUtils::BroadcastPostChange(
						Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
				}
			}
			else if (Op == TEXT("update_row"))
			{
				uint8* const* RowData = Table->GetRowMap().Find(RowName);
				if (!RowData)
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("row '%s' not found"), *RowName.ToString()));
				}
				if (!bHasValues)
				{
					return FUplinkToolResult::Error(TEXT("'values' is required for update_row"));
				}
				Applied = WriteColumns(Writes, *RowData, Failed);
				FDataTableEditorUtils::BroadcastPostChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
			}
			else if (Op == TEXT("remove_row"))
			{
				if (!FDataTableEditorUtils::RemoveRow(Table, RowName))
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("row '%s' not found"), *RowName.ToString()));
				}
			}
			else if (Op == TEXT("rename_row"))
			{
				// RenameRow answers three different problems with one false, so
				// separate them here rather than guess in the message.
				const FString NewText = GetString(Ctx.Params, TEXT("new_name"));
				if (NewText.IsEmpty())
				{
					return FUplinkToolResult::Error(TEXT("'new_name' is required for rename_row"));
				}
				const FName NewName(*NewText);
				if (!Table->GetRowMap().Contains(RowName))
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("row '%s' not found"), *RowName.ToString()));
				}
				if (Table->GetRowMap().Contains(NewName))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("a row named '%s' already exists - remove it first, or pick another name"), *NewName.ToString()));
				}
				if (!FDataTableEditorUtils::RenameRow(Table, RowName, NewName))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("the engine refused to rename '%s' to '%s'"), *RowName.ToString(), *NewName.ToString()));
				}
			}
			else
			{
				// Unreachable while the guard above names the same four ops -
				// kept so that adding a fifth without a branch here is refused
				// rather than falling through to the success return below.
				return FUplinkToolResult::Error(TEXT("op must be add_row, update_row, remove_row, or rename_row"));
			}

			Table->MarkPackageDirty();
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("total_rows"), Table->GetRowMap().Num());
			if (bHasValues)
			{
				Data->SetNumberField(TEXT("applied"), Applied);
			}
			if (Failed.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> FailedRows;
				for (const FString& Entry : Failed)
				{
					FailedRows.Add(MakeShared<FJsonValueString>(Entry));
				}
				Data->SetArrayField(TEXT("failed"), FailedRows);

				// Answering ok here is how a half-written row gets treated as
				// authored: the columns that landed are real, the rest are not,
				// and only the caller can tell which mattered.
				FUplinkToolResult Result = FUplinkToolResult::Ok(Data, FString::Printf(
					TEXT("%d of %d column(s) written to '%s'; the rest are listed in 'failed' - fix those values and call update_row again"),
					Applied, Applied + Failed.Num(), *RowName.ToString()));
				Result.bError = true;
				return Result;
			}
			return FUplinkToolResult::Ok(Data);
		});
}
