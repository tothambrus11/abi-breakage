#!/usr/bin/env python3
"""Measure client-visible body/macro churn across the corpus.

Runs on the host with nothing but python3 + dpkg-deb: only the small `-dev`
packages are needed, each is deleted straight after indexing, and the resulting
index is a few KB. Disk high-water mark is a handful of megabytes.
"""
import argparse, json, os, pickle, shutil, subprocess, sys, tempfile, urllib.parse, urllib.request, time
from concurrent.futures import ThreadPoolExecutor, as_completed
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hdrlex import index_tree, compare

BASE = "https://snapshot.debian.org"
IDXDIR = "v2/results/hdrindex"
os.makedirs(IDXDIR, exist_ok=True)


def _get(url, tries=4):
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "abi-study/2.0"})
            with urllib.request.urlopen(req, timeout=180) as r:
                return r.read()
        except Exception as e:
            last = e; time.sleep(min(2 ** i, 20))
    raise RuntimeError(f"{url}: {last}")


def api(path):
    return json.loads(_get(BASE + path))


def amd64_hash(pkg, ver):
    r = api(f"/mr/binary/{urllib.parse.quote(pkg)}/{urllib.parse.quote(ver)}/binfiles")
    for want in ("amd64", "all"):
        for x in r["result"]:
            if x["architecture"] == want:
                return x["hash"]
    return None


def build_index(source, srcver, devs, binvers):
    key = os.path.join(IDXDIR, f"{source}__{srcver}".replace("/", "_") + ".pkl")
    if os.path.exists(key):
        try:
            return pickle.load(open(key, "rb"))
        except Exception:
            pass
    tmp = tempfile.mkdtemp(prefix="hdr")
    try:
        root = os.path.join(tmp, "x")
        for pkg in devs:
            for bver in sorted(set(binvers.get(pkg, [])), reverse=True):
                try:
                    h = amd64_hash(pkg, bver)
                except Exception:
                    continue
                if not h:
                    continue
                deb = os.path.join(tmp, "p.deb")
                open(deb, "wb").write(_get(f"{BASE}/file/{h}"))
                subprocess.run(["dpkg-deb", "-x", deb, root],
                               capture_output=True, timeout=300)
                os.remove(deb)
                break
        inc = os.path.join(root, "usr", "include")
        idx = index_tree(inc) if os.path.isdir(inc) else dict(defs={}, macros={}, files=0)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    pickle.dump(idx, open(key, "wb"))
    return idx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plans", nargs="+", required=True)
    ap.add_argument("--out", default="v2/results/headers.json")
    ap.add_argument("--workers", type=int, default=8)
    a = ap.parse_args()

    jobs = []
    seen = set()
    for pl in a.plans:
        for j in json.load(open(pl))["jobs"]:
            k = (j["src"], j["v1"]["srcver"], j["v2"]["srcver"])
            if k in seen:
                continue
            seen.add(k)
            jobs.append(j)
    print(f"{len(jobs)} pairs; indexing -dev headers", flush=True)

    def do(j):
        try:
            a_ = build_index(j["src"], j["v1"]["srcver"], j["v1"]["devs"], j["v1"]["binvers"])
            b_ = build_index(j["src"], j["v2"]["srcver"], j["v2"]["devs"], j["v2"]["binvers"])
            c = compare(a_, b_)
            return dict(source=j["src"], up1=j["v1"]["upstream"], up2=j["v2"]["upstream"],
                        hdr=c, error=None)
        except Exception as e:
            return dict(source=j["src"], up1=j["v1"]["upstream"], up2=j["v2"]["upstream"],
                        hdr=None, error=str(e)[:200])

    out = []
    with ThreadPoolExecutor(max_workers=a.workers) as ex:
        futs = [ex.submit(do, j) for j in jobs]
        for i, fu in enumerate(as_completed(futs), 1):
            r = fu.result(); out.append(r)
            if i % 50 == 0:
                print(f"[{i}/{len(jobs)}] {r['source']}", flush=True)
                json.dump(out, open(a.out, "w"))
    json.dump(out, open(a.out, "w"))
    print("HEADERS DONE", flush=True)


main()
