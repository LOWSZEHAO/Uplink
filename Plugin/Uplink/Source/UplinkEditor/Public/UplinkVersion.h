// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

// The version reported over the wire: /status, the MCP serverInfo, and the
// provider version stamped on every tool this module registers.
//
// It is NOT a single source of truth: Uplink.uplugin's VersionName,
// bridge/package.json and the README release line carry the same number
// independently. Nothing checked that they agreed, which is how this one sat
// at 0.26.0 while the plugin shipped 0.29.0, quietly telling every client the
// wrong version for four releases. scripts/check_repo.ps1 compares all four
// now; bump them together.
#define UPLINK_VERSION TEXT("0.30.0")
