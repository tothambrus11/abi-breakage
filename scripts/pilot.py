#!/usr/bin/env python3
"""End-to-end pilot: can we diff two real Debian releases with dbgsym debug info?"""
import os, shutil, subprocess, sys
sys.path.insert(0, "/work/scripts")
import snap

SRC = "icu"
PAIR = [("72.1-3", "libicu72"), ("76.1-1", "libicu76")]
WORK = "/data/pilot"

shutil.rmtree(WORK, ignore_errors=True)
roots = []
for ver, runtime in PAIR:
    bp = dict()
    for name, bver in snap.binpackages(SRC, ver):
        bp.setdefault(name, []).append(bver)
    want = {"rt": runtime, "dev": "libicu-dev", "dbg": runtime + "-dbgsym"}
    root = os.path.join(WORK, ver)
    got = {}
    for role, pkg in want.items():
        if pkg not in bp:
            print(f"  MISSING {pkg} for {ver}"); continue
        bver, h = snap.pick_amd64_build(pkg, bp[pkg])
        if not h:
            print(f"  NO amd64 build for {pkg} @ {ver}"); continue
        deb = snap.fetch_file(h)
        d = os.path.join(root, role)
        ok, err = snap.extract_deb(deb, d)
        got[role] = d
        print(f"  {ver} {role:4} {pkg}={bver} -> {os.path.getsize(deb)//1024}KB extracted={ok}")
    roots.append(got)

# locate the library and its debug info
for i, g in enumerate(roots):
    sos = snap.find_sonames(g["rt"])
    print(f"[{i}] shared objects:", [os.path.basename(s) for s in sos])
    dbg = []
    for dp, _, fs in os.walk(g["dbg"]):
        for f in fs:
            if f.endswith(".debug"):
                dbg.append(os.path.join(dp, f))
    print(f"[{i}] debug files: {len(dbg)}", dbg[:2])
    g["sos"] = sos

# pick libicuuc (the common core, C++ with vtables)
def pick(g, stem):
    for s in g["sos"]:
        if os.path.basename(s).startswith(stem):
            return s
    return None

a, b = pick(roots[0], "libicuuc.so"), pick(roots[1], "libicuuc.so")
print("\ndiffing:", a, "\n     vs:", b)
cmd = ["abidiff",
       "--debug-info-dir1", os.path.join(roots[0]["dbg"], "usr/lib/debug"),
       "--debug-info-dir2", os.path.join(roots[1]["dbg"], "usr/lib/debug"),
       "--headers-dir1", os.path.join(roots[0]["dev"], "usr/include"),
       "--headers-dir2", os.path.join(roots[1]["dev"], "usr/include"),
       "--leaf-changes-only", "--harmless", a, b]
r = subprocess.run(cmd, capture_output=True, text=True)
print("rc =", r.returncode)
print("stderr:", r.stderr[:600])
print("stdout head:\n", "\n".join(r.stdout.splitlines()[:40]))
print("...\nstdout total lines:", len(r.stdout.splitlines()))
