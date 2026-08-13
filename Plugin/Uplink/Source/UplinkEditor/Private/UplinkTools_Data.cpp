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
		UDataTable* Table = LoadObject<UDataTable>(nullptr, *Path);
		if (!Table)
		{
			OutError = FString::Printf(TEXT("data table not found: %s"), *Path);
		}
		return Table;
	}

	bool ImportRowValues(UDataTable* Table, uint8* RowData, const TSharedPtr<FJsonObject>& Values,
		TArray<FString>& OutFailed)
	{
		const UScriptStruct* RowStruct = Table->GetRowStruct();
		bool bAny = false;
		for (const auto& Pair : Values->Values)
		{
			const FString PropertyName = UplinkCompat::JsonKeyToString(Pair.Key);
			FProperty* Property = FindFProperty<FProperty>(RowStruct, *PropertyName);
			if (!Property)
			{
				OutFailed.Add(FString::Printf(TEXT("%s: no such column"), *PropertyName));
				continue;
			}
			if (FJsonObjectConverter::JsonValueToUProperty(
				Pair.Value, Property, Property->ContainerPtrToValuePtr<void>(RowData)))
			{
				bAny = true;
			}
			else
			{
				OutFailed.Add(FString::Printf(TEXT("%s: JSON conversion failed"), *PropertyName));
			}
		}
		return bAny || Values->Values.Num() == 0;
	}
}

void UplinkTools::RegisterData(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("datatable_create"),
		TEXT("Create a DataTable asset for a row struct (any USTRUCT with a FTableRowBase parent), e.g. row_struct '/Script/MyGame.FWeaponRow' or a full user-struct path. In memory, marked dirty - save with EditorAssetLibrary.SaveAsset."),
		TEXT(R"json({"type":"object","properties":{"path":{"type":"string","description":"e.g. /Game/Data/DT_Weapons"},"row_struct":{"type":"string"}},"required":["path","row_struct"]})json"),
		/*bReadOnly=*/false,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			const FString Path = GetString(Ctx.Params, TEXT("path"));
			if (!Path.StartsWith(TEXT("/Game/")))
			{
				return FUplinkToolResult::Error(TEXT("'path' must start with /Game/"));
			}
			if (LoadObject<UObject>(nullptr, *Path))
			{
				return FUplinkToolResult::Error(TEXT("an asset already exists at that path"));
			}
			const FString StructPath = GetString(Ctx.Params, TEXT("row_struct"));
			UScriptStruct* RowStruct = LoadObject<UScriptStruct>(nullptr, *StructPath);
			if (!RowStruct)
			{
				return FUplinkToolResult::Error(FString::Printf(TEXT("row struct not found: %s"), *StructPath));
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
		TEXT("Read a DataTable: row struct, columns, and rows as JSON. 'row' filters to one row."),
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
			if (!RowStruct)
			{
				return FUplinkToolResult::Error(TEXT("data table has no row struct"));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("row_struct"), RowStruct->GetPathName());

			TArray<TSharedPtr<FJsonValue>> Columns;
			for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
			{
				Columns.Add(MakeShared<FJsonValueString>(It->GetName()));
			}
			Data->SetArrayField(TEXT("columns"), Columns);

			const FString RowFilter = GetString(Ctx.Params, TEXT("row"));
			const int32 Max = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max"), 100)), 1, 1000);
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const auto& Pair : Table->GetRowMap())
			{
				if (!RowFilter.IsEmpty() && !Pair.Key.ToString().Equals(RowFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (Rows.Num() >= Max)
				{
					break;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Pair.Key.ToString());
				TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
				if (FJsonObjectConverter::UStructToJsonObject(RowStruct, Pair.Value, Values, 0, 0))
				{
					Row->SetObjectField(TEXT("values"), Values);
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Data->SetArrayField(TEXT("rows"), Rows);
			Data->SetNumberField(TEXT("total_rows"), Table->GetRowMap().Num());
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("datatable_modify"),
		TEXT("Edit a DataTable. op: add_row {row, values?} | update_row {row, values} | remove_row {row} | rename_row {row, new_name}. 'values' maps column names to JSON. Marked dirty, not saved."),
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
			const TSharedPtr<FJsonObject>* Values = nullptr;
			Ctx.Params->TryGetObjectField(FStringView(TEXT("values")), Values);

			Table->Modify();
			TArray<FString> Failed;

			if (Op == TEXT("add_row"))
			{
				if (Table->GetRowMap().Contains(RowName))
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("row '%s' already exists"), *RowName.ToString()));
				}
				uint8* RowData = FDataTableEditorUtils::AddRow(Table, RowName);
				if (!RowData)
				{
					return FUplinkToolResult::Error(TEXT("AddRow failed"));
				}
				if (Values && Values->IsValid())
				{
					ImportRowValues(Table, RowData, *Values, Failed);
				}
			}
			else if (Op == TEXT("update_row"))
			{
				uint8* const* RowData = Table->GetRowMap().Find(RowName);
				if (!RowData)
				{
					return FUplinkToolResult::Error(FString::Printf(TEXT("row '%s' not found"), *RowName.ToString()));
				}
				if (!Values || !Values->IsValid())
				{
					return FUplinkToolResult::Error(TEXT("'values' is required for update_row"));
				}
				ImportRowValues(Table, *RowData, *Values, Failed);
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
				const FName NewName(*GetString(Ctx.Params, TEXT("new_name")));
				if (!FDataTableEditorUtils::RenameRow(Table, RowName, NewName))
				{
					return FUplinkToolResult::Error(TEXT("rename failed (row missing or name taken?)"));
				}
			}
			else
			{
				return FUplinkToolResult::Error(TEXT("op must be add_row, update_row, remove_row, or rename_row"));
			}

			Table->MarkPackageDirty();
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("total_rows"), Table->GetRowMap().Num());
			if (Failed.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> FailedRows;
				for (const FString& Entry : Failed)
				{
					FailedRows.Add(MakeShared<FJsonValueString>(Entry));
				}
				Data->SetArrayField(TEXT("failed"), FailedRows);
			}
			return FUplinkToolResult::Ok(Data);
		});
}
