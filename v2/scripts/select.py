#!/usr/bin/env python3
"""Pick the study corpus automatically -- no hand-written package lists.

A library qualifies if it is among Debian's most-installed `lib*` packages
(popcon) and its source ships both a `-dbgsym` (without DWARF abidiff sees only
symbol names) and a `-dev` (needed to separate public API from internals).

Language is decided from the archive's own dependency data: a C++ shared library
essentially always links libstdc++, so `Depends: libstdc++6` is an objective
marker. This matters because popcon's top ranks are overwhelmingly C system
libraries -- a plain "top N by installs" corpus gave 743 C transitions against
22 C++, leaving vtables, base classes and the whole C++ layout story
undersampled. C and C++ are therefore quota'd separately.
"""
import argparse, json, lzma, os, sys, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snap

POPCON = "https://popcon.debian.org/by_inst"
PACKAGES = "https://deb.debian.org/debian/dists/trixie/main/binary-amd64/Packages.xz"
POPCON_CACHE, PKG_CACHE = "/data/popcon.by_inst", "/data/Packages"

SKIP_SUFFIX = ("-dev", "-dbg", "-dbgsym", "-doc", "-common", "-data",
               "-bin", "-utils", "-tools")


def _download(url, path, decompress=False):
    if not os.path.exists(path):
        with urllib.request.urlopen(url, timeout=900) as r:
            data = r.read()
        if decompress:
            data = lzma.decompress(data)
        open(path, "wb").write(data)
    return path


def archive_depends():
    """binary package -> its Depends line, from the archive's Packages index."""
    _download(PACKAGES, PKG_CACHE, decompress=True)
    deps, name = {}, None
    for line in open(PKG_CACHE, errors="replace"):
        if line.startswith("Package: "):
            name = line[9:].strip()
        elif line.startswith("Depends: ") and name:
            deps[name] = line[9:].strip()
    return deps


def popcon_libs():
    _download(POPCON, POPCON_CACHE)
    out = []
    for line in open(POPCON_CACHE, errors="replace"):
        if line.startswith("#") or not line.strip():
            continue
        p = line.split()
        if len(p) < 3 or not p[0].isdigit():
            continue
        rank, name = int(p[0]), p[1]
        if not name.startswith("lib") or any(name.endswith(s) for s in SKIP_SUFFIX):
            continue
        if name.startswith(("libpython", "libperl", "libruby")):
            continue
        out.append((rank, name, int(p[2])))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--c-limit", type=int, default=70)
    ap.add_argument("--cxx-limit", type=int, default=50)
    ap.add_argument("--scan", type=int, default=4000)
    ap.add_argument("--out", default="/work/results/selected.json")
    a = ap.parse_args()

    deps = archive_depends()
    print(f"archive dependency data for {len(deps)} binary packages", flush=True)
    cands = popcon_libs()[:a.scan]
    print(f"examining {len(cands)} popular lib* packages", flush=True)

    seen, picked = set(), []
    n_c = n_cxx = 0
    for rank, name, inst in cands:
        if n_c >= a.c_limit and n_cxx >= a.cxx_limit:
            break
        lang = "cxx" if "libstdc++6" in deps.get(name, "") else "c"
        if lang == "c" and n_c >= a.c_limit:
            continue
        if lang == "cxx" and n_cxx >= a.cxx_limit:
            continue
        try:
            res = snap.api(f"/mr/binary/{name}/")["result"]
        except Exception:
            continue
        if not res:
            continue
        src = res[0].get("source") or name
        if src in seen:
            continue
        seen.add(src)
        try:
            names = {n for n, _ in snap.binpackages(src, res[0]["version"])}
        except Exception:
            continue
        if f"{name}-dbgsym" not in names or not any(n.endswith("-dev") for n in names):
            continue
        picked.append(dict(rank=rank, popcon_inst=inst, binary=name,
                           source=src, lang_hint=lang))
        if lang == "cxx":
            n_cxx += 1
        else:
            n_c += 1
        print(f"  [{len(picked):>3}] {lang:<3} rank={rank:<5} {name:<30} source={src}", flush=True)

    json.dump(picked, open(a.out, "w"), indent=1)
    print(f"\nselected {len(picked)} sources: {n_c} C, {n_cxx} C++ -> {a.out}")


main()
