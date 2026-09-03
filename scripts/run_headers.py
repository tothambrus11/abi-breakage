#!/usr/bin/env python3
"""Header stage: measure client-visible body/macro churn for every pair.

Indexes are cached per RELEASE (each release appears in two consecutive pairs),
and each parse is time-budgeted because some libraries ship thousands of headers.
"""
import json, os, pickle, sys, time, traceback
from concurrent.futures import ProcessPoolExecutor, as_completed
sys.path.insert(0, "/work/scripts")
from hdrdiff import index_headers, compare

CACHE = "/data/hdrindex"
os.makedirs(CACHE, exist_ok=True)
MAX_HEADERS = 900


def get_index(inc_root, lang, key):
    p = os.path.join(CACHE, key.replace("/", "_") + ".pkl")
    if os.path.exists(p):
        with open(p, "rb") as f:
            return pickle.load(f)
    if not os.path.isdir(inc_root):
        idx = dict(defs={}, macros={}, files=0, parsed=0, failed=0, missing=True)
    else:
        t0 = time.time()
        idx = index_headers(inc_root, lang, limit_files=MAX_HEADERS)
        idx["seconds"] = round(time.time() - t0, 1)
    with open(p, "wb") as f:
        pickle.dump(idx, f)
    return idx


def do(pair):
    lang = "c" if pair["lang"] == "c" else "c++"
    try:
        a = get_index(pair["inc1"], lang, f"{pair['source']}__{pair['src1']}")
        b = get_index(pair["inc2"], lang, f"{pair['source']}__{pair['src2']}")
        c = compare(a, b)
        c["parse"] = dict(files1=a.get("files"), parsed1=a.get("parsed"), failed1=a.get("failed"),
                          files2=b.get("files"), parsed2=b.get("parsed"), failed2=b.get("failed"))
        return dict(pair=pair["pair"], source=pair["source"], lang=pair["lang"],
                    up1=pair["up1"], up2=pair["up2"], hdr=c, error=None)
    except Exception as e:
        return dict(pair=pair["pair"], source=pair["source"], lang=pair["lang"],
                    up1=pair["up1"], up2=pair["up2"], hdr=None,
                    error=f"{e}\n{traceback.format_exc()[:800]}")


def main():
    pairs = [p for p in json.load(open("/work/results/pairs_raw.json")) if p.get("inc1")]
    print(f"{len(pairs)} pairs", flush=True)
    out = []
    with ProcessPoolExecutor(max_workers=int(os.environ.get("WORKERS", "6"))) as ex:
        futs = [ex.submit(do, p) for p in pairs]
        for i, fu in enumerate(as_completed(futs), 1):
            r = fu.result(); out.append(r)
            h = r["hdr"] or {}
            print(f"[{i}/{len(pairs)}] {r['pair']:58} "
                  f"bodies_changed={h.get('inline_body_changed','-')} "
                  f"common_defs={h.get('defs_common','-')} "
                  f"macro_changed={h.get('macro_value_changed','-')}"
                  f"{' ERR' if r['error'] else ''}", flush=True)
            with open("/work/results/headers_raw.json", "w") as f:
                json.dump(out, f)
    print("done", flush=True)

main()
