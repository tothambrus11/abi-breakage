#!/usr/bin/env python3
"""Run the study: diff every pair of consecutive releases.

Each pair runs in its OWN subprocess with an address-space cap. abidiff on a
large C++ library (Qt, GDAL) can consume several GB of DWARF, and the analysis
VM has 7 GB; without isolation one such pair would OOM-kill the whole run
instead of failing alone. Results are written per pair, so the run is resumable.
"""
import gzip, json, os, resource, shutil, subprocess, sys, time, traceback
from concurrent.futures import ThreadPoolExecutor, as_completed
sys.path.insert(0, "/work/scripts")

REPORTS = "/data/reports"
EXTRACT = "/data/extract"
OUTDIR = "/data/pairout"
ABIDIFF_TIMEOUT = 1200
MEM_CAP = int(os.environ.get("MEM_CAP_GB", "4")) * 1024**3

for d in (REPORTS, EXTRACT, OUTDIR):
    os.makedirs(d, exist_ok=True)


# ----------------------------------------------------------------- worker side

def _limit():
    resource.setrlimit(resource.RLIMIT_AS, (MEM_CAP, MEM_CAP))


def materialize(vrec, tag):
    import snap
    root = os.path.join(EXTRACT, tag)
    if os.path.exists(os.path.join(root, ".done")):
        return root
    shutil.rmtree(root, ignore_errors=True)
    binvers = vrec["binvers"]
    roles = [("rt", vrec["runtimes"]), ("dev", vrec["devs"]),
             ("dbg", [r + "-dbgsym" for r in vrec["runtimes"]])]
    for role, pkgs in roles:
        for pkg in pkgs:
            if pkg not in binvers:
                continue
            bver, h = snap.pick_amd64_build(pkg, binvers[pkg])
            if not h:
                continue
            snap.extract_deb(snap.fetch_file(h), os.path.join(root, role))
    open(os.path.join(root, ".done"), "w").close()
    return root


def so_stems(rt_root):
    import snap
    out = {}
    for p in snap.find_sonames(rt_root):
        stem = os.path.basename(p).split(".so")[0]
        if stem not in out or os.path.getsize(p) > os.path.getsize(out[stem]):
            out[stem] = p
    return out


def run_abidiff(a, b, dbg1, dbg2, inc1, inc2, extra):
    cmd = ["abidiff"]
    for flag, val in (("--debug-info-dir1", dbg1), ("--debug-info-dir2", dbg2),
                      ("--headers-dir1", inc1), ("--headers-dir2", inc2)):
        if val and os.path.isdir(val):
            cmd += [flag, val]
    cmd += list(extra) + [a, b]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=ABIDIFF_TIMEOUT, preexec_fn=_limit)
        return dict(rc=r.returncode, out=r.stdout, err=r.stderr[:1500], timeout=False)
    except subprocess.TimeoutExpired:
        return dict(rc=None, out="", err="TIMEOUT", timeout=True)
    except Exception as e:
        return dict(rc=None, out="", err=f"EXC {e}", timeout=False)


def do_pair(job):
    import snap
    from classify import classify_both, headline
    src, lang, v1, v2 = job["src"], job["lang"], job["v1"], job["v2"]
    pid = f"{src}__{v1['upstream']}__{v2['upstream']}".replace("/", "_")
    res = dict(source=src, lang=lang, arm=job["arm"], pair=pid,
               up1=v1["upstream"], up2=v2["upstream"],
               src1=v1["srcver"], src2=v2["srcver"], libs=[], error=None)
    r1 = materialize(v1, f"{src}__{v1['srcver']}".replace("/", "_"))
    r2 = materialize(v2, f"{src}__{v2['srcver']}".replace("/", "_"))
    s1, s2 = so_stems(os.path.join(r1, "rt")), so_stems(os.path.join(r2, "rt"))
    dbg1 = os.path.join(r1, "dbg", "usr/lib/debug")
    dbg2 = os.path.join(r2, "dbg", "usr/lib/debug")
    inc1 = os.path.join(r1, "dev", "usr/include")
    inc2 = os.path.join(r2, "dev", "usr/include")
    res.update(inc1=inc1, inc2=inc2,
               libs_v1=len(s1), libs_v2=len(s2))
    for stem in sorted(set(s1) & set(s2)):
        a, b = s1[stem], s2[stem]
        leafh = run_abidiff(a, b, dbg1, dbg2, inc1, inc2,
                            ["--leaf-changes-only", "--harmless"])
        harm = run_abidiff(a, b, dbg1, dbg2, inc1, inc2, ["--harmless"])
        # The plain-mode run was only ever used for its exit code, which no
        # analysis consumes; dropping it cuts abidiff work by a third on the
        # large C++ libraries. The --harmless report IS kept, because leaf mode
        # provably omits "base class insertion" lines (calibration case
        # cxx_base_class_added) and it is the only place they survive.
        strict = dict(rc=None)
        counts, det, summ = classify_both(leafh["out"], harm["out"])
        res["libs"].append(dict(
            stem=stem,
            size1=os.path.getsize(a), size2=os.path.getsize(b),
            soname1=snap.soname_of(a), soname2=snap.soname_of(b),
            rc_leafh=leafh["rc"], rc_strict=strict["rc"],
            timeout=leafh["timeout"] or harm["timeout"],
            err=((leafh["err"] or "") + (harm["err"] or ""))[:400],
            counts=dict(counts), headline=dict(headline(counts)), summary=summ,
            n_changed_types=len(det.get("changed_types", [])),
            vtable_classes=sorted(set(x for x in det.get("vtable_classes", []) if x))[:50],
        ))
        with gzip.open(os.path.join(REPORTS, f"{pid}__{stem}.leafh.txt.gz"), "wt") as f:
            f.write(leafh["out"])
        with gzip.open(os.path.join(REPORTS, f"{pid}__{stem}.harm.txt.gz"), "wt") as f:
            f.write(harm["out"])
    return res


def worker(idx):
    plan = json.load(open("/work/results/plan.json"))
    job = plan["jobs"][idx]
    out = os.path.join(OUTDIR, f"{idx:04d}.json")
    try:
        res = do_pair(job)
    except Exception as e:
        res = dict(source=job["src"], lang=job["lang"], arm=job["arm"],
                   pair=f"{job['src']}__{job['v1']['upstream']}__{job['v2']['upstream']}",
                   up1=job["v1"]["upstream"], up2=job["v2"]["upstream"],
                   src1=job["v1"]["srcver"], src2=job["v2"]["srcver"],
                   libs=[], error=f"{e}\n{traceback.format_exc()[:1200]}")
    with open(out, "w") as f:
        json.dump(res, f)
    print(json.dumps(dict(idx=idx, pair=res["pair"], libs=len(res["libs"]),
                          err=bool(res["error"]))), flush=True)


# ----------------------------------------------------------------- driver side

def driver():
    plan = json.load(open("/work/results/plan.json"))
    n = len(plan["jobs"])
    todo = [i for i in range(n) if not os.path.exists(os.path.join(OUTDIR, f"{i:04d}.json"))]
    print(f"{n} pairs total, {len(todo)} to do", flush=True)
    nw = int(os.environ.get("WORKERS", "4"))

    def one(i):
        t0 = time.time()
        r = subprocess.run([sys.executable, "-u", __file__, "--one", str(i)],
                           capture_output=True, text=True, timeout=5400)
        return i, r.returncode, round(time.time() - t0, 1), r.stdout.strip()[-300:], r.stderr.strip()[-300:]

    done = 0
    with ThreadPoolExecutor(max_workers=nw) as ex:
        futs = [ex.submit(one, i) for i in todo]
        for fu in as_completed(futs):
            try:
                i, rc, secs, so, se = fu.result()
            except Exception as e:
                print(f"driver-level failure: {e}", flush=True); continue
            done += 1
            print(f"[{done}/{len(todo)}] idx={i} rc={rc} {secs}s {so}"
                  f"{' STDERR:' + se if rc != 0 else ''}", flush=True)
    print("ALL DONE", flush=True)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--one":
        worker(int(sys.argv[2]))
    else:
        driver()
