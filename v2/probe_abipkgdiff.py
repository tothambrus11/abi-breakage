#!/usr/bin/env python3
"""Does abipkgdiff on raw .debs replace the hand-rolled extraction + filters?

Test case: dbus 1.15.2 -> 1.15.4, where the hand-rolled pipeline reported
"651 public functions removed". They are `_dbus_*` internals exported under a
version-stamped private ELF node (LIBDBUS_PRIVATE_1.15.2 -> _1.15.4).
"""
import json, os, subprocess, sys, urllib.request, urllib.parse

BASE = "https://snapshot.debian.org"
CACHE = "/data/deb"
os.makedirs(CACHE, exist_ok=True)

def api(path):
    with urllib.request.urlopen(BASE + path, timeout=120) as r:
        return json.loads(r.read())

def fetch(pkg, ver):
    r = api(f"/mr/binary/{urllib.parse.quote(pkg)}/{urllib.parse.quote(ver)}/binfiles")
    h = next((x["hash"] for x in r["result"] if x["architecture"] == "amd64"), None)
    if not h:
        h = next((x["hash"] for x in r["result"] if x["architecture"] == "all"), None)
    if not h:
        return None
    p = os.path.join(CACHE, f"{pkg}_{ver.replace(':','_')}.deb")
    if not os.path.exists(p):
        with urllib.request.urlopen(f"{BASE}/file/{h}", timeout=300) as r, open(p, "wb") as f:
            f.write(r.read())
    return p

V1, V2 = "1.15.2-1", "1.15.4-1"
got = {}
for v in (V1, V2):
    for pkg in ("libdbus-1-3", "libdbus-1-3-dbgsym", "libdbus-1-dev"):
        got[(v, pkg)] = fetch(pkg, v)
        print(f"  {v} {pkg:22} -> {os.path.basename(got[(v,pkg)] or 'MISSING')}")

def run(label, extra):
    cmd = ["abipkgdiff",
           "--d1", got[(V1, "libdbus-1-3-dbgsym")], "--d2", got[(V2, "libdbus-1-3-dbgsym")],
           "--devel-pkg1", got[(V1, "libdbus-1-dev")], "--devel-pkg2", got[(V2, "libdbus-1-dev")],
           "--dso-only", *extra,
           got[(V1, "libdbus-1-3")], got[(V2, "libdbus-1-3")]]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    head = [l for l in r.stdout.splitlines() if "summary" in l or "Removed" in l][:6]
    print(f"\n### {label}  rc={r.returncode}  stdout_lines={len(r.stdout.splitlines())}")
    for l in head:
        print("   ", l)
    if r.stderr.strip():
        print("    stderr:", r.stderr.strip()[:300])
    return r.stdout

run("plain", [])
run("--drop-private-types", ["--drop-private-types"])
run("--drop-private-types --leaf --harmless",
    ["--drop-private-types", "--leaf-changes-only", "--harmless"])
