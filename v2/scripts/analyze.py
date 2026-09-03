#!/usr/bin/env python3
"""Aggregate the study into the tables the research question asks for."""
import argparse, glob, json, os, sys
from collections import Counter, defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from resilience import ABSORBED, NOT_ABSORBED, verdict, mechanism

BREAKING = {
    "field_added_to_struct", "field_removed_from_struct", "field_type_changed",
    "member_offset_changed", "type_size_changed", "base_class_changed",
    "vtable_changed", "function_signature_changed", "symbol_removed",
}
# Layout-neutral but still breaks a client that switched exhaustively.
RESILIENCE_RELEVANT = BREAKING | {"enum_case_added", "enum_case_removed"}
MASS_RENAME_MIN = 50


def rollup(p):
    agg, third, priv = Counter(), Counter(), Counter()
    langs, soname_changed, timeout, renamed = Counter(), False, False, 0
    for l in p["libs"]:
        for k, v in l.get("headline", {}).items():
            agg[k] += v
        for k, v in l.get("headline_thirdparty", {}).items():
            third[k] += v
        for k, v in l.get("headline_private", {}).items():
            priv[k] += v
        renamed += l.get("counts", {}).get("symbol_version_renamed", 0)
        langs[l.get("lang", "unknown")] += 1
        soname_changed |= l["soname1"] != l["soname2"]
        timeout |= bool(l.get("timeout"))
    lang = "cxx" if langs.get("cxx") else ("c" if langs.get("c") else "unknown")
    total_sym = agg.get("symbol_removed", 0) + agg.get("symbol_added", 0)
    return dict(source=p["source"], lang=lang, pair=p["pair"], up1=p["up1"], up2=p["up2"],
                nlibs=len(p["libs"]), counts=dict(agg), thirdparty=dict(third),
                private=dict(priv), soname_changed=soname_changed, timeout=timeout,
                renamed=renamed,
                mass_rename=renamed >= MASS_RENAME_MIN and renamed >= total_sym,
                error=p.get("error"))


def table(ps, title, out):
    n = len(ps)
    if not n:
        return None
    prev, vol = Counter(), Counter()
    for p in ps:
        for k, v in p["counts"].items():
            prev[k] += 1; vol[k] += v
    print(f"\n{'='*104}\n{title}   (n = {n} release transitions)\n{'='*104}")
    print(f"{'change kind':<30}{'% transitions':>14}{'transitions':>12}{'events':>10}  "
          f"{'resilient boundary':<14}mechanism")
    print("-"*104)
    for k, c in sorted(prev.items(), key=lambda x: -x[1]):
        mark = {"absorbed": "ABSORBS", "not_absorbed": "cannot help", "neutral": "n/a"}[verdict(k)]
        print(f"{k:<30}{100*c/n:>13.1f}%{c:>12}{vol[k]:>10}  {mark:<14}{mechanism(k)[:34]}")
    out[title] = dict(n=n, prevalence=dict(prev), volume=dict(vol))
    return prev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairout", default="/work/results/pairout")
    ap.add_argument("--out", default="/work/results/summary.json")
    a = ap.parse_args()

    raw = [json.load(open(f)) for f in sorted(glob.glob(os.path.join(a.pairout, "*.json")))]
    pairs = [rollup(p) for p in raw]
    ok = [p for p in pairs if not p["error"] and p["nlibs"] > 0]
    errs = [p for p in pairs if p["error"] or p["nlibs"] == 0]
    mass = [p for p in ok if p["mass_rename"]]
    good = [p for p in ok if not p["mass_rename"]]
    out = {}

    print("="*104); print("CORPUS"); print("="*104)
    print(f"pairs attempted              : {len(pairs)}")
    print(f"pairs analysed               : {len(ok)}")
    print(f"pairs failed / no shared obj : {len(errs)}")
    print(f"source packages              : {len(set(p['source'] for p in ok))}")
    print(f"shared objects compared      : {sum(p['nlibs'] for p in ok)}")
    print(f"  language (auto-detected from mangled-symbol fraction):")
    print(f"    C   transitions          : {sum(1 for p in good if p['lang']=='c')}")
    print(f"    C++ transitions          : {sum(1 for p in good if p['lang']=='cxx')}")
    print(f"excluded, mass symbol rename : {len(mass)} "
          f"({sorted(set(p['source'] for p in mass))})")

    tp, pv = Counter(), Counter()
    for p in ok:
        for k, v in p["thirdparty"].items(): tp[k] += v
        for k, v in p["private"].items(): pv[k] += v
    print("\nFILTERED OUT (would otherwise be attributed to these libraries):")
    print(f"  changes in third-party headers (glibc/libstdc++/...): {dict(tp)}")
    print(f"  changes on private ELF version-node symbols          : {dict(pv)}")

    table(good, "PREVALENCE OF ABI CHANGE KINDS ACROSS CONSECUTIVE RELEASES", out)
    table([p for p in good if p["lang"] == "c"], "C LIBRARIES ONLY", out)
    table([p for p in good if p["lang"] == "cxx"], "C++ LIBRARIES ONLY", out)

    def brk(p):
        return {k for k in p["counts"] if k in BREAKING}
    broke = [p for p in good if brk(p)]
    print(f"\n{'='*104}\nDECLARED vs SILENT BREAKS\n{'='*104}")
    print(f"transitions with a client-breaking change : {len(broke)}/{len(good)}"
          f" ({100*len(broke)/max(len(good),1):.1f}%)")
    print(f"  SONAME bumped  (break declared)         : {sum(1 for p in broke if p['soname_changed'])}")
    print(f"  SONAME unchanged (silent break)         : {sum(1 for p in broke if not p['soname_changed'])}")

    fully = [p for p in broke if brk(p) <= set(ABSORBED)]
    partly = [p for p in broke if not brk(p) <= set(ABSORBED)]
    print(f"\n{'='*104}\nWOULD A RESILIENT BOUNDARY HAVE SAVED THE RELEASE?\n{'='*104}")
    print(f"breaking transitions                      : {len(broke)}")
    print(f"  all breaking changes absorbable         : {len(fully)} "
          f"({100*len(fully)/max(len(broke),1):.1f}%)")
    print(f"  contained an unabsorbable change        : {len(partly)} "
          f"({100*len(partly)/max(len(broke),1):.1f}%)")
    print("\nLOAD-BEARING MECHANISMS (of the fully-rescued transitions, how many")
    print("needed each mechanism -- i.e. what the runtime budget actually buys)")
    print("-"*104)
    for m in sorted(set(ABSORBED.values())):
        kinds = {k for k, mm in ABSORBED.items() if mm == m}
        c = sum(1 for p in fully if brk(p) & kinds)
        print(f"  {m:<40} {c:>4} / {len(fully)} rescued transitions "
              f"({100*c/max(len(fully),1):.1f}%)")
    out["breaks"] = dict(breaking=len(broke), analysed=len(good),
                         declared=sum(1 for p in broke if p["soname_changed"]),
                         silent=sum(1 for p in broke if not p["soname_changed"]),
                         fully_absorbable=len(fully), partly=len(partly))
    out["pairs"] = pairs
    json.dump(out, open(a.out, "w"))
    print(f"\nwrote {a.out}")


main()
