#!/usr/bin/env python3
"""Diff one pair of consecutive releases.

Uses abidiff with EXPLICIT shared-object pairing rather than abipkgdiff, because
abipkgdiff pairs ELF files by filename and therefore refuses to compare across a
SONAME rename: for icu 72 -> 73 it reports six "Removed binaries" and six "Added
binaries" with exit code 0 and no ABI analysis at all. SONAME bumps are exactly
the transitions where the interesting breakage lives, so they cannot be skipped.

Package handling (download, unpack, debug-info lookup) is otherwise the same
work abipkgdiff does internally; it is ~30 lines here and needs no per-library
configuration.
"""
import argparse, gzip, json, os, re, resource, shutil, subprocess, sys, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snap
from classify import classify_both, headline, is_private_symbol
from report_model import summarize, header_basenames

EXTRACT = "/data/extract"
REPORTS = os.environ.get("REPORTS", "/data/reports")
OUTDIR = os.environ.get("OUTDIR", "/data/pairout")
TIMEOUT = int(os.environ.get("ABIDIFF_TIMEOUT", "900"))
MEM_CAP = int(os.environ.get("MEM_CAP_GB", "4")) * 1024**3
for d in (EXTRACT, REPORTS, OUTDIR):
    os.makedirs(d, exist_ok=True)


def _limit():
    resource.setrlimit(resource.RLIMIT_AS, (MEM_CAP, MEM_CAP))


MAX_EXTRACT_GB = int(os.environ.get("MAX_EXTRACT_GB", "80"))


def gc_extract(keep):
    """Extracted trees are a cache: the .debs they came from are kept, so the
    oldest can be dropped once the tree grows past its budget."""
    try:
        entries = []
        total = 0
        for d in os.listdir(EXTRACT):
            p = os.path.join(EXTRACT, d)
            if not os.path.isdir(p) or d in keep:
                continue
            sz = sum(os.path.getsize(os.path.join(dp, f))
                     for dp, _, fs in os.walk(p) for f in fs
                     if os.path.exists(os.path.join(dp, f)))
            entries.append((os.path.getmtime(p), sz, p)); total += sz
        if total < MAX_EXTRACT_GB * 1024**3:
            return
        for _, sz, p in sorted(entries):
            shutil.rmtree(p, ignore_errors=True)
            total -= sz
            if total < MAX_EXTRACT_GB * 0.7 * 1024**3:
                break
    except Exception:
        pass


def materialize(vrec, tag):
    root = os.path.join(EXTRACT, tag)
    if os.path.exists(os.path.join(root, ".done")):
        return root
    shutil.rmtree(root, ignore_errors=True)
    bv = vrec["binvers"]
    for role, pkgs in (("rt", vrec["runtimes"]), ("dev", vrec["devs"]),
                       ("dbg", [r + "-dbgsym" for r in vrec["runtimes"]])):
        for pkg in pkgs:
            if pkg not in bv:
                continue
            _, h = snap.pick_amd64_build(pkg, bv[pkg])
            if h:
                snap.extract_deb(snap.fetch_file(h), os.path.join(root, role))
    open(os.path.join(root, ".done"), "w").close()
    return root


def so_by_stem(rt_root):
    out = {}
    for p in snap.find_sonames(rt_root):
        stem = os.path.basename(p).split(".so")[0]
        if stem not in out or os.path.getsize(p) > os.path.getsize(out[stem]):
            out[stem] = p
    return out


def detect_language(so):
    """C vs C++ decided by evidence, not by a hand-written label: a library is
    C++ if a meaningful share of its exported symbols are Itanium-mangled."""
    r = subprocess.run(["readelf", "--dyn-syms", "-W", so], capture_output=True, text=True)
    names = re.findall(r"\s(\S+)$", r.stdout)
    if not names:
        return "unknown", 0.0
    mangled = sum(1 for n in names if n.startswith("_Z"))
    frac = mangled / len(names)
    return ("cxx" if frac > 0.15 else "c"), round(frac, 3)


def run_abidiff(a, b, dbg1, dbg2, inc1, inc2, extra):
    cmd = ["abidiff"]
    for flag, val in (("--debug-info-dir1", dbg1), ("--debug-info-dir2", dbg2),
                      ("--headers-dir1", inc1), ("--headers-dir2", inc2)):
        if val and os.path.isdir(val):
            cmd += [flag, val]
    cmd += list(extra) + [a, b]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=TIMEOUT, preexec_fn=_limit)
        return dict(rc=r.returncode, out=r.stdout, err=r.stderr[:1000], timeout=False)
    except subprocess.TimeoutExpired:
        return dict(rc=None, out="", err="TIMEOUT", timeout=True)
    except Exception as e:
        return dict(rc=None, out="", err=f"EXC {e}", timeout=False)


def do(job):
    src, v1, v2 = job["src"], job["v1"], job["v2"]
    pid = f"{src}__{v1['upstream']}__{v2['upstream']}".replace("/", "_")
    res = dict(source=src, arm=job["arm"], pair=pid, up1=v1["upstream"], up2=v2["upstream"],
               src1=v1["srcver"], src2=v2["srcver"], libs=[], error=None)
    t1 = f"{src}__{v1['srcver']}".replace("/", "_")
    t2 = f"{src}__{v2['srcver']}".replace("/", "_")
    gc_extract({t1, t2})
    r1, r2 = materialize(v1, t1), materialize(v2, t2)
    s1, s2 = so_by_stem(os.path.join(r1, "rt")), so_by_stem(os.path.join(r2, "rt"))
    dbg1, dbg2 = os.path.join(r1, "dbg", "usr/lib/debug"), os.path.join(r2, "dbg", "usr/lib/debug")
    inc1, inc2 = os.path.join(r1, "dev", "usr/include"), os.path.join(r2, "dev", "usr/include")
    res.update(inc1=inc1, inc2=inc2, libs_v1=len(s1), libs_v2=len(s2))
    own = header_basenames(inc1, inc2)   # filters third-party type churn
    for stem in sorted(set(s1) & set(s2)):
        a, b = s1[stem], s2[stem]
        leafh = run_abidiff(a, b, dbg1, dbg2, inc1, inc2, ["--leaf-changes-only", "--harmless"])
        harm = run_abidiff(a, b, dbg1, dbg2, inc1, inc2, ["--harmless"])
        counts, det, summ = classify_both(leafh["out"], harm["out"])
        # Final, filtered numbers are computed here so the stored result needs
        # no later re-derivation from the (bulky, volatile) report archive.
        final = summarize(leafh["out"], harm["out"], own)
        lang, frac = detect_language(b)
        npriv = sum(1 for r in det.get("removed", []) + det.get("added", [])
                    if is_private_symbol(r.get("sym")))
        res["libs"].append(dict(
            stem=stem, lang=lang, mangled_frac=frac,
            size1=os.path.getsize(a), size2=os.path.getsize(b),
            soname1=snap.soname_of(a), soname2=snap.soname_of(b),
            rc_leafh=leafh["rc"], timeout=leafh["timeout"] or harm["timeout"],
            err=((leafh["err"] or "") + (harm["err"] or ""))[:300],
            counts=final["counts"], headline=final["headline"],
            headline_thirdparty=final["headline_thirdparty"],
            headline_private=final["headline_private"],
            headline_unfiltered=dict(headline(counts)), summary=summ,
            private_symbol_entries=npriv,
            n_changed_types=len(det.get("changed_types", [])),
        ))
        for tag, rep in (("leafh", leafh), ("harm", harm)):
            with gzip.open(os.path.join(REPORTS, f"{pid}__{stem}.{tag}.txt.gz"), "wt") as f:
                f.write(rep["out"])
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", default="/work/results/plan.json")
    ap.add_argument("--index", type=int, required=True)
    a = ap.parse_args()
    job = json.load(open(a.plan))["jobs"][a.index]
    out = os.path.join(OUTDIR, f"{a.index:05d}.json")
    try:
        res = do(job)
    except Exception as e:
        res = dict(source=job["src"], arm=job["arm"],
                   pair=f"{job['src']}__{job['v1']['upstream']}__{job['v2']['upstream']}",
                   up1=job["v1"]["upstream"], up2=job["v2"]["upstream"],
                   src1=job["v1"]["srcver"], src2=job["v2"]["srcver"],
                   libs=[], error=f"{e}\n{traceback.format_exc()[:900]}")
    json.dump(res, open(out, "w"))
    print(json.dumps(dict(i=a.index, pair=res["pair"], libs=len(res["libs"]),
                          err=bool(res["error"]))), flush=True)


main()
