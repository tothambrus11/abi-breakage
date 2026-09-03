#!/usr/bin/env python3
"""Build every calibration case and record exactly what abidiff reports."""
import json, os, shutil, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from calib_cases import CASES

OUT = "/data/calib"

def write(root, files):
    for rel, content in files.items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(content)

def build(root, lang):
    src = [os.path.join(root, "src", f) for f in sorted(os.listdir(os.path.join(root, "src")))]
    cc = "gcc" if lang == "c" else "g++"
    so = os.path.join(root, "lib.so")
    cmd = [cc, "-shared", "-fPIC", "-g", "-O2", "-I", os.path.join(root, "include"),
           *src, "-o", so]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr
    return so, ""

def run_abidiff(so1, so2, inc1, inc2, extra=()):
    cmd = ["abidiff", "--headers-dir1", inc1, "--headers-dir2", inc2, *extra, so1, so2]
    r = subprocess.run(cmd, capture_output=True, text=True)
    # abidiff exit code is a bitfield:
    #   0 ok, 1 error, 2 ABI_CHANGE, 4 ABI_INCOMPATIBLE_CHANGE (bit 3)
    return dict(rc=r.returncode, out=r.stdout, err=r.stderr, cmd=" ".join(cmd))

def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    results = {}
    for name, case in CASES.items():
        root = os.path.join(OUT, name)
        r1, r2 = os.path.join(root, "v1"), os.path.join(root, "v2")
        write(r1, case["v1"]); write(r2, case["v2"])
        so1, e1 = build(r1, case["lang"])
        so2, e2 = build(r2, case["lang"])
        if not so1 or not so2:
            results[name] = dict(case_meta=_meta(case), build_error=(e1 or e2))
            print(f"BUILD FAIL {name}: {(e1 or e2)[:300]}")
            continue
        inc1, inc2 = os.path.join(r1, "include"), os.path.join(r2, "include")
        results[name] = dict(
            case_meta=_meta(case),
            default=run_abidiff(so1, so2, inc1, inc2),
            harmless=run_abidiff(so1, so2, inc1, inc2, ["--harmless"]),
            leafh=run_abidiff(so1, so2, inc1, inc2, ["--leaf-changes-only", "--harmless"]),
            no_hdrs=_run_no_hdrs(so1, so2),
        )
        print(f"ran {name}")
    with open("/work/results/calibration_raw.json", "w") as f:
        json.dump(results, f, indent=1)
    print("wrote /work/results/calibration_raw.json")

def _run_no_hdrs(so1, so2):
    r = subprocess.run(["abidiff", so1, so2], capture_output=True, text=True)
    return dict(rc=r.returncode, out=r.stdout, err=r.stderr, cmd="abidiff (no headers-dir)")

def _meta(case):
    return {k: case[k] for k in ("lang", "truth", "breaks", "note")}

main()
