#!/usr/bin/env python3
"""Decide which libraries the study can actually use, from availability data.

For each candidate source package we ask snapshot.debian.org which source
versions produced an amd64 (runtime, -dev, -dbgsym) triple. Only those versions
are usable: without -dbgsym abidiff has no DWARF and degrades to symbol-only
comparison, which cannot see struct fields, enum cases or vtables at all.
"""
import json, os, re, sys, traceback
sys.path.insert(0, "/work/scripts")
import snap
from candidates import CANDIDATES

MAX_SRC_VERSIONS = 45          # newest N source versions to consider per package
SKIP_SUFFIX = ("-dev", "-doc", "-dbg", "-dbgsym", "-utils", "-tools", "-bin",
               "-common", "-data", "-udeb", "-tests", "-examples", "-devtools",
               "-progs", "-client", "-server", "-runtime", "-plugins")

def upstream_of(v):
    """'1:1.3.dfsg+really1.3.2-3' -> '1.3.dfsg+really1.3.2' (drop epoch+revision)."""
    v = v.split(":", 1)[-1]
    return v.rsplit("-", 1)[0] if "-" in v else v

def is_prerelease(v):
    return bool(re.search(r"(~|~rc|~beta|~alpha|~exp|\+really.*~)", v)) and "~" in v

def classify_pkgs(names):
    """Split a source package's binary packages into runtime / dev / dbgsym roles."""
    nameset = set(names)
    runtimes, devs = [], []
    for n in names:
        if n.endswith("-dbgsym"):
            continue
        if n.endswith("-dev"):
            devs.append(n); continue
        if any(n.endswith(s) for s in SKIP_SUFFIX):
            continue
        # NOTE: do not require a "lib" prefix -- that silently dropped zlib1g,
        # libjpeg62-turbo and friends. Packages without shared objects are
        # filtered later, by actually looking for .so files after extraction.
        if (n + "-dbgsym") in nameset:
            runtimes.append(n)
    return sorted(runtimes), sorted(devs)

def main():
    out = {}
    for src, lang in CANDIDATES:
        rec = dict(source=src, lang=lang, versions=[], error=None)
        try:
            allv = snap.source_versions(src)
        except Exception as e:
            rec["error"] = f"source_versions: {e}"
            out[src] = rec; print(f"{src:26} ERROR {e}"); continue

        # newest first, drop pre-releases, keep newest Debian revision per upstream
        seen = {}
        for v in allv:
            if is_prerelease(v):
                continue
            u = upstream_of(v)
            seen.setdefault(u, v)          # snapshot lists newest first
        picked = list(seen.items())[:MAX_SRC_VERSIONS]

        for upstream, srcver in picked:
            try:
                bps = snap.binpackages(src, srcver)
            except Exception as e:
                continue
            byname = {}
            for n, bv in bps:
                byname.setdefault(n, []).append(bv)
            runtimes, devs = classify_pkgs(list(byname))
            if not runtimes or not devs:
                continue
            rec["versions"].append(dict(upstream=upstream, srcver=srcver,
                                        runtimes=runtimes, devs=devs,
                                        binvers={k: v for k, v in byname.items()}))
        out[src] = rec
        print(f"{src:26} lang={lang:3} usable_versions={len(rec['versions']):3} "
              f"runtimes={rec['versions'][0]['runtimes'][:3] if rec['versions'] else []}")
    with open("/work/results/discovery.json", "w") as f:
        json.dump(out, f, indent=1)
    print("\nwrote /work/results/discovery.json")

main()
