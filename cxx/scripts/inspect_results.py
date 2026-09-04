#!/usr/bin/env python3
"""Diagnostics that the summary deliberately leaves out: which transitions
were lost and why, what the strict-but-not-lenient breaks consist of, how
concentrated the aggregates are, and where the evidence is weakest.

    scripts/inspect_results.py [study-dir]

Reads pairs/*.json and headers/pairs/*.json directly, so it can be run on a
partial study while the diff stage is still going.
"""
import glob
import json
import os
import sys
from collections import Counter, defaultdict

BINARY = {
    "field_added_to_struct", "field_removed_from_struct", "field_type_changed",
    "member_offset_changed", "type_size_changed", "base_class_changed",
    "vtable_changed", "function_signature_changed", "symbol_removed",
}
LAYOUT = {
    "field_added_to_struct", "field_removed_from_struct", "field_type_changed",
    "member_offset_changed", "type_size_changed", "base_class_changed", "vtable_changed",
}


def load(path):
    with open(path) as f:
        return json.load(f)["data"]


def is_public_symbol_event(e):
    """Mirrors the comparer's tallies: private ELF version nodes and
    vague-linkage (weak, Itanium-mangled) symbols are not public events."""
    node = (e.get("version") or "").upper()
    if "PRIVATE" in node or "INTERNAL" in node:
        return False
    return not (e.get("weak") and e["symbol"].startswith("_Z"))


def lenient_layout(e):
    if e["kind"] not in LAYOUT:
        return True
    return not (e["exposure"] in ("by_pointer", "not_in_interface") and e["append_only"])


def main(work):
    pairs = {os.path.basename(p)[:-5]: load(p) for p in sorted(glob.glob(f"{work}/pairs/*.json"))}
    heads = {os.path.basename(p)[:-5]: load(p) for p in sorted(glob.glob(f"{work}/headers/pairs/*.json"))}
    plan = load(f"{work}/plan.json")
    planned = {j["source"] + "@" + j["v1"]["upstream"] + ".." + j["v2"]["upstream"] for j in plan["jobs"]}

    print(f"planned {len(planned)}  on disk {len(pairs)}  missing {len(planned - set(pairs))}")
    missing_by_source = Counter(p.split("@")[0] for p in planned - set(pairs))
    if missing_by_source:
        print("  missing by source:", dict(missing_by_source))

    print("\nLOST TRANSITIONS")
    for pid, d in pairs.items():
        err = d.get("error") or ""
        if err or not d["objects"]:
            errs = "; ".join(str(e)[:80] for e in d.get("object_errors", []))[:200]
            print(f"  {pid:<44} {err[:90] or '(no error, 0 objects)'} {errs}")

    # Strict-only breaks: what makes them strict and not lenient?
    print("\nSTRICT-ONLY BINARY BREAKS (strict breaks, lenient clean), by library and reason")
    reasons_by_lib = defaultdict(Counter)
    undeclared_removals_by_lib = Counter()
    strict_only = 0
    for pid, d in pairs.items():
        if not d["objects"] or any(o["mass_rename"] for o in d["objects"]):
            continue
        h = heads.get(pid, {})
        declared = h.get("symbol_declared", {})
        strict_kinds = Counter()
        lenient_kinds = Counter()
        reasons = Counter()
        for o in d["objects"]:
            for e in o["type_events"]:
                if e["third_party"] or e["kind"] not in BINARY:
                    continue
                strict_kinds[e["kind"]] += 1
                if lenient_layout(e):
                    lenient_kinds[e["kind"]] += 1
                else:
                    reasons[f"append-only {e['exposure']} type"] += 1
            for e in o["symbol_events"]:
                if e["kind"] not in BINARY or not is_public_symbol_event(e):
                    continue
                strict_kinds[e["kind"]] += 1
                st = declared.get(e["symbol"], "unknown")
                if st == "undeclared":
                    reasons[f"{e['kind']} of undeclared symbol"] += 1
                    if e["kind"] == "symbol_removed":
                        undeclared_removals_by_lib[d["source"]] += 1
                else:
                    lenient_kinds[e["kind"]] += 1
        if strict_kinds and not lenient_kinds:
            strict_only += 1
            for r, n in reasons.items():
                reasons_by_lib[d["source"]][r] += n
    print(f"  {strict_only} strict-only transitions")
    for lib, rs in sorted(reasons_by_lib.items(), key=lambda kv: -sum(kv[1].values())):
        print(f"  {lib:<24} " + ", ".join(f"{r} x{n}" for r, n in rs.most_common()))

    print("\nUNDECLARED PUBLIC SYMBOL REMOVALS by library (top 15)")
    for lib, n in undeclared_removals_by_lib.most_common(15):
        print(f"  {lib:<24}{n:>6}")

    # Concentration: how much of each aggregate do the top libraries carry?
    print("\nCONCENTRATION of strict binary-breaking transitions")
    breaking_by_lib = Counter()
    trans_by_lib = Counter()
    for pid, d in pairs.items():
        if not d["objects"] or any(o["mass_rename"] for o in d["objects"]):
            continue
        trans_by_lib[d["source"]] += 1
        kinds = Counter()
        for o in d["objects"]:
            for e in o["type_events"]:
                if not e["third_party"] and e["kind"] in BINARY:
                    kinds[e["kind"]] += 1
            for e in o["symbol_events"]:
                if e["kind"] in BINARY and is_public_symbol_event(e):
                    kinds[e["kind"]] += 1
        if kinds:
            breaking_by_lib[d["source"]] += 1
    total = sum(breaking_by_lib.values())
    acc = 0
    for i, (lib, n) in enumerate(breaking_by_lib.most_common(10), 1):
        acc += n
        print(f"  top {i:>2}: {lib:<24}{n:>4} / {trans_by_lib[lib]:<3}  cumulative {100 * acc / total:5.1f}% of {total}")

    n_all = sum(trans_by_lib.values())
    rate = total / n_all if n_all else 0
    worst = max(
        ((lib, abs((total - breaking_by_lib[lib]) / (n_all - trans_by_lib[lib]) - rate)) for lib in trans_by_lib),
        key=lambda kv: kv[1],
    )
    print(f"  leave-one-library-out: strict binary rate {100 * rate:.1f}%, largest shift {100 * worst[1]:.2f} points ({worst[0]})")

    print("\nSONAME CHANGES vs strict binary breaks")
    tab = Counter()
    for pid, d in pairs.items():
        if not d["objects"]:
            continue
        soname_changed = any(o["soname"][0] != o["soname"][1] for o in d["objects"]) or any(d["unpaired"])
        tab[(soname_changed, _breaks(d))] += 1
    for (s, b), n in sorted(tab.items()):
        print(f"  soname_changed={s!s:<5} strict_break={b!s:<5} {n:>5}")

    print("\nEVIDENCE QUALITY")
    dwarf_missing = [pid for pid, d in pairs.items() if d["objects"] and not all(all(o["coverage"]["debug_info_found"]) for o in d["objects"])]
    print(f"  DWARF missing on a side: {len(dwarf_missing)}  {dwarf_missing[:12]}")
    poor = [pid for pid, h in heads.items() if any(c["parsed"] == 0 or c["with_fatal_error"] * 2 > max(c["parsed"], 1) for c in h["coverage"])]
    print(f"  poor header parse coverage: {len(poor)}  by source: {dict(Counter(p.split('@')[0] for p in poor).most_common(12))}")
    unpaired = [(pid, d["unpaired"]) for pid, d in pairs.items() if any(d["unpaired"])]
    print(f"  transitions with unpaired shared objects: {len(unpaired)}")
    for pid, u in unpaired[:15]:
        print(f"    {pid:<44} only-old={u[0][:3]} only-new={u[1][:3]}")

    print("\nLANGUAGE x LEVEL (transitions)")
    lv = Counter()
    for pid, d in pairs.items():
        if d["objects"]:
            langs = {o["language"] for o in d["objects"]}
            lv[("cxx" if "cxx" in langs else "c")] += 1
    print(" ", dict(lv))


def _breaks(d):
    for o in d["objects"]:
        for e in o["type_events"]:
            if not e["third_party"] and e["kind"] in BINARY:
                return True
        for e in o["symbol_events"]:
            if e["kind"] in BINARY and is_public_symbol_event(e):
                return True
    return False


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "study")
