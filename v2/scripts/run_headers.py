#!/usr/bin/env python3
"""Header stage: the one measurement no existing ABI tool provides.

abidiff and abi-compliance-checker both work from compiled artefacts + declared
interfaces. Neither can see a change to the BODY of a header `inline` function,
an in-class method, or a template -- the code C++ copies into every client. The
calibration suite proves this: those cases produce empty abidiff output and exit
code 0 even with --harmless.

Indexes are cached per RELEASE (each appears in two consecutive pairs).
"""
import argparse, glob, json, os, pickle, sys, time, traceback
from concurrent.futures import ProcessPoolExecutor, as_completed
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hdrdiff import index_headers, compare

CACHE = "/data/hdrindex"
os.makedirs(CACHE, exist_ok=True)
MAX_HEADERS = int(os.environ.get("MAX_HEADERS", "700"))


def get_index(inc_root, lang, key):
    p = os.path.join(CACHE, key.replace("/", "_") + f".{lang}.pkl")
    if os.path.exists(p):
        try:
            return pickle.load(open(p, "rb"))
        except Exception:
            pass
    if not os.path.isdir(inc_root):
        idx = dict(defs={}, macros={}, public=set(), files=0, parsed=0,
                   failed=0, missing=True)
    else:
        t0 = time.time()
        idx = index_headers(inc_root, lang, limit_files=MAX_HEADERS)
        idx["seconds"] = round(time.time() - t0, 1)
    pickle.dump(idx, open(p, "wb"))
    return idx


def do(pair):
    lang = "c" if pair.get("lang") == "c" else "c++"
    try:
        a = get_index(pair["inc1"], lang, f"{pair['source']}__{pair['src1']}")
        b = get_index(pair["inc2"], lang, f"{pair['source']}__{pair['src2']}")
        c = compare(a, b)
        c["parse"] = dict(files1=a.get("files"), parsed1=a.get("parsed"),
                          files2=b.get("files"), parsed2=b.get("parsed"),
                          missing1=a.get("missing", False), missing2=b.get("missing", False))
        return dict(pair=pair["pair"], source=pair["source"], lang=pair.get("lang"),
                    up1=pair["up1"], up2=pair["up2"], hdr=c, error=None)
    except Exception as e:
        return dict(pair=pair["pair"], source=pair["source"], lang=pair.get("lang"),
                    up1=pair["up1"], up2=pair["up2"], hdr=None,
                    error=f"{e}\n{traceback.format_exc()[:600]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairout", default="/work/results/pairout")
    ap.add_argument("--out", default="/work/results/headers.json")
    ap.add_argument("--workers", type=int, default=int(os.environ.get("WORKERS", "6")))
    a = ap.parse_args()

    pairs = []
    for f in sorted(glob.glob(os.path.join(a.pairout, "*.json"))):
        d = json.load(open(f))
        if d.get("inc1") and not d.get("error"):
            langs = {l.get("lang") for l in d["libs"]}
            d["lang"] = "cxx" if "cxx" in langs else "c"
            pairs.append(d)
    print(f"{len(pairs)} pairs with headers", flush=True)
    out = []
    with ProcessPoolExecutor(max_workers=a.workers) as ex:
        futs = [ex.submit(do, p) for p in pairs]
        for i, fu in enumerate(as_completed(futs), 1):
            r = fu.result(); out.append(r)
            h = r["hdr"] or {}
            if i % 20 == 0 or r["error"]:
                print(f"[{i}/{len(pairs)}] {r['pair'][:46]:46} "
                      f"bodies={h.get('inline_body_changed','-')} "
                      f"common={h.get('defs_common','-')} "
                      f"macros={h.get('macro_value_changed','-')}"
                      f"{' ERR' if r['error'] else ''}", flush=True)
            json.dump(out, open(a.out, "w"))
    print("HEADERS DONE", flush=True)


main()
