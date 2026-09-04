#!/usr/bin/env python3
"""Compares two study summaries transition by transition.

    scripts/compare_runs.py OLD_summary.json NEW_summary.json

OLD is a schema-1 summary (one `counts` tally per transition); NEW is a
schema-2 summary (`strict` and `lenient` tallies). Only transitions present
in both are compared, so the numbers isolate the effect of the corrected
definitions from the effect of a different corpus or a different run.
"""
import json
import sys
from collections import Counter, defaultdict

BINARY = {
    "field_added_to_struct", "field_removed_from_struct", "field_type_changed",
    "member_offset_changed", "type_size_changed", "base_class_changed",
    "vtable_changed", "function_signature_changed", "symbol_removed",
}
ABSORBED_OLD = {  # the mechanism map before REVIEW.md §1.2
    "field_added_to_struct", "field_removed_from_struct", "field_type_changed",
    "member_offset_changed", "type_size_changed", "base_class_changed",
    "enum_case_added", "enum_case_removed", "vtable_changed",
    "inline_body_changed", "macro_value_changed",
}
ABSORBED_NEW = ABSORBED_OLD - {"field_removed_from_struct", "field_type_changed", "enum_case_removed"}


def load(path):
    with open(path) as f:
        doc = json.load(f)
    return doc.get("data", doc)


def by_id(summary, key):
    out = {}
    for t in summary["transitions"]:
        if t.get("mass_rename"):
            continue
        out[t["id"]] = t[key]
    return out


def breaks(counts):
    return any(k in BINARY and n > 0 for k, n in counts.items())


def rescued(counts, absorbed):
    kinds = {k for k, n in counts.items() if k in BINARY and n > 0}
    return bool(kinds) and kinds <= absorbed


def main(old_path, new_path):
    old = load(old_path)
    new = load(new_path)
    old_t = by_id(old, "counts")
    new_s = by_id(new, "strict")
    new_l = by_id(new, "lenient")
    common = sorted(set(old_t) & set(new_s))
    print(f"transitions: old {len(old_t)}, new {len(new_s)}, common {len(common)}")
    if not common:
        return

    kinds = sorted({k for c in old_t.values() for k in c} | {k for c in new_s.values() for k in c})
    print(f"\n{'kind':<28}{'old':>7}{'new strict':>12}{'new lenient':>13}   (transitions with >=1 event, common set)")
    for k in kinds:
        o = sum(1 for i in common if old_t[i].get(k, 0) > 0)
        s = sum(1 for i in common if new_s[i].get(k, 0) > 0)
        l = sum(1 for i in common if new_l[i].get(k, 0) > 0)
        print(f"{k:<28}{o:>7}{s:>12}{l:>13}")

    ob = sum(breaks(old_t[i]) for i in common)
    sb = sum(breaks(new_s[i]) for i in common)
    lb = sum(breaks(new_l[i]) for i in common)
    n = len(common)
    print(f"\nbinary-breaking transitions (common set, n={n}):")
    print(f"  old            {ob:>5}  {100*ob/n:5.1f}%")
    print(f"  new strict     {sb:>5}  {100*sb/n:5.1f}%")
    print(f"  new lenient    {lb:>5}  {100*lb/n:5.1f}%")

    orc = sum(rescued(old_t[i], ABSORBED_OLD) for i in common)
    src = sum(rescued(new_s[i], ABSORBED_NEW) for i in common)
    lrc = sum(rescued(new_l[i], ABSORBED_NEW) for i in common)
    print("\nfully absorbable among breaking (binary framing):")
    print(f"  old map, old counts        {orc:>5} / {ob}")
    print(f"  new map, new strict        {src:>5} / {sb}")
    print(f"  new map, new lenient       {lrc:>5} / {lb}")

    flips = defaultdict(list)
    for i in common:
        a, b = breaks(old_t[i]), breaks(new_s[i])
        if a != b:
            gained = {k for k in new_s[i] if k in BINARY and old_t[i].get(k, 0) == 0}
            lost = {k for k in old_t[i] if k in BINARY and new_s[i].get(k, 0) == 0}
            flips["old!=strict"].append((i, sorted(gained), sorted(lost)))
        if b != breaks(new_l[i]):
            flips["strict!=lenient"].append((i, [], []))
    for name, items in flips.items():
        print(f"\n{name}: {len(items)} transitions")
        reasons = Counter()
        for i, gained, lost in items[:2000]:
            for k in gained:
                reasons[f"+{k}"] += 1
            for k in lost:
                reasons[f"-{k}"] += 1
        for r, c in reasons.most_common(12):
            print(f"  {r:<32}{c:>5}")
        for i, gained, lost in items[:10]:
            print(f"    {i:<44} +{gained} -{lost}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
