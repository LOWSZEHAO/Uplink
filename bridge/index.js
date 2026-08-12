#!/usr/bin/env node
// Uplink MCP bridge — stdio MCP server that forwards to the Uplink UE plugin's
// localhost REST API. Stays alive when the editor is closed (tools degrade to a
// "not connected" status instead of the MCP server dying).
// Copyright (c) 2026 Low Sze Hao. MIT License.

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  ListToolsRequestSchema,
  CallToolRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";

const UPLINK_URL = process.env.UPLINK_URL ?? "http://127.0.0.1:3777";
const REQUEST_TIMEOUT_MS = Number(process.env.UPLINK_TIMEOUT_MS ?? 30000);
const TOOL_CACHE_TTL_MS = Number(process.env.UPLINK_TOOL_CACHE_MS ?? 15000);

const log = (...args) => console.error("[uplink-bridge]", ...args);

async function fetchJson(path, options = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  try {
    const res = await fetch(`${UPLINK_URL}${path}`, {
      ...options,
      signal: controller.signal,
      headers: { "Content-Type": "application/json", ...(options.headers ?? {}) },
    });
    const text = await res.text();
    let json;
    try {
      json = JSON.parse(text);
    } catch {
      json = { raw: text };
    }
    return { ok: res.ok, status: res.status, json };
  } finally {
    clearTimeout(timer);
  }
}

async function editorStatus() {
  try {
    const { ok, json } = await fetchJson("/status");
    return ok ? json : null;
  } catch {
    return null;
  }
}

// --- tool list -------------------------------------------------------------

const STATUS_TOOL = {
  name: "status",
  description:
    "Uplink connection status: whether the Unreal editor is running with the Uplink plugin, " +
    "which engine/project is loaded, and whether a PIE (Play-In-Editor) session is active.",
  inputSchema: { type: "object", properties: {}, additionalProperties: false },
};

let toolCache = { at: 0, tools: [STATUS_TOOL] };

async function listTools() {
  if (Date.now() - toolCache.at < TOOL_CACHE_TTL_MS) return toolCache.tools;

  const status = await editorStatus();
  let tools = [STATUS_TOOL];
  if (status) {
    try {
      const { ok, json } = await fetchJson("/tools");
      if (ok && Array.isArray(json.tools)) {
        tools = tools.concat(
          json.tools
            .filter((t) => t.name !== "status") // the bridge serves its own status tool
            .map((t) => ({
            name: t.name,
            description: t.description ?? "",
            inputSchema: t.inputSchema ?? {
              type: "object",
              properties: {},
              additionalProperties: true,
            },
            ...(t.annotations ? { annotations: t.annotations } : {}),
          }))
        );
      }
    } catch (err) {
      log("tool list fetch failed:", err.message);
    }
  }
  toolCache = { at: Date.now(), tools };
  return tools;
}

// --- server ----------------------------------------------------------------

const server = new Server(
  { name: "uplink", version: "0.10.0" },
  { capabilities: { tools: {} } }
);

server.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: await listTools(),
}));

server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args = {} } = request.params;

  if (name === "status") {
    const status = await editorStatus();
    return status
      ? { content: [{ type: "text", text: JSON.stringify(status, null, 2) }] }
      : {
          content: [
            {
              type: "text",
              text:
                `Uplink is NOT connected (no editor answering at ${UPLINK_URL}). ` +
                "Start the Unreal editor with the Uplink plugin enabled, then try again.",
            },
          ],
        };
  }

  try {
    const { ok, status, json } = await fetchJson(
      `/tool/${encodeURIComponent(name)}`,
      { method: "POST", body: JSON.stringify(args) }
    );
    const isError = !ok || json.isError === true || json.success === false;
    return {
      content: [{ type: "text", text: JSON.stringify(json, null, 2) }],
      ...(isError ? { isError: true } : {}),
    };
  } catch (err) {
    return {
      content: [
        {
          type: "text",
          text:
            `Tool call failed: ${err.message}. The Unreal editor may not be running ` +
            `(no Uplink server at ${UPLINK_URL}).`,
        },
      ],
      isError: true,
    };
  }
});

const transport = new StdioServerTransport();
await server.connect(transport);
log(`ready — forwarding to ${UPLINK_URL}`);
