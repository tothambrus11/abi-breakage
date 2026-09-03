#!/usr/bin/env python3
"""Final aggregation across both corpora, reporting absolute frequencies as well
as prevalence."""
import argparse, glob, json, os, statistics, sys
from collections import Counter, defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from resilience import ABSORBED, verdict, mechanism

# Two distinct questions, which must not be conflated.
#
# BINARY_BREAKING: breaks an already-compiled client at load or call time. This
# is what abidiff means by an ABI break.
BINARY_BREAKING = {
    "field_added_to_struct", "field_removed_from_struct", "field_type_changed",
    "member_offset_changed", "type_size_changed", "base_class_changed",
    "vtable_changed", "function_signature_changed", "symbol_removed",
}
# EVOLUTION_RELEVANT: everything a boundary must tolerate for a library to
# evolve without forcing clients to recompile. Adding an enum case moves no
# byte, so it is not a binary break -- but it breaks any client that switched
# exhaustively, and it is precisely what a non-frozen enum exists for. Scoring
# only BINARY_BREAKING credits the enum mechanism with zero by construction.
EVOLUTION_RELEVANT = BINARY_BREAKING | {"enum_case_added", "enum_case_removed"}
BREAKING = BINARY_BREAKING
MASS_RENAME_MIN = 50

ORDER = ["field_added_to_struct", "field_removed_from_struct", "field_type_changed",
         "member_offset_changed", "type_size_changed", "base_class_changed",
         "enum_case_added", "enum_case_removed", "vtable_changed",
         "function_signature_changed", "symbol_removed", "symbol_added"]


def rollup(p):
    agg, third, priv = Counter(), Counter(), Counter()
    langs, soname, renamed = Counter(), False, 0
    for l in p["libs"]:
        for k, v in l.get("headline", {}).items():
            agg[k] += v
        for k, v in l.get("headline_thirdparty", {}).items():
            third[k] += v
        for k, v in l.get("headline_private", {}).items():
            priv[k] += v
        renamed += l.get("counts", {}).get("symbol_version_renamed", 0)
        langs[l.get("lang", "unknown")] += 1
        soname |= l["soname1"] != l["soname2"]
    lang = "cxx" if langs.get("cxx") else ("c" if langs.get("c") else "unknown")
    tot = agg.get("symbol_removed", 0) + agg.get("symbol_added", 0)
    return dict(source=p["source"], lang=lang, pair=p["pair"], up1=p["up1"], up2=p["up2"],
                nlibs=len(p["libs"]), counts=dict(agg), thirdparty=dict(third),
                private=dict(priv), soname_changed=soname, renamed=renamed,
                mass_rename=renamed >= MASS_RENAME_MIN and renamed >= tot,
                error=p.get("error"))


def load(dirs):
    by_pair = {}
    for d in dirs:
        for f in sorted(glob.glob(os.path.join(d, "*.json"))):
            p = json.load(open(f))
            if p.get("error") or not p.get("libs"):
                continue
            by_pair[p["pair"]] = rollup(p)
    return list(by_pair.values())


def freq_table(ps, title):
    n = len(ps)
    if not n:
        return
    prev, vol, per = Counter(), Counter(), defaultdict(list)
    for p in ps:
        for k, v in p["counts"].items():
            prev[k] += 1; vol[k] += v; per[k].append(v)
    print(f"\n{'='*118}\n{title}\n  n = {n} release transitions"
          f"   ({len(set(x['source'] for x in ps))} libraries)\n{'='*118}")
    print(f"{'change kind':<28}{'transitions':>12}{'% of n':>9}{'TOTAL events':>14}"
          f"{'median':>8}{'max':>7}   {'resilience':<12}mechanism")
    print("-"*118)
    keys = [k for k in ORDER if k in prev] + [k for k in prev if k not in ORDER]
    for k in keys:
        v = per[k]
        mark = {"absorbed": "ABSORBS", "not_absorbed": "cannot help", "neutral": "n/a"}[verdict(k)]
        print(f"{k:<28}{prev[k]:>12}{100*prev[k]/n:>8.1f}%{vol[k]:>14}"
              f"{statistics.median(v):>8.0f}{max(v):>7}   {mark:<12}{mechanism(k)[:32]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dirs", nargs="+", required=True)
    ap.add_argument("--out", default="v2/results/final_summary.json")
    a = ap.parse_args()

    pairs = load(a.dirs)
    mass = [p for p in pairs if p["mass_rename"]]
    good = [p for p in pairs if not p["mass_rename"]]
    C = [p for p in good if p["lang"] == "c"]
    X = [p for p in good if p["lang"] == "cxx"]

    print("="*118); print("CORPUS"); print("="*118)
    print(f"distinct release transitions analysed : {len(pairs)}")
    print(f"  libraries (Debian source packages)  : {len(set(p['source'] for p in pairs))}")
    print(f"  shared objects compared             : {sum(p['nlibs'] for p in pairs)}")
    print(f"  C transitions                       : {len(C)}  "
          f"({len(set(p['source'] for p in C))} libraries)")
    print(f"  C++ transitions                     : {len(X)}  "
          f"({len(set(p['source'] for p in X))} libraries)")
    print(f"  excluded (mass symbol rename policy) : {len(mass)} "
          f"{sorted(set(p['source'] for p in mass))}")

    tp, pv = Counter(), Counter()
    for p in pairs:
        for k, v in p["thirdparty"].items(): tp[k] += v
        for k, v in p["private"].items(): pv[k] += v
    print("\nEVENTS EXCLUDED BY THE TWO CORRECTNESS FILTERS")
    print(f"  declared in third-party headers (glibc/libstdc++/...) : "
          f"{sum(tp.values())} events {dict(tp)}")
    print(f"  on private ELF version-node symbols                   : "
          f"{sum(pv.values())} events {dict(pv)}")

    freq_table(good, "FREQUENCY OF ABI CHANGE KINDS ACROSS CONSECUTIVE RELEASES  (all libraries)")
    freq_table(C, "C LIBRARIES ONLY")
    freq_table(X, "C++ LIBRARIES ONLY")

    def brk(p):
        return {k for k in p["counts"] if k in BREAKING}
    broke = [p for p in good if brk(p)]
    declared = [p for p in broke if p["soname_changed"]]
    silent = [p for p in broke if not p["soname_changed"]]
    print(f"\n{'='*118}\nDECLARED vs SILENT BREAKS\n{'='*118}")
    print(f"transitions analysed                        : {len(good)}")
    print(f"transitions with a client-breaking change   : {len(broke)} "
          f"({100*len(broke)/max(len(good),1):.1f}%)")
    print(f"  SONAME bumped   (break declared)          : {len(declared)}")
    print(f"  SONAME unchanged (SILENT break)           : {len(silent)} "
          f"({100*len(silent)/max(len(broke),1):.1f}% of breaks)")

    fully = [p for p in broke if brk(p) <= set(ABSORBED)]
    partly = [p for p in broke if not brk(p) <= set(ABSORBED)]
    print(f"\n{'='*118}\nWOULD A RESILIENT BOUNDARY HAVE SAVED THE RELEASE?\n{'='*118}")
    print(f"breaking transitions                        : {len(broke)}")
    print(f"  every breaking change absorbable          : {len(fully)} "
          f"({100*len(fully)/max(len(broke),1):.1f}%)")
    print(f"  contained an unabsorbable change          : {len(partly)} "
          f"({100*len(partly)/max(len(broke),1):.1f}%)")
    print("\nLOAD-BEARING MECHANISMS  (of the fully-rescued transitions, how many needed each)")
    print("-"*118)
    for m in sorted(set(ABSORBED.values())):
        kinds = {k for k, mm in ABSORBED.items() if mm == m}
        c = sum(1 for p in fully if brk(p) & kinds)
        solo = sum(1 for p in fully if brk(p) and brk(p) <= kinds)
        print(f"  {m:<38} needed by {c:>4} / {len(fully)}   "
              f"(sole reason for {solo})")

    # per-library churn ranking, absolute counts
    print(f"\n{'='*118}\nMOST ABI-UNSTABLE LIBRARIES  (breaking transitions / transitions)\n{'='*118}")
    bysrc = defaultdict(lambda: [0, 0, Counter()])
    for p in good:
        bysrc[p["source"]][1] += 1
        if brk(p):
            bysrc[p["source"]][0] += 1
            for k in brk(p):
                bysrc[p["source"]][2][k] += 1
    rows = sorted(bysrc.items(), key=lambda x: (-x[1][0], -x[1][1]))[:22]
    print(f"{'library':<26}{'breaking':>9}{'of':>5}   kinds")
    for s, (b, t, kinds) in rows:
        if not b:
            continue
        print(f"{s:<26}{b:>9}{t:>5}   {', '.join(f'{k}×{v}' for k, v in kinds.most_common(5))}")

    # ---- second framing: recompilation-free evolution, not just linkage
    def ev(p):
        return {k for k in p["counts"] if k in EVOLUTION_RELEVANT}
    ev_pairs = [p for p in good if ev(p)]
    ev_fully = [p for p in ev_pairs if ev(p) <= set(ABSORBED)]
    print(f"\n{'='*118}")
    print("SECOND FRAMING: RECOMPILATION-FREE EVOLUTION")
    print("(adds enum-case changes, which move no byte so are not binary breaks,")
    print(" but break exhaustive switches -- the reason non-frozen enums exist)")
    print("="*118)
    print(f"transitions with an evolution-relevant change : {len(ev_pairs)} "
          f"({100*len(ev_pairs)/max(len(good),1):.1f}% of {len(good)})")
    print(f"  fully absorbable by a resilient boundary    : {len(ev_fully)} "
          f"({100*len(ev_fully)/max(len(ev_pairs),1):.1f}%)")
    print("\nLOAD-BEARING MECHANISMS under this framing")
    print("-"*118)
    for m in sorted(set(ABSORBED.values())):
        kinds = {k for k, mm in ABSORBED.items() if mm == m}
        c = sum(1 for p in ev_fully if ev(p) & kinds)
        solo = sum(1 for p in ev_fully if ev(p) and ev(p) <= kinds)
        print(f"  {m:<38} needed by {c:>4} / {len(ev_fully)}   "
              f"(sole reason for {solo})")

    # ---- header-visible churn: the category no ABI tool can see
    hp = "v2/results/headers.json"
    if os.path.exists(hp):
        H = [h for h in json.load(open(hp)) if h.get("hdr")]
        n = len(H)
        body = [h for h in H if h["hdr"]["inline_body_changed"] > 0]
        macro = [h for h in H if h["hdr"]["macro_value_changed"] > 0]
        tot_body = sum(h["hdr"]["inline_body_changed"] for h in H)
        tot_macro = sum(h["hdr"]["macro_value_changed"] for h in H)
        withdefs = [h for h in H if h["hdr"]["defs_common"] > 0]
        print(f"\n{'='*118}")
        print("INLINE / TEMPLATE BODY AND MACRO CHURN")
        print("(invisible to abidiff AND to abi-compliance-checker by construction;")
        print(" measured from the -dev headers, validated on the same 30-case ground truth)")
        print("="*118)
        print(f"{'measure':<58}{'transitions':>12}{'% of n':>9}{'TOTAL events':>14}")
        print("-"*118)
        print(f"{'transitions analysed':<58}{n:>12}")
        print(f"{'  ...that ship any inlinable definition in public headers':<58}"
              f"{len(withdefs):>12}{100*len(withdefs)/max(n,1):>8.1f}%")
        print(f"{'transitions changing >=1 inline/template body':<58}"
              f"{len(body):>12}{100*len(body)/max(n,1):>8.1f}%{tot_body:>14}")
        print(f"{'transitions changing >=1 public macro value':<58}"
              f"{len(macro):>12}{100*len(macro)/max(n,1):>8.1f}%{tot_macro:>14}")
        med = statistics.median([h["hdr"]["inline_body_changed"] for h in body]) if body else 0
        mx = max([h["hdr"]["inline_body_changed"] for h in body]) if body else 0
        print(f"{'  bodies changed per affected transition (median / max)':<58}"
              f"{med:>12.0f}{mx:>17}")
        top = sorted(H, key=lambda h: -h["hdr"]["inline_body_changed"])[:10]
        print("\nlibraries with the most changed client-visible bodies in one release:")
        for h in top:
            if h["hdr"]["inline_body_changed"]:
                print(f"  {h['source']:<22} {h['up1']} -> {h['up2']:<18} "
                      f"{h['hdr']['inline_body_changed']:>5} bodies "
                      f"(of {h['hdr']['defs_common']} shared)")

    json.dump(dict(pairs=pairs), open(a.out, "w"))
    print(f"\nwrote {a.out}")


main()
