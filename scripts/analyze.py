#!/usr/bin/env python3
"""Aggregate the study into the tables the research question actually asks for."""
import json, os, glob, sys
from collections import Counter, defaultdict
sys.path.insert(0, "/work/scripts")
from resilience import ABSORBED, NOT_ABSORBED, NEUTRAL, verdict, mechanism

OUT = "/work/results"

# Kinds that break an existing compiled client (C/C++ linkage sense).
BREAKING = {
    "field_added_to_struct", "field_removed_from_struct", "field_type_changed",
    "member_offset_changed", "type_size_changed", "base_class_changed",
    "vtable_changed", "function_signature_changed", "symbol_removed",
}
# Changes that need resilience to survive recompilation-free evolution but are
# not C-ABI breaks: an added enum case does not move any byte, yet it breaks a
# client that switched exhaustively.
RESILIENCE_RELEVANT = BREAKING | {"enum_case_added", "enum_case_removed"}

ADDITIVE_ONLY = {"symbol_added", "symbol_version_renamed"}


def load_pairs():
    pairs = []
    for f in sorted(glob.glob("/data/pairout/*.json")):
        pairs.append(json.load(open(f)))
    return pairs


def pair_rollup(p):
    """Collapse a pair's per-shared-object results into one record."""
    agg = Counter()
    soname_changed = False
    any_timeout = False
    renamed = 0
    for l in p["libs"]:
        for k, v in l["headline"].items():
            agg[k] += v
        renamed += l["counts"].get("symbol_version_renamed", 0)
        if l["soname1"] != l["soname2"]:
            soname_changed = True
        any_timeout |= bool(l.get("timeout"))
    return dict(
        source=p["source"], lang=p["lang"], arm=p["arm"], pair=p["pair"],
        up1=p["up1"], up2=p["up2"], nlibs=len(p["libs"]),
        counts=dict(agg), soname_changed=soname_changed,
        timeout=any_timeout, renamed=renamed, error=p.get("error"),
    )


def main():
    pairs = [pair_rollup(p) for p in load_pairs()]
    ok = [p for p in pairs if not p["error"] and p["nlibs"] > 0]
    errs = [p for p in pairs if p["error"] or p["nlibs"] == 0]
    cons = [p for p in ok if p["arm"] == "consecutive"]
    ctrl = [p for p in ok if p["arm"] == "control"]

    print("="*100)
    print("CORPUS")
    print("="*100)
    print(f"pairs attempted            : {len(pairs)}")
    print(f"pairs analysed             : {len(ok)}  "
          f"(consecutive={len(cons)}, control={len(ctrl)})")
    print(f"pairs failed / no shared obj: {len(errs)}")
    print(f"libraries                  : {len(set(p['source'] for p in cons))} "
          f"(C={len(set(p['source'] for p in cons if p['lang']=='c'))}, "
          f"C++={len(set(p['source'] for p in cons if p['lang']=='cxx'))})")
    print(f"shared objects compared    : {sum(p['nlibs'] for p in cons)}")

    # ---------------------------------------------------------------- control
    print()
    print("="*100)
    print("NOISE FLOOR  (same upstream version, different Debian revision:")
    print("              any change here is packaging/compiler noise, not library evolution)")
    print("="*100)
    noisy = [p for p in ctrl if any(k in RESILIENCE_RELEVANT for k in p["counts"])]
    print(f"control pairs               : {len(ctrl)}")
    print(f"  with any reported change  : {sum(1 for p in ctrl if p['counts'])}")
    print(f"  with a resilience-relevant change : {len(noisy)}")
    if noisy:
        for p in noisy[:12]:
            print(f"    {p['source']:<14} {p['up1']:<22} {p['counts']}")
    nf = Counter()
    for p in ctrl:
        for k in p["counts"]:
            nf[k] += 1
    print(f"  kinds seen in control     : {dict(nf)}")

    # ------------------------------------------------------- prevalence table
    def table(ps, title):
        n = len(ps)
        if not n:
            return
        prev, vol = Counter(), Counter()
        for p in ps:
            for k, v in p["counts"].items():
                prev[k] += 1
                vol[k] += v
        print()
        print("="*100)
        print(f"{title}   (n = {n} release transitions)")
        print("="*100)
        print(f"{'change kind':<30}{'% transitions':>14}{'transitions':>13}"
              f"{'total events':>14}  {'resilient boundary':<22}")
        print("-"*100)
        for k, c in sorted(prev.items(), key=lambda x: -x[1]):
            v = verdict(k)
            mark = {"absorbed": "ABSORBS", "not_absorbed": "cannot help",
                    "neutral": "n/a"}[v]
            print(f"{k:<30}{100*c/n:>13.1f}%{c:>13}{vol[k]:>14}  {mark:<12}{mechanism(k)[:28]}")
        return prev, vol

    table(cons, "PREVALENCE OF ABI CHANGE KINDS ACROSS CONSECUTIVE RELEASES (all)")
    table([p for p in cons if p["lang"] == "c"], "C LIBRARIES ONLY")
    table([p for p in cons if p["lang"] == "cxx"], "C++ LIBRARIES ONLY")

    # ------------------------------------------------- soname / break framing
    print()
    print("="*100)
    print("DECLARED vs SILENT BREAKS")
    print("="*100)
    def brk(p):
        return {k for k in p["counts"] if k in BREAKING}
    broke = [p for p in cons if brk(p)]
    print(f"transitions with a client-breaking change : {len(broke)}/{len(cons)}"
          f" ({100*len(broke)/max(len(cons),1):.1f}%)")
    declared = [p for p in broke if p["soname_changed"]]
    silent = [p for p in broke if not p["soname_changed"]]
    print(f"  of which SONAME was bumped (declared)   : {len(declared)}")
    print(f"  of which SONAME unchanged (silent)      : {len(silent)}")

    # ------------------------------------------------- the actionable question
    print()
    print("="*100)
    print("WOULD A RESILIENT BOUNDARY HAVE SAVED THE RELEASE?")
    print("="*100)
    fully, partly = [], []
    for p in broke:
        b = brk(p)
        if b <= set(ABSORBED):
            fully.append(p)
        else:
            partly.append(p)
    print(f"breaking transitions                       : {len(broke)}")
    print(f"  every breaking change absorbable         : {len(fully)} "
          f"({100*len(fully)/max(len(broke),1):.1f}%)  -> release becomes source+binary compatible")
    print(f"  contained an unabsorbable change         : {len(partly)} "
          f"({100*len(partly)/max(len(broke),1):.1f}%)  -> resilience alone insufficient")

    # marginal value: which mechanism is load-bearing for the rescued releases?
    print()
    print("MARGINAL VALUE OF EACH RESILIENCE MECHANISM")
    print("(how many fully-rescued transitions would stop being rescued if this")
    print(" mechanism alone were dropped -- i.e. what the budget actually buys)")
    print("-"*100)
    mechs = sorted(set(ABSORBED.values()))
    for m in mechs:
        kinds = {k for k, mm in ABSORBED.items() if mm == m}
        need = sum(1 for p in fully if brk(p) & kinds)
        only = sum(1 for p in fully if brk(p) & kinds and not (brk(p) - kinds) <= set())
        crit = sum(1 for p in fully if brk(p) & kinds)
        print(f"  {m:<36} load-bearing for {crit:>4} / {len(fully)} rescued transitions "
              f"({100*crit/max(len(fully),1):.1f}%)")

    json.dump(dict(pairs=pairs), open(os.path.join(OUT, "pairs_rollup.json"), "w"))
    print("\nwrote results/pairs_rollup.json")


main()
