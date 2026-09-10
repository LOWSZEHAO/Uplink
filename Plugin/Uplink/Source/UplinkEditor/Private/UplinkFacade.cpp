// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// See UplinkFacade.h for why this exists at all.

#include "UplinkFacade.h"
#include "UplinkToolAreas.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Misc/Parse.h"

using namespace UplinkToolUtil;

namespace UplinkFacade
{
	const FString ListAreasName = TEXT("list_areas");
	const FString DescribeToolsName = TEXT("describe_tools");
	const FString CallToolName = TEXT("call_tool");

	bool IsEnabled()
	{
		// Read once. A caller that could flip this mid-session would change the
		// tool list under a client that has already read it, and MCP clients do
		// not re-read on a whim - they would go on calling names that are no
		// longer served, or never learn the ones that are.
		static const bool bEnabled = []()
		{
			const FString Mode = FPlatformMisc::GetEnvironmentVariable(TEXT("UPLINK_TOOL_LIST"));
			return !Mode.Equals(TEXT("flat"), ESearchCase::IgnoreCase);
		}();
		return bEnabled;
	}

	namespace
	{
		TSharedRef<FJsonObject> Entry(const FString& Name, const FString& Description,
			const FString& SchemaJson, bool bReadOnly)
		{
			TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
			Tool->SetStringField(TEXT("name"), Name);
			Tool->SetStringField(TEXT("description"), Description);
			if (const TSharedPtr<FJsonObject> Schema = FUplinkToolRegistry::ParseSchema(SchemaJson))
			{
				Tool->SetObjectField(TEXT("inputSchema"), Schema);
			}
			TSharedRef<FJsonObject> Annotations = MakeShared<FJsonObject>();
			Annotations->SetBoolField(TEXT("readOnlyHint"), bReadOnly);
			Annotations->SetBoolField(TEXT("destructiveHint"), false);
			Tool->SetObjectField(TEXT("annotations"), Annotations);
			return Tool;
		}

		/** {areas:[{area,summary,tools:[...]}], tool_count} - names, never schemas. */
		TSharedRef<FJsonObject> BuildAreaIndex(const FUplinkToolRegistry& Registry)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Areas;
			int32 Total = 0;

			for (const FUplinkArea& Area : UplinkToolAreas::All())
			{
				TArray<TSharedPtr<FJsonValue>> Names;
				for (const FString& Tool : Area.Tools)
				{
					// Only what this build actually serves. A row for a tool that
					// did not register - a provider plugin that is not loaded, a
					// schema that failed to parse - would send a caller after
					// something that is not there.
					if (Registry.Find(Tool))
					{
						Names.Add(MakeShared<FJsonValueString>(Tool));
						++Total;
					}
				}
				if (Names.Num() == 0)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("area"), Area.Name);
				Row->SetStringField(TEXT("summary"), Area.Summary);
				Row->SetArrayField(TEXT("tools"), Names);
				Areas.Add(MakeShared<FJsonValueObject>(Row));
			}

			// Anything the area table does not claim still has to be reachable.
			// check_repo makes this empty in a healthy build; serving it anyway
			// means a tool added without a table row is discoverable rather than
			// invisible until somebody notices.
			TArray<TSharedPtr<FJsonValue>> Unfiled;
			for (const auto& Pair : Registry.All())
			{
				if (Pair.Key != ListAreasName && Pair.Key != DescribeToolsName && Pair.Key != CallToolName
					&& UplinkToolAreas::AreaOf(Pair.Key).IsEmpty())
				{
					Unfiled.Add(MakeShared<FJsonValueString>(Pair.Key));
					++Total;
				}
			}
			if (Unfiled.Num() > 0)
			{
				Unfiled.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
					{ return A->AsString() < B->AsString(); });
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("area"), TEXT("other"));
				Row->SetStringField(TEXT("summary"), TEXT("Registered but not filed under any area."));
				Row->SetArrayField(TEXT("tools"), Unfiled);
				Areas.Add(MakeShared<FJsonValueObject>(Row));
			}

			Data->SetArrayField(TEXT("areas"), Areas);
			Data->SetNumberField(TEXT("tool_count"), Total);
			return Data;
		}
	}

	TArray<TSharedPtr<FJsonValue>> BuildToolList(const FUplinkToolRegistry& Registry)
	{
		const int32 Count = Registry.All().Num();

		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(MakeShared<FJsonValueObject>(Entry(
			ListAreasName,
			FString::Printf(
				TEXT("START HERE. Lists every one of this editor's %d tools by name, grouped into subject areas with a one-line summary each. Names and summaries only - no schemas, so it is cheap to call and cheap to keep. Read it first, decide which tools you need, then fetch just those schemas with %s and run them with %s. This server drives a live Unreal Editor: it can author Blueprint graphs, run and drive a game in PIE, and read or call anything the engine exposes by reflection."),
				Count, *DescribeToolsName, *CallToolName),
			TEXT(R"json({"type":"object","properties":{}})json"),
			/*bReadOnly=*/true)));

		Out.Add(MakeShared<FJsonValueObject>(Entry(
			DescribeToolsName,
			FString::Printf(
				TEXT("Full description and JSON input schema for the tools you name, or for a whole area, or for everything matching a search word. Ask for the few you are about to use rather than an area at a time - the schemas are the expensive part, which is the whole reason %s exists. Refuses a name it does not serve, with the near misses, rather than answering emptily."),
				*ListAreasName),
			TEXT(R"json({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"},"description":"Exact tool names"},"area":{"type":"string","description":"Every tool in one area, as named by list_areas"},"search":{"type":"string","description":"Tools whose name or description contains this"}}})json"),
			/*bReadOnly=*/true)));

		Out.Add(MakeShared<FJsonValueObject>(Entry(
			CallToolName,
			FString::Printf(
				TEXT("Run one of the tools by name, passing its own parameters in 'params'. The reply is exactly what that tool would have returned called directly - same validation, same undo transaction, same task id for anything long-running. Fetch the tool's schema from %s first; params are validated against it and a misspelt one is refused, not ignored."),
				*DescribeToolsName),
			TEXT(R"json({"type":"object","properties":{"tool":{"type":"string","description":"Tool name from list_areas"},"params":{"type":"object","description":"That tool's own parameters"}},"required":["tool"]})json"),
			/*bReadOnly=*/false)));

		return Out;
	}

	void RegisterTools(FUplinkToolRegistry& Registry)
	{
		// Registered whatever the mode: in flat mode they are three more tools
		// among the rest, and a client that has learnt to browse by area keeps
		// working. Only tools/list changes shape.
		// Spelt out rather than passed as ListAreasName so the name is greppable
		// at its registration site, which is what check_repo.ps1 reads. The
		// ensure below is what keeps the two from drifting.
		ensure(ListAreasName == TEXT("list_areas") && DescribeToolsName == TEXT("describe_tools") && CallToolName == TEXT("call_tool"));

		Registry.RegisterQuick(
			TEXT("list_areas"),
			TEXT("Every tool this editor serves, by name, grouped into subject areas. No schemas - call describe_tools for those."),
			TEXT(R"json({"type":"object","properties":{}})json"),
			/*bReadOnly=*/true,
			[&Registry](const FUplinkToolContext& Ctx) -> FUplinkToolResult
			{
				return FUplinkToolResult::Ok(BuildAreaIndex(Registry));
			});

		Registry.RegisterQuick(
			TEXT("describe_tools"),
			TEXT("Full description and input schema for the tools you name, an area, or a search word."),
			TEXT(R"json({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}},"area":{"type":"string"},"search":{"type":"string"}}})json"),
			/*bReadOnly=*/true,
			[&Registry](const FUplinkToolContext& Ctx) -> FUplinkToolResult
			{
				TArray<FString> Wanted;

				const TArray<TSharedPtr<FJsonValue>>* NamesJson = nullptr;
				if (Ctx.Params->TryGetArrayField(FStringView(TEXT("names")), NamesJson) && NamesJson)
				{
					for (const TSharedPtr<FJsonValue>& Value : *NamesJson)
					{
						if (Value.IsValid())
						{
							Wanted.AddUnique(Value->AsString());
						}
					}
				}

				const FString Area = GetString(Ctx.Params, TEXT("area"));
				if (!Area.IsEmpty())
				{
					bool bFoundArea = false;
					for (const FUplinkArea& Row : UplinkToolAreas::All())
					{
						if (Row.Name.Equals(Area, ESearchCase::IgnoreCase))
						{
							bFoundArea = true;
							for (const FString& Tool : Row.Tools)
							{
								Wanted.AddUnique(Tool);
							}
						}
					}
					if (!bFoundArea)
					{
						TArray<FString> Known;
						for (const FUplinkArea& Row : UplinkToolAreas::All())
						{
							Known.Add(Row.Name);
						}
						return FUplinkToolResult::Error(FString::Printf(
							TEXT("no area '%s'. Areas: %s"), *Area, *FString::Join(Known, TEXT(", "))));
					}
				}

				const FString Search = GetString(Ctx.Params, TEXT("search"));
				if (!Search.IsEmpty())
				{
					for (const auto& Pair : Registry.All())
					{
						if (Pair.Key.Contains(Search, ESearchCase::IgnoreCase)
							|| Pair.Value.Info.Description.Contains(Search, ESearchCase::IgnoreCase))
						{
							Wanted.AddUnique(Pair.Key);
						}
					}
				}

				if (Wanted.Num() == 0)
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("give 'names', 'area' or 'search'. %s lists what there is."), *ListAreasName));
				}

				TArray<TSharedPtr<FJsonValue>> Rows;
				TArray<FString> Unknown;
				Wanted.Sort();
				for (const FString& Name : Wanted)
				{
					const FUplinkToolDef* Def = Registry.Find(Name);
					if (!Def)
					{
						Unknown.Add(Name);
						continue;
					}
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Def->Info.Name);
					Row->SetStringField(TEXT("description"), Def->Info.Description);
					if (Def->Info.InputSchema.IsValid())
					{
						Row->SetObjectField(TEXT("input_schema"), Def->Info.InputSchema);
					}
					const FString Owner = UplinkToolAreas::AreaOf(Def->Info.Name);
					if (!Owner.IsEmpty())
					{
						Row->SetStringField(TEXT("area"), Owner);
					}
					Row->SetBoolField(TEXT("read_only"), Def->Info.bReadOnly);
					if (Def->Info.bDestructive)
					{
						Row->SetBoolField(TEXT("destructive"), true);
					}
					if (Def->Info.bRequiresPie)
					{
						Row->SetBoolField(TEXT("requires_pie"), true);
					}
					if (Def->Info.bLongRunning)
					{
						Row->SetBoolField(TEXT("long_running"), true);
					}
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}

				// A name that does not exist is refused rather than quietly
				// dropped: silently returning four of five schemas reads as
				// success and the fifth call then fails somewhere less obvious.
				if (Unknown.Num() > 0)
				{
					TArray<FString> Registered;
					for (const auto& Pair : Registry.All())
					{
						Registered.Add(Pair.Key);
					}
					TArray<FString> Near;
					for (const FString& Miss : Unknown)
					{
						const FString Closest = NearestName(Miss, Registered);
						if (!Closest.IsEmpty())
						{
							Near.AddUnique(FString::Printf(TEXT("'%s' for '%s'"), *Closest, *Miss));
						}
					}
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no such tool: %s.%s"),
						*FString::Join(Unknown, TEXT(", ")),
						Near.Num() > 0
							? *FString::Printf(TEXT(" Did you mean %s?"), *FString::Join(Near, TEXT(", ")))
							: *FString::Printf(TEXT(" %s lists every name."), *ListAreasName)));
				}

				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetArrayField(TEXT("tools"), Rows);
				Data->SetNumberField(TEXT("count"), Rows.Num());
				return FUplinkToolResult::Ok(Data);
			});
	}
}
