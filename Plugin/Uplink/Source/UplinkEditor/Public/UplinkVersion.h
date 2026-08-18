// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#pragma once

// The version reported over the wire: /status, the MCP serverInfo, and the
// provider version stamped on every tool this module registers.
//
// It is NOT a single source of truth, however much the old comment here
// wished it were. Uplink.uplugin's VersionName and bridge/package.json carry
// the same number and nothing checks that the three agree - which is how this
// one sat at 0.26.0 while the plugin shipped 0.29.0, quietly telling every
// client the wrong version for four releases. Bump all three together.
#define UPLINK_VERSION TEXT("0.30.0")
