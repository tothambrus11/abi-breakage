#!/usr/bin/env python3
"""Run every pair. Each pair is its own subprocess so a crash or OOM loses one
pair rather than the run, and completed pairs are skipped on restart."""
import argparse, json, os, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed

OUTDIR = os.environ.get("OUTDIR", "/data/pairout")
os.makedirs(OUTDIR, exist_ok=True)

ap = argparse.ArgumentParser()
ap.add_argument("--plan", default="/work/results/plan.json")
ap.add_argument("--workers", type=int, default=int(os.environ.get("WORKERS", "5")))
ap.add_argument("--pair-timeout", type=int, default=5400)
a = ap.parse_args()

jobs = json.load(open(a.plan))["jobs"]
todo = [i for i in range(len(jobs))
        if not os.path.exists(os.path.join(OUTDIR, f"{i:05d}.json"))]
print(f"{len(jobs)} pairs total, {len(todo)} to do, {a.workers} workers", flush=True)
here = os.path.dirname(os.path.abspath(__file__))

def one(i):
    t0 = time.time()
    try:
        r = subprocess.run([sys.executable, "-u", os.path.join(here, "diff_pair.py"),
                            "--plan", a.plan, "--index", str(i)],
                           capture_output=True, text=True, timeout=a.pair_timeout)
        return i, r.returncode, round(time.time()-t0, 1), r.stdout.strip()[-200:], r.stderr.strip()[-200:]
    except subprocess.TimeoutExpired:
        return i, -9, round(time.time()-t0, 1), "", "PAIR TIMEOUT"

done = 0
with ThreadPoolExecutor(max_workers=a.workers) as ex:
    futs = [ex.submit(one, i) for i in todo]
    for fu in as_completed(futs):
        i, rc, secs, so, se = fu.result()
        done += 1
        print(f"[{done}/{len(todo)}] i={i} rc={rc} {secs}s {so}"
              f"{'  STDERR:'+se if rc != 0 else ''}", flush=True)
print("ALL DONE", flush=True)
