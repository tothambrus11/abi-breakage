#!/usr/bin/env python3
"""Two scalability questions abipkgdiff must answer:
   1. Can it pair libicuuc.so.72 with libicuuc.so.76 across a SONAME rename?
   2. Is "private symbol version node" a generic substitute for header parsing?
"""
import json, os, re, subprocess, urllib.request, urllib.parse
BASE = "https://snapshot.debian.org"; CACHE = "/data/deb"
os.makedirs(CACHE, exist_ok=True)

def api(p):
    with urllib.request.urlopen(BASE + p, timeout=120) as r: return json.loads(r.read())

def fetch(pkg, ver):
    r = api(f"/mr/binary/{urllib.parse.quote(pkg)}/{urllib.parse.quote(ver)}/binfiles")
    h = next((x["hash"] for x in r["result"] if x["architecture"] == "amd64"), None) or \
        next((x["hash"] for x in r["result"] if x["architecture"] == "all"), None)
    if not h: return None
    p = os.path.join(CACHE, f"{pkg}_{ver.replace(':','_')}.deb")
    if not os.path.exists(p):
        with urllib.request.urlopen(f"{BASE}/file/{h}", timeout=600) as r, open(p, "wb") as f:
            f.write(r.read())
    return p

print("=== Q1: SONAME rename pairing (icu 72 -> 73) ===")

def newest(pkg):
    """Resolve a real version instead of guessing the Debian revision."""
    return api(f"/mr/binary/{urllib.parse.quote(pkg)}/")["result"][-1]["binary_version"]

v72, v73 = newest("libicu72"), newest("libicu73")
print("  resolved:", v72, "->", v73)
A = {p: fetch(p, v72) for p in ("libicu72", "libicu72-dbgsym")}
A["libicu-dev"] = fetch("libicu-dev", v72)
B = {p: fetch(p, v73) for p in ("libicu73", "libicu73-dbgsym")}
B["libicu-dev"] = fetch("libicu-dev", v73)
cmd = ["abipkgdiff", "--d1", A["libicu72-dbgsym"], "--d2", B["libicu73-dbgsym"],
       "--devel-pkg1", A["libicu-dev"], "--devel-pkg2", B["libicu-dev"],
       "--dso-only", "--leaf-changes-only", "--harmless",
       A["libicu72"], B["libicu73"]]
r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
print("rc =", r.returncode, " stdout lines =", len(r.stdout.splitlines()))
for l in r.stdout.splitlines()[:14]: print("   ", l)
print("   stderr:", (r.stderr.strip()[:400] or "(none)"))

print("\n=== Q2: are the dbus 'removed' symbols all in a PRIVATE version node? ===")
d1 = os.path.join(CACHE, "libdbus-1-3_1.15.2-1.deb")
os.makedirs("/tmp/x", exist_ok=True)
subprocess.run(["dpkg-deb", "-x", d1, "/tmp/x"], check=True)
so = subprocess.run("find /tmp/x -name 'libdbus-1.so.*' -not -type l | head -1",
                    shell=True, capture_output=True, text=True).stdout.strip()
out = subprocess.run(["readelf", "--dyn-syms", "-W", so], capture_output=True, text=True).stdout
nodes = {}
for line in out.splitlines():
    m = re.search(r"\s(\S+)@@?(\S+)$", line)
    if m: nodes.setdefault(m.group(2), []).append(m.group(1))
for k, v in sorted(nodes.items(), key=lambda x: -len(x[1])):
    print(f"   {k:28} {len(v):>5} symbols   e.g. {v[:3]}")
