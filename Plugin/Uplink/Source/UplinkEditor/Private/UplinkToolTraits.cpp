// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
//
// One table saying which tools can do real damage, cost real time, or need a
// live game. The facts could live at each registration site, but then nobody
// could answer "what can this server break?" without reading twenty files, and
// a wrong answer there is invisible. Here it is one screen you can audit.
//
// What earns each flag:
//   Destructive        - destroys or overwrites work that undo will not bring
//                        back. Not merely "has a remove op": bp_modify can
//                        delete a node and datatable_modify a row, but both run
//                        inside a transaction the caller can walk back with
//                        edit_history, and flagging every authoring tool would
//                        leave the flag meaning nothing.
//   ArbitraryExecution - runs whatever the caller names rather than a fixed
//                        operation, so the blast radius is the engine, not the
//                        tool's parameters.
//   RequiresPie        - fails, or means nothing, without a live PIE session.
//                        input_record is not one of these despite the name: it
//                        is a Slate tap and records the editor just as happily.
//   LongRunning        - routinely many seconds, so slowness is not a hang.
//
// `save` is deliberately not destructive. It does overwrite the .uasset on
// disk, but what it writes is the edit the caller already made in memory;
// flagging it would only teach an agent to leave work unsaved, which is the
// one way to actually lose it here.

#include "UplinkToolRegistry.h"
#include "UplinkEditorModule.h"

namespace
{
	/**
	 * Bits for one row. Prefixed because this file is compiled into a unity
	 * blob with a hundred engine headers and "Destructive" is not a name to
	 * claim unqualified.
	 */
	enum EUplinkToolTrait : uint8
	{
		Trait_Destructive        = 1 << 0,
		Trait_ArbitraryExecution = 1 << 1,
		Trait_RequiresPie        = 1 << 2,
		Trait_LongRunning        = 1 << 3,
	};

	struct FUplinkToolTraitRow
	{
		const TCHAR* Name;
		uint8 Traits;
	};

	/** Alphabetical, so a missing or duplicated row is easy to spot by eye. */
	const FUplinkToolTraitRow GToolTraits[] =
	{
		{ TEXT("asset_import"),      Trait_Destructive | Trait_LongRunning },      // imports replace an existing asset in place
		{ TEXT("bp_find_broken"),    Trait_LongRunning },                          // loads every Blueprint in the project
		{ TEXT("bp_references"),     Trait_LongRunning },                          // opens unloaded packages to find callers
		{ TEXT("bp_repair"),         Trait_Destructive | Trait_LongRunning },      // rewrites and recompiles whole assets, outside any transaction
		{ TEXT("call_function"),     Trait_ArbitraryExecution },
		{ TEXT("console_command"),   Trait_ArbitraryExecution },
		{ TEXT("delete_actors"),     Trait_Destructive },
		{ TEXT("frame_strip"),       Trait_LongRunning },                          // N frames at a fixed interval is the tool
		{ TEXT("input_action"),      Trait_RequiresPie },
		{ TEXT("input_key"),         Trait_RequiresPie },
		{ TEXT("input_replay"),      Trait_RequiresPie | Trait_LongRunning },      // replays a take at its recorded pace
		{ TEXT("landscape_create"),  Trait_LongRunning },                          // heightmap import and resample
		{ TEXT("live_compile"),      Trait_LongRunning },                          // a C++ compile
		{ TEXT("navigate_to"),       Trait_RequiresPie | Trait_LongRunning },      // the pawn walks there in real time
		{ TEXT("niagara_compile"),   Trait_LongRunning },                          // blocks until the system finishes compiling
		{ TEXT("pie_pause"),         Trait_RequiresPie },
		{ TEXT("pie_resume"),        Trait_RequiresPie },
		{ TEXT("pie_start"),         Trait_LongRunning },                          // loading a map and reaching BeginPlay
		{ TEXT("pie_step"),          Trait_RequiresPie },
		{ TEXT("pie_stop"),          Trait_RequiresPie },
		{ TEXT("player_info"),       Trait_RequiresPie },
		{ TEXT("player_teleport"),   Trait_RequiresPie },
		{ TEXT("possess"),           Trait_RequiresPie },
		{ TEXT("profile_capture"),   Trait_LongRunning },                          // samples for the seconds it was asked for
		{ TEXT("run_scenario"),      Trait_ArbitraryExecution | Trait_LongRunning }, // a step may name call_function or console_command
		{ TEXT("run_tests"),         Trait_LongRunning },
		{ TEXT("viewport_annotate"), Trait_RequiresPie },                          // refuses without a player controller to see from
		{ TEXT("wait_until"),        Trait_LongRunning },                          // waiting is what it does
	};
}

void FUplinkToolRegistry::ApplyTraits()
{
	TArray<FString> Unknown;
	TArray<FString> Broken;

	for (const FUplinkToolTraitRow& Row : GToolTraits)
	{
		const FString Name(Row.Name);
		FUplinkToolDef* Def = Tools.Find(Name);
		if (!Def)
		{
			// A tool dropped for an unparseable schema is a different fault
			// from a name nobody updated, and the fix is in a different file.
			(SkippedSchemaTools.Contains(Name) ? Broken : Unknown).Add(Name);
			continue;
		}

		// Assigned rather than or-ed, so this table is the whole truth about a
		// tool and running it twice cannot leave a stale flag behind.
		Def->Info.bDestructive = (Row.Traits & Trait_Destructive) != 0;
		Def->Info.bArbitraryExecution = (Row.Traits & Trait_ArbitraryExecution) != 0;
		Def->Info.bRequiresPie = (Row.Traits & Trait_RequiresPie) != 0;
		Def->Info.bLongRunning = (Row.Traits & Trait_LongRunning) != 0;
	}

	if (Unknown.Num() > 0)
	{
		// The point of the table: a rename six months from now silently stops
		// marking a destructive tool as destructive unless this shouts.
		UE_LOG(LogUplink, Error,
			TEXT("Tool trait table names %d unregistered tool%s: %s. Renamed or removed without updating ")
			TEXT("UplinkToolTraits.cpp - correct the name there, or delete the row."),
			Unknown.Num(), Unknown.Num() > 1 ? TEXT("s") : TEXT(""), *FString::Join(Unknown, TEXT(", ")));
		ensureMsgf(false,
			TEXT("Uplink: the tool trait table names tools that are not registered; see the log for which."));
	}

	if (Broken.Num() > 0)
	{
		UE_LOG(LogUplink, Warning,
			TEXT("Tool trait table: %s never registered, so their traits went nowhere. The table is right ")
			TEXT("here - it is the tool's input schema that needs fixing."),
			*FString::Join(Broken, TEXT(", ")));
	}

	// This runs after every tool has registered, which makes it the only place
	// that can report how much of the tool list went missing on the way.
	if (SkippedSchemaTools.Num() > 0)
	{
		TArray<FString> Names = SkippedSchemaTools.Array();
		Names.Sort();
		UE_LOG(LogUplink, Error,
			TEXT("%d tool%s not registered because the input schema would not parse: %s. They are not ")
			TEXT("callable, and no client can see that they are missing - fix the JSON."),
			Names.Num(), Names.Num() > 1 ? TEXT("s") : TEXT(""), *FString::Join(Names, TEXT(", ")));
	}
}
