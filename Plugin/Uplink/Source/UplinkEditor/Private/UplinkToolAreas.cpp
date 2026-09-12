// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
//
// Which subject each tool belongs to, in one table.
//
// This exists because a server's whole tool list is not free. Every MCP client
// re-reads it on connect and carries it for the rest of the session, so at a
// hundred-odd tools the schemas alone cost more context than most of the
// conversations they are there to serve. The areas below are what let a client
// take the names first and the schemas only for what it is about to use.
//
// The grouping is by SUBJECT, not by source file or by engine module: a caller
// asking "how do I drive a running game" wants pie, input and observe together,
// and does not care that they register from three different .cpp files.
//
// Every registered tool must appear exactly once. scripts/check_repo.ps1
// enforces both halves - a tool missing here would be invisible to a client
// browsing by area, and a name listed here that no longer registers would send
// one looking for a tool that is gone.

#include "UplinkToolAreas.h"

namespace
{
	struct FUplinkAreaRow
	{
		const TCHAR* Name;
		const TCHAR* Summary;
		const TCHAR* Tools;
	};

	/**
	 * Ordered the way an unfamiliar project is approached rather than
	 * alphabetically: find out where you are, read, author, then run and watch.
	 * A client showing this list to a model is showing it a route.
	 */
	const FUplinkAreaRow GAreas[] =
	{
		{
			TEXT("project"),
			TEXT("Orientation: what this project is, what is loaded, what the code offers, what the log says."),
			TEXT("project_entry status plugin_list plugin_enable output_log class_info find_functions live_compile")
		},
		{
			TEXT("assets"),
			TEXT("Find, create, import, duplicate, rename, move, delete and save assets, and trace what depends on what."),
			TEXT("asset_create asset_search asset_import asset_dependencies asset_referencers asset_modify save")
		},
		{
			TEXT("blueprints"),
			TEXT("Author Blueprint graphs: nodes, wires, variables, components, compile, and repair what is broken."),
			TEXT("bp_create bp_modify bp_query bp_add_component bp_compile bp_find_broken bp_repair bp_references")
		},
		{
			TEXT("reflection"),
			TEXT("The escape hatch: call any BlueprintCallable UFUNCTION, read or write any UPROPERTY by dotted path.")
			TEXT(" Reaches systems that have no dedicated tool here."),
			TEXT("call_function get_property set_property")
		},
		{
			TEXT("world"),
			TEXT("Levels and what is in them: open, diff, spawn, move, delete, scatter, trace, stream."),
			TEXT("level_actors level_diff level_new level_open worlds spawn_actor spawn_batch spawn_volume")
			TEXT(" delete_actors move_actor actor_components get_world_state streaming_status streaming_control lighting_setup")
			TEXT(" foliage_scatter landscape_create navigate_to trace")
		},
		{
			TEXT("pie"),
			TEXT("Drive a running game: start, stop, pause, step, possess a pawn, move the player."),
			TEXT("pie_start pie_stop pie_pause pie_resume pie_step pie_status possess player_info player_teleport")
		},
		{
			TEXT("input"),
			TEXT("Send keys and Enhanced Input actions into a live session, and record or replay a sequence of them."),
			TEXT("input_key input_action input_map input_record input_replay")
		},
		{
			TEXT("observe"),
			TEXT("Watch what happened: events, frames, screenshots, timings, and waiting on a condition before asserting."),
			TEXT("observe watch_events unwatch drain_events wait_until perf_stats profile_capture")
			TEXT(" viewport_screenshot viewport_annotate capture_widget frame_strip dialog_state")
		},
		{
			TEXT("editor_ui"),
			TEXT("Read the editor's own Slate UI, and point the level viewport camera."),
			TEXT("ui_tree ui_live click_widget viewport_camera")
		},
		{
			TEXT("data"),
			TEXT("Data tables, user-defined structs and enums: the shapes a project's content is authored against."),
			TEXT("datatable_create datatable_modify datatable_query struct_modify struct_query enum_modify enum_query")
		},
		{
			TEXT("animation"),
			TEXT("Skeletons, sockets, animation assets and blueprints, motion matching and choosers."),
			TEXT("anim_query anim_modify animbp_query skeleton_query socket_modify posesearch_query")
			TEXT(" motionmatch_debug chooser_query")
		},
		{
			TEXT("niagara"),
			TEXT("Niagara systems and emitters: modules, renderers, inputs and user parameters."),
			TEXT("niagara_create niagara_query niagara_compile niagara_add_module niagara_remove_module")
			TEXT(" niagara_module_inputs niagara_renderer niagara_set_input niagara_set_user_param")
		},
		{
			TEXT("pcg"),
			TEXT("Procedural content graphs: build them, wire them, run them."),
			TEXT("pcg_create pcg_query pcg_add_node pcg_connect pcg_generate")
		},
		{
			TEXT("material"),
			TEXT("Read material graphs. Authoring goes through call_function against MaterialEditingLibrary - see TOOLS.md."),
			TEXT("material_query")
		},
		{
			TEXT("ai"),
			TEXT("Read a running pawn's brain: behaviour tree, active task, blackboard."),
			TEXT("ai_query")
		},
		{
			TEXT("sequencer"),
			TEXT("Read level sequences: range, frame rate, bindings, section times."),
			TEXT("sequence_query")
		},
		{
			TEXT("umg"),
			TEXT("Widget blueprints: read the tree, add a widget, remove one or move it to another panel. Layout is set_property against the slot paths widget_tree reports."),
			TEXT("widget_add widget_modify widget_tree")
		},
		{
			TEXT("testing"),
			TEXT("Replay a decided sequence and assert on it, run the project's automation tests, walk the undo stack."),
			TEXT("run_scenario run_tests edit_history console_command")
		},
		{
			TEXT("tasks"),
			TEXT("Poll, fetch and cancel work that outlives one call."),
			TEXT("task_status task_result task_list task_cancel")
		},
	};
}

namespace UplinkToolAreas
{
	TArray<FUplinkArea> All()
	{
		TArray<FUplinkArea> Out;
		Out.Reserve(UE_ARRAY_COUNT(GAreas));
		for (const FUplinkAreaRow& Row : GAreas)
		{
			FUplinkArea Area;
			Area.Name = Row.Name;
			Area.Summary = Row.Summary;
			FString(Row.Tools).ParseIntoArrayWS(Area.Tools);
			Out.Add(MoveTemp(Area));
		}
		return Out;
	}

	FString AreaOf(const FString& ToolName)
	{
		for (const FUplinkAreaRow& Row : GAreas)
		{
			TArray<FString> Names;
			FString(Row.Tools).ParseIntoArrayWS(Names);
			if (Names.Contains(ToolName))
			{
				return Row.Name;
			}
		}
		return FString();
	}
}
