#!/usr/bin/env python3
"""Turn discovery + config into an explicit list of pair jobs."""
import json, re, sys
sys.path.insert(0, "/work/scripts")
import snap
from study_config import LIBS, N_RELEASES, CONTROL_LIBS, CONTROL_PAIRS_PER_LIB
from discover import upstream_of, is_prerelease

def _roles(src, srcver, rt_re):
    """Binary-package roles for one exact source version (control arm)."""
    byname = {}
    for n, bv in snap.binpackages(src, srcver):
        byname.setdefault(n, []).append(bv)
    rts = sorted(n for n in byname if re.search(rt_re, n)
                 and (n + "-dbgsym") in byname)
    devs = sorted(n for n in byname if n.endswith("-dev"))[:8]
    keep = set(rts) | set(devs) | {r + "-dbgsym" for r in rts}
    return dict(runtimes=rts, devs=devs,
                binvers={k: v for k, v in byname.items() if k in keep})


disc = json.load(open("/work/results/discovery.json"))
jobs, report = [], []

def shrink(vrec, rt_re):
    rts = [r for r in vrec["runtimes"] if re.search(rt_re, r)]
    if not rts:
        return None
    devs = vrec["devs"][:8]
    keep = set(rts) | set(devs) | {r + "-dbgsym" for r in rts}
    return dict(upstream=vrec["upstream"], srcver=vrec["srcver"],
                runtimes=rts, devs=devs,
                binvers={k: v for k, v in vrec["binvers"].items() if k in keep})

for src, lang, rt_re in LIBS:
    rec = disc.get(src)
    if not rec or not rec["versions"]:
        report.append((src, lang, 0, "NO DISCOVERY DATA")); continue
    vs = [shrink(v, rt_re) for v in rec["versions"]]
    vs = [v for v in vs if v]
    vs = vs[:N_RELEASES]            # newest first
    vs = list(reversed(vs))         # oldest -> newest, so pairs read forward
    for a, b in zip(vs, vs[1:]):
        jobs.append(dict(src=src, lang=lang, v1=a, v2=b, arm="consecutive"))
    report.append((src, lang, len(vs), vs[0]["runtimes"] if vs else []))

# ---- control arm: same upstream version, different Debian revision
for src in CONTROL_LIBS:
    cfg = next((l for l in LIBS if l[0] == src), None)
    if not cfg:
        continue
    _, lang, rt_re = cfg
    try:
        allv = snap.source_versions(src)
    except Exception:
        continue
    groups = {}
    for v in allv:
        if is_prerelease(v):
            continue
        groups.setdefault(upstream_of(v), []).append(v)
    made = 0
    for up, revs in groups.items():
        if made >= CONTROL_PAIRS_PER_LIB or len(revs) < 2:
            continue
        newer, older = revs[0], revs[1]     # snapshot lists newest first
        try:
            v_new = dict(upstream=up, srcver=newer,
                         **_roles(src, newer, rt_re))
            v_old = dict(upstream=up, srcver=older,
                         **_roles(src, older, rt_re))
        except Exception:
            continue
        if not v_new["runtimes"] or not v_old["runtimes"]:
            continue
        jobs.append(dict(src=src, lang=lang, v1=v_old, v2=v_new, arm="control"))
        made += 1

json.dump(dict(jobs=jobs), open("/work/results/plan.json", "w"))
print(f"{'source':<26}{'lang':<5}{'releases':<10}runtime packages")
print("-"*90)
for src, lang, n, rts in report:
    print(f"{src:<26}{lang:<5}{n:<10}{rts}")
print("-"*90)
cons = sum(1 for j in jobs if j["arm"] == "consecutive")
ctrl = sum(1 for j in jobs if j["arm"] == "control")
print(f"consecutive-release pairs: {cons}\ncontrol (same upstream) pairs: {ctrl}\ntotal: {len(jobs)}")
