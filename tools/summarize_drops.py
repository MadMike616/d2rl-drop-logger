"""Summarize container observations without treating temporal candidates as proven drops."""
import collections
import json
from pathlib import Path
import sys

def signature(items):
    # Compare only what was actually recorded, not item identity or all possible affixes.
    return tuple(sorted(
        (i.get("code", ""), i.get("quality") or "unknown", tuple(sorted(i.get("stats_layer0", {}).items())))
        for i in items
    ))

def summarize(path):
    opens = {}
    direct = collections.defaultdict(list)
    later = collections.Counter()
    lost_by_capture = collections.Counter()
    gaps = 0
    with path.open(encoding="utf-8-sig") as stream:
        for line_number, line in enumerate(stream, 1):
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                print(f"Skipped incomplete/invalid JSON at line {line_number}.", file=sys.stderr)
                continue
            kind = row.get("type")
            key = (row.get("capture"), row.get("run"), row.get("opening"))
            if kind == "container_open":
                opens[key] = row
            elif kind == "drop_during_open":
                direct[key].append(row)
            elif kind == "later_ground_item":
                for candidate in row.get("candidate_openings", []):
                    later[(row.get("capture"), row.get("run"), candidate)] += 1
            elif kind == "game_left":
                capture = row.get("capture")
                lost_by_capture[capture] = max(lost_by_capture[capture], row.get("lost_records", 0))
            elif kind == "sampling_gap" or (kind == "sampling_window" and row.get("status") != "complete"):
                gaps += 1
    by_container = collections.defaultdict(list)
    for key, opening in opens.items():
        stable = tuple(opening.get(n) for n in ("level", "class_id", "kind", "x", "y"))
        by_container[stable].append((key, opening))
    print(f"Openings: {len(opens)}; containers: {len(by_container)}; lost records: {sum(lost_by_capture.values())}; sampling gaps: {gaps}")
    for stable, runs in sorted(by_container.items(), key=lambda pair: str(pair[0])):
        eligible = [(key, row) for key, row in runs if row.get("snapshot_complete")]
        sets = collections.Counter(signature(direct[key]) for key, _ in eligible)
        bases = collections.Counter(item.get("base_name") or item.get("code") for key, _ in runs for item in direct[key])
        print(f"\nlevel/class/kind/coordinates={stable}; opens={len(runs)}; complete direct captures={len(eligible)}")
        print(f"  directly observed bases: {dict(bases)}")
        print(f"  uncertain later candidate records: {sum(later[key] for key, _ in runs)}")
        for recorded_set, count in sets.most_common(5):
            if not recorded_set:
                print(f"  {count} run(s): no new ground item observed during the call (not proof of no drop)")
            else:
                print(f"  {count} run(s) with matching recorded fields: {recorded_set}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python summarize_drops.py <container-drop-observations.jsonl>")
    summarize(Path(sys.argv[1]))
