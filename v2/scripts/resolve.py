#!/usr/bin/env python3
"""Turn selected source packages into consecutive-release pair jobs.

The runtime/dev/dbgsym split is decided by a GENERIC rule -- a binary package is
a runtime shared-library package iff a `<name>-dbgsym` sibling exists in the same
source version. v1 used a hand-written regex per library; that does not scale and
was the main source of per-project debugging.
"""
import argparse, json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snap

SKIP_RUNTIME = ("-dev", "-doc", "-dbg", "-dbgsym", "-udeb", "-tests", "-examples")
MAX_RUNTIME = 4          # backstop against mega-sources
MAX_DEV = 4


def norm_pkg(n):
    """Version-blind package identity: libicu72 ~ libicu73, libssl3 ~ libssl4."""
    return re.sub(r"\d+", "#", n)


def upstream_of(v):
    v = v.split(":", 1)[-1]
    return v.rsplit("-", 1)[0] if "-" in v else v


def is_prerelease(v):
    return "~" in v


def roles(names, want_binary):
    """Runtime packages for the specific popular library we selected.

    The earlier rule -- "any package with a -dbgsym sibling" -- is generic but
    far too broad for mega-sources: gcc-16 matches 166 packages (every
    cross-compiler), which alone pushed extraction past 23 GB. Anchoring on the
    binary package popcon actually ranked keeps the unit of study equal to the
    library people install, and stays version-blind so libicu72 -> libicu73 and
    libssl3 -> libssl4 still line up.
    """
    dbg = {n for n in names if n.endswith("-dbgsym")}
    cands = [n for n in sorted(names)
             if not any(n.endswith(s) for s in SKIP_RUNTIME) and f"{n}-dbgsym" in dbg]
    target = norm_pkg(want_binary)
    runtime = [n for n in cands if norm_pkg(n) == target]
    if not runtime:                                  # renamed beyond digits (t64 etc.)
        stem = re.sub(r"[0-9]+$", "", want_binary).rstrip("-")
        runtime = [n for n in cands if n.startswith(stem)][:MAX_RUNTIME]
    runtime = runtime[:MAX_RUNTIME]
    devs = sorted(n for n in names if n.endswith("-dev"))
    pref = [d for d in devs if norm_pkg(d).startswith(target.rstrip("#"))]
    dev = (pref or devs)[:MAX_DEV]
    return runtime, dev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selected", default="/work/results/selected.json")
    ap.add_argument("--releases", type=int, default=10)
    ap.add_argument("--max-scan", type=int, default=40)
    ap.add_argument("--out", default="/work/results/plan.json")
    a = ap.parse_args()

    sel = json.load(open(a.selected))
    print(f"{len(sel)} selected sources", flush=True)
    jobs, report = [], []
    for rec in sel:
        src, want_binary = rec["source"], rec["binary"]
        try:
            allv = snap.source_versions(src)
        except Exception as e:
            report.append((src, 0, f"ERR {e}")); continue
        seen = {}
        for v in allv:
            if is_prerelease(v):
                continue
            seen.setdefault(upstream_of(v), v)     # snapshot lists newest first
        picked = []
        for up, sv in list(seen.items())[:a.max_scan]:
            if len(picked) >= a.releases:
                break
            try:
                bps = snap.binpackages(src, sv)
            except Exception:
                continue
            byname = {}
            for n, bv in bps:
                byname.setdefault(n, []).append(bv)
            rt, dev = roles(set(byname), want_binary)
            if not rt or not dev:
                continue
            keep = set(rt) | set(dev) | {r + "-dbgsym" for r in rt}
            picked.append(dict(upstream=up, srcver=sv, runtimes=rt, devs=dev[:8],
                               binvers={k: v for k, v in byname.items() if k in keep}))
        picked.reverse()                            # oldest -> newest
        for x, y in zip(picked, picked[1:]):
            jobs.append(dict(src=src, v1=x, v2=y, arm="consecutive"))
        report.append((src, len(picked), rt[:3] if picked else []))
        print(f"{src:<24} releases={len(picked):<4} {report[-1][2]}", flush=True)

    json.dump(dict(jobs=jobs), open(a.out, "w"))
    print(f"\n{len(jobs)} consecutive-release pairs across "
          f"{len(set(j['src'] for j in jobs))} sources -> {a.out}")


main()
