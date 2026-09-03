# -*- coding: utf-8 -*-
"""Minimal snapshot.debian.org client + .deb extraction helpers."""
import json, os, re, subprocess, time, urllib.parse, urllib.request

BASE = "https://snapshot.debian.org"
CACHE = "/data/cache"
ARCH = "amd64"

os.makedirs(CACHE, exist_ok=True)
os.makedirs(f"{CACHE}/api", exist_ok=True)
os.makedirs(f"{CACHE}/deb", exist_ok=True)


def _get(url, tries=5):
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "abi-study/1.0"})
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.read()
        except Exception as e:                       # snapshot throttles; back off
            last = e
            time.sleep(min(2 ** i, 30))
    raise RuntimeError(f"GET failed {url}: {last}")


def api(path):
    """Cached JSON call against the snapshot 'machine readable' API."""
    key = os.path.join(CACHE, "api", re.sub(r"[^A-Za-z0-9._-]", "_", path) + ".json")
    if os.path.exists(key):
        with open(key) as f:
            return json.load(f)
    data = json.loads(_get(BASE + path))
    with open(key, "w") as f:
        json.dump(data, f)
    return data


def source_versions(src):
    return [r["version"] for r in api(f"/mr/package/{urllib.parse.quote(src)}/")["result"]]


def binpackages(src, version):
    r = api(f"/mr/package/{urllib.parse.quote(src)}/{urllib.parse.quote(version)}/binpackages")
    return [(x["name"], x["version"]) for x in r["result"]]


def binfile_hash(binpkg, binver, arch=ARCH):
    r = api(f"/mr/binary/{urllib.parse.quote(binpkg)}/{urllib.parse.quote(binver)}/binfiles")
    for x in r["result"]:
        if x["architecture"] == arch:
            return x["hash"]
    for x in r["result"]:                             # arch:all packages
        if x["architecture"] == "all":
            return x["hash"]
    return None


def dpkg_version_key():
    """Sort key using dpkg's own version comparison (not string order)."""
    import functools
    def cmp(a, b):
        if a == b:
            return 0
        r = subprocess.run(["dpkg", "--compare-versions", a, "gt", b])
        return 1 if r.returncode == 0 else -1
    return functools.cmp_to_key(cmp)


def pick_amd64_build(binpkg, candidate_versions):
    """Newest binary version of `binpkg` that actually has an amd64 build.

    binNMUs are often arch-specific (e.g. libicu72 72.1-3+b1 exists only for
    riscv64), so "newest version" and "newest amd64 build" are not the same.
    """
    for bver in sorted(set(candidate_versions), key=dpkg_version_key(), reverse=True):
        h = binfile_hash(binpkg, bver)
        if h:
            return bver, h
    return None, None


def fetch_file(h):
    """Download a file by content hash; cached forever (hashes are immutable)."""
    p = os.path.join(CACHE, "deb", h + ".deb")
    if os.path.exists(p) and os.path.getsize(p) > 0:
        return p
    data = _get(f"{BASE}/file/{h}")
    # Unique temp name + atomic replace: several workers legitimately want the
    # same .deb at once (a release is shared by two consecutive pairs), and a
    # shared ".part" path made them clobber each other's rename.
    tmp = f"{p}.{os.getpid()}.part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, p)
    return p


def extract_deb(deb, dest):
    os.makedirs(dest, exist_ok=True)
    r = subprocess.run(["dpkg-deb", "-x", deb, dest], capture_output=True, text=True)
    return r.returncode == 0, r.stderr


def find_sonames(root):
    """Real (non-symlink) shared objects under a extracted runtime package."""
    out = []
    for dirpath, _, files in os.walk(root):
        for fn in files:
            p = os.path.join(dirpath, fn)
            if os.path.islink(p):
                continue
            if re.search(r"\.so(\.\d+)*$", fn) and "/debug/" not in p:
                out.append(p)
    return sorted(out)


def soname_of(path):
    r = subprocess.run(["objdump", "-p", path], capture_output=True, text=True)
    m = re.search(r"^\s*SONAME\s+(\S+)", r.stdout, re.M)
    return m.group(1) if m else None
