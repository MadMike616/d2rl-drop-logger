# Hook Inventory — Drop Logger

## Plugin metadata

| Field | Value |
|---|---|
| Plugin name | Drop Logger |
| Plugin ID | container-drop-logger |
| Release version | Unreleased 1.0.1 |
| Inventory updated | 2026-11-14 |
| Source revision | 1.0.1 source update on `main` |
| D2R build(s) audited | 3.4.95032 with D2RLoader 1.3.1-beta |
| D2RLoader SDK / Plugin ABI | PluginSDK 0.4.0 / Plugin ABI 4 |
| Audit status | Complete |

## Scope and method

Inspected `src/plugin.cpp`, `src/native_witnesses.hpp`, `src/chest_loot_witnesses.hpp`, and `src/treasure_drop_witnesses.hpp`. The plugin wraps the object dispatcher and the Treasure Class drop helper. It uses SDK lifecycle and game-thread services and checked native read helpers. The drop-helper witnesses were captured from the Loader-hosted 3.4.0 / 95032 image with Random Treasure Drop loaded. See `THIRD_PARTY_NOTICES.md`.

## Hook sites

| Site ID | Type | Target | Address / identifier | Operation | Guard / validation | Source reference | Notes |
|---|---|---|---|---|---|---|---|
| H-01 | Inline | Native object-operation dispatcher | RVA 0x593630 | Wraps the original operation to snapshot ground items before and after and record container identity/mode changes. | Checks the complete 731-byte dispatcher witness, room/unit access witnesses and container-handler witnesses; installs over the first 15 complete instruction bytes. Runtime player/game/object checks; bounded reads and snapshots; refuses unknown native layouts. | `src/plugin.cpp` — `OperateHook`, `D2RLoaderLoadPlugin`; `src/native_witnesses.hpp` | Original arguments and return value are forwarded. No RNG or seed writes and no item-generation calls. |
| H-02 | Inline | Treasure Class ground-drop helper | RVA 0x444680 | Observes the ground item returned for the source monster passed into the helper. | Checks the helper and its caller, descriptor writes, and Create Item entry. Supplies 21 whole prologue bytes as the SDK installation guard. Validates the caller return RVA 0x4410CC, active game, source identity and returned item. | `src/plugin.cpp` — `TreasureDropHook`, `D2RLoaderLoadPlugin`; `src/treasure_drop_witnesses.hpp` | Forwards all six arguments and the original result. Create Item at RVA 0x43CD50 is checked read-only. |

## Known collision overlaps

None identified in the current inventory; this is not a compatibility guarantee.

| Other plugin | Shared target | Relationship | Evidence / notes |
|---|---|---|---|
| None | N/A | N/A | Workspace inventory has no other recorded hook at RVA 0x593630. RuffnecKk Auto Interact calls this dispatcher as a helper; that call is not a hook. Map Seed Selector's hooks target different sites. |
| Random Treasure Drop | Treasure Class call chain | Different hook entries | Its entry is RVA 0x4404F0. Drop Logger checks and observes a downstream creation path without modifying that entry. |
| Grail On! 1.2.2 | Create Item entry RVA 0x43CD50 | Historical overlap in Drop Logger 1.0.0 | Grail On! checks the original prologue. Drop Logger 1.0.1 moved its observer to RVA 0x444680 and confirmed drops in an offline session with Grail On! loaded. |

## Audit notes

The object dispatcher hook and byte guards match 1.0.0. The SDK monster-death listener and breadcrumb service can still operate when the native capture checks fail. Native item snapshots require the Core `ReadWideUnitStat` export and validated data layouts.

The 0.0.13 standalone test loaded only Drop Logger in an unmodded offline game. It recorded two container openings, three container drops, three monster deaths, and two confirmed monster drops. All five item records included `item_area_id`, all scan windows completed, and zero records were lost.

On 2026-11-14, an offline 1.0.1 run with Grail On! 1.2.2 and Random Treasure Drop 0.1.43 loaded captured 15 confirmed drops across 30 deaths. All scan windows completed and zero records were lost. The run exercised the Treasure Class helper and SDK death listener. No containers were opened. Random Treasure Drop's bonus item generation uses a separate path and can produce uncertain ground-item observations.
