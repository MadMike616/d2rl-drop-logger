# Drop Logger

D2RLoader plugin by MadMike, version 0.0.13.

Records container openings, monster deaths, and observed ground items during offline play. Use the logs to reproduce loot tests, compare runs, or correlate another plugin's diagnostic messages with in-game events.

## Install

Copy `d2rl-container-drop-logger.dll` to `d2rloader/plugins` under your mod or the game directory while D2RLoader is closed. Start D2RLoader and join an offline game. No TOML configuration is required.

Tested with D2RLoader 1.3.1-beta and Diablo II: Resurrected 3.4.95032 on Windows x64. The DLL uses Plugin ABI 4. Each native capture feature checks the code and data layout it needs at startup.

## Output

Each Loader launch creates two matching files in the active scope's `d2rloader/logs/drop logs` directory. For a mod installation, this is under that mod's folder.

- `drops-2026-11-10_18-35-12.406Z_pid-1234.txt` is the readable log. Each game heading shows its sequential number, difficulty, and character name when the local player is ready, followed by container openings, monster deaths, and indented item lines.
- `drops-2026-11-10_18-35-12.406Z_pid-1234.jsonl` contains the same observations as structured records, with a run identifier per game.

The filename includes the launch time in UTC and the Loader process ID. The logger keeps the latest five session pairs and removes older files with its session naming pattern when it opens a new pair. The legacy `container-drop-observations.jsonl` remains available at its original path. D2RLoader's own plugin diagnostic log reports admission and observation status.

The illustrative [text log](examples/sample-session.txt) and [JSONL log](examples/sample-session.jsonl) show the same game, including a confirmed monster drop and a ground item with a possible source.

- `container_open`: opening order, object class ID and name, container kind, loot rule, coordinates, level, native result and capture completeness. Chest records also include the lock state before opening. The text log shows the class name and rule together, for example `JungleChest2 [super chest, locked]`.
- `drop_during_open`: a new ground-item identity observed between snapshots immediately before and after the original container operation.
- `monster_death`: a per-game death number, monster and direct killer identities, area, and position when it can be validated. `position_status` reports what prevented a lookup when a position is unavailable.
- `monster_drop`: a confirmed source monster and the returned ground item from the validated native Treasure Class path. Item details are refreshed after creation when the item remains available. A record may reference an SDK death number when that event arrives before the next sample.
- `later_ground_item`: a newly seen ground item during the two seconds after an opening or a located death. `candidate_openings` and `candidate_deaths` reference nearby events by their per-game numbers. `exact_source` is always false for these records. A death candidate must be in the same area as the item.
- Item records include the base code and name, item quality, and selected layer-zero stats such as gold amount (stat 14), quantity (70), durability (72/73), and sockets (194). The base name is plain text with in-game color tokens removed. JSON includes `quality_id` for the native value and `quality` for the label, such as `set` or `unique`. Unavailable values are `null`.
- In schema 4, `item_area_id` is the area containing the item. Schema 3 and earlier used `item_level` for the same area ID.
- `game_joined` records entry into a game and whether the selector DLL is loaded. Schema version 3 adds `game_ready` with the local player's ID, difficulty, and character name. Difficulty and character name are `null` when the logger cannot verify them. `game_left` reports opening and death counts and any lost records. All three include the sequential `game_number`. Use the loader's `run` ID to match those events to the game's other records.
- `capture_start` records the game build, loader version, active scope, schema version, and which capture features are available. `capture_diagnostic` reports failed native checks and any overlapping patch owner D2RLoader can identify.
- `sampling_window` reports the coverage of a finished two-second scan window and the number of candidate items seen. A zero count with complete coverage means the logger saw no candidate items. An incomplete, failed, skipped, or interrupted window cannot establish that result. If the game ends before a window finishes, its final record may be absent. Container openings also report their immediate before/after scan status and observed item count.
- `test_plugin` identifies a plugin under test and its reported version. `plugin_breadcrumb` records that plugin's own diagnostic message, action, and optional correlation ID alongside the observations.

Use object class, level and coordinates to compare the same container across runs; runtime object IDs and item IDs change. The opening counter includes scenery containers as well as chests and racks, but excludes ordinary doors and switches.

Regular and super chests can share an object class. The logger reads the flag on the individual chest before the native operation to distinguish them. The sparkly ConsolationChest has a separate branch. The recorded rule identifies the branch family. It does not include the random quality roll inside that branch. If a class name or chest flag cannot be validated, the logs identify the unknown field.

`tools/summarize_drops.py <session.jsonl>` summarizes repeated container drop sets and rack base types. It also accepts the legacy JSONL, keeps direct observations and uncertain later items separate, and excludes incomplete direct captures from its strict comparisons.

## Observation limits

The logger forwards every original operation once with its original arguments. It never supplies a seed, calls a random-number generator, or edits drops. The object-dispatch and item-creation hooks and the SDK monster-death listener observe the authoritative game thread. File writes run on a separate worker.

Container observation requires the original native code and structure witnesses to match at load. Container modes and identities are checked again at runtime. Snapshot limits, failed reads, queue overflow and uncertain source attribution can affect a capture. Very short-lived drops collected before a snapshot can be missed. Keep the same logger enabled in both comparison conditions because observation itself adds execution time.

The monster-death listener runs after normal loot handling. The logger periodically remembers ground items near the player so it can filter items it has already seen. In an unobserved area, an item that was already there can still appear as a candidate. Close deaths can share candidates, and player-dropped items can appear in the same window. If container checks fail, the SDK death listener and breadcrumb service can still run. If the native ground or position checks fail, death identities remain available through the listener and candidate scanning is disabled.

Confirmed monster drops use a separate item-creation site from Random Treasure Drop's Treasure Class hook. Drop Logger can capture stock Treasure Class drops independently of that plugin. Random Treasure Drop's bonus items use a separate creation and placement path and can appear as uncertain ground items. Capture falls back to the SDK death listener and ground scans when the exact-source checks fail.

## Breadcrumbs for plugin developers

Include [`drop_logger_breadcrumbs.h`](include/drop_logger_breadcrumbs.h) in your plugin and query the SDK's `PluginCommunicationService`. Acquire the `breadcrumbs` service from `container-drop-logger` with version 1 and keep the SDK lease while calling it. Acquire on first use and retry later if the logger has yet to load. Release the lease before your plugin unloads. Treat the service as optional so your plugin also runs independently.

```cpp
#include <D2RLPlugin/api.h>
#include "drop_logger_breadcrumbs.h"

// Acquire once per consumer and retain the lease while using the service.
D2RL::PluginCommunication::ServiceLease<DropLogger::BreadcrumbsV1> breadcrumbs;

bool FindDropLogger(const D2RL::PluginContext* ctx) {
    const D2RL::PluginCommunicationService* service{};
    return ctx->QueryService(&service) == D2RL::ServiceQueryResult::Success
        && D2RL::HasPluginCommunicationServiceField(service, D2RL::PluginCommunicationServiceRequiredSize)
        && D2RL::PluginCommunication::Acquire(ctx, service, DropLogger::PluginId,
            DropLogger::BreadcrumbService, DropLogger::BreadcrumbVersion,
            sizeof(DropLogger::BreadcrumbsV1), &breadcrumbs) == D2RL::PluginCommunication::Result::Success;
}

void LogTestStep(const D2RL::PluginContext* ctx) {
    if (!breadcrumbs) return;
    const DropLogger::Breadcrumb marker{
        .correlationId = 42,
        .pluginVersion = "1.2.0",
        .action = "drop_rule_skipped",
        .message = "Item ineligible"
    };
    (void)breadcrumbs->write(ctx, &marker);
}
```

Send `BreadcrumbKind::IdentifyTestPlugin` with your version once per session to add a test-plugin declaration to the capture. A marker can use correlation ID zero when no ID is needed. Use the same nonzero ID for related messages from your own plugin. Marker text is copied into both logs as supplied by the caller. Use bounded, null-terminated UTF-8 fields and keep private data out of the messages. Calls can come from any thread and return once the record has been queued. `QueueFull` indicates an overloaded capture and increments the lost-record count.

## Build from source

Requires Windows x64, MSVC, Windows SDK, CMake 3.29 or later, and official PluginSDK 0.4.0 / Plugin ABI 4 (commit `03a5af9e0feb3f95f95e148e330e01f6635950df`). The default SDK path is the adjacent `PluginSDK/SDK-0.4.0`; set `D2RL_SDK_ROOT` for a different location.

From a Visual Studio x64 Developer Command Prompt:

```text
cmake -S . -B ../build-container-drop-logger -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build ../build-container-drop-logger
```

## License

MIT; see `LICENSE`. Native reference witnesses are credited in `THIRD_PARTY_NOTICES.md`.
