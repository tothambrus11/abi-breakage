#!/usr/bin/env python3
"""Re-classify the stored abidiff reports against the LIBRARY'S OWN PUBLIC API.

Two corrections, both discovered by inspecting real output rather than assumed:

1. Header ownership. A library's public API drags in types it does not own --
   glibc's `_IO_FILE`, libstdc++'s `std::tuple` and `allocator`. When the build
   toolchain changes, abidiff attributes that layout churn to the library.
   Changed types are therefore attributed to their declaring header and split
   into "own" vs "third-party".

2. Public API names. `--headers-dir` restricts TYPE analysis to public headers
   (proven by the pimpl control) but still lists added/removed FUNCTIONS that
   are exported yet never declared publicly -- dbus exports ~300 `_dbus_*`
   internals under a version-stamped private ELF node, which reads as "651
   public functions removed".

Where a release's headers could not be indexed the pair keeps unfiltered counts
and is flagged, rather than silently reporting zero.
"""
import glob, gzip, json, os, pickle, re, sys
from collections import Counter, defaultdict
sys.path.insert(0, "/work/scripts")
from classify import (parse_tree, ENTRY_RE, TYPEBLK_LOC_RE, TYPEBLK_RE,
                      _base_name, _count_subtree, HEADLINE)

HDRCACHE = "/data/hdrindex"
EXTRACT = "/data/extract"
MIN_PUBLIC = 5

TYPE_SCOPED = {"struct_field_added", "struct_field_removed",
               "struct_field_type_changed", "enum_case_added",
               "enum_case_removed", "enum_case_changed", "type_size_changed",
               "type_alignment_changed", "member_offset_changed",
               "base_class_added", "base_class_removed",
               "member_fn_added", "member_fn_removed",
               "vtable_offset_changed", "vtable_entry_added",
               "vtable_entry_removed", "vtable_created", "method_became_virtual"}


def own_header_basenames(source, srcver):
    root = os.path.join(EXTRACT, f"{source}__{srcver}".replace("/", "_"),
                        "dev", "usr", "include")
    if not os.path.isdir(root):
        return None
    out = set()
    for dp, _, fs in os.walk(root):
        for f in fs:
            out.add(f)
    return out or None


def public_names(source, srcver):
    p = os.path.join(HDRCACHE, f"{source}__{srcver}".replace("/", "_") + ".pkl")
    if not os.path.exists(p):
        return None
    try:
        pub = pickle.load(open(p, "rb")).get("public") or set()
    except Exception:
        return None
    return pub if len(pub) >= MIN_PUBLIC else None


def is_public(rec, pub):
    b = rec["base"]
    if b in pub or b.split("::")[-1] in pub:
        return True
    return (rec["sym"] or "").split("@")[0] in pub


def digits_blind(x):
    return re.sub(r"\d+", "#", x or "")


def process_report(report, own, pub):
    """-> (own_counts, thirdparty_counts, private_counts, entries)"""
    own_c, third_c, priv_c = Counter(), Counter(), Counter()
    root = parse_tree(report.splitlines())
    kept, dropped = {"A": [], "D": []}, {"A": [], "D": []}

    for node in root.children:
        m = TYPEBLK_LOC_RE.match(node.text) or TYPEBLK_RE.match(node.text)
        if m:
            f = m.group(2) if m.re is TYPEBLK_LOC_RE else None
            c = _count_subtree(node, Counter())
            if own is None or f is None or f in own:
                own_c += c
            else:
                third_c += c
            continue
        # function / variable sections: attribute per entry, public or not
        for sub in node.walk():
            em = ENTRY_RE.match(sub.text)
            if not em:
                continue
            rec = dict(kind=em.group(1), entity=em.group(2), sig=em.group(3),
                       sym=em.group(4), base=_base_name(em.group(3)))
            c = _count_subtree(sub, Counter())
            if pub is None or is_public(rec, pub):
                own_c += c
                if rec["kind"] in kept:
                    kept[rec["kind"]].append(rec)
            else:
                priv_c += c
                if rec["kind"] in dropped:
                    dropped[rec["kind"]].append(rec)
    return own_c, third_c, priv_c, kept, dropped


def resolve_symbols(kept):
    """Split kept [D]/[A] entries into renames, signature changes and real adds/removes."""
    rem, add = kept["D"], kept["A"]
    byn = defaultdict(list)
    for r in rem:
        byn[digits_blind(r["sym"] or r["sig"])].append(r)
    renamed, matched, add_left = 0, set(), []
    for a in add:
        k = digits_blind(a["sym"] or a["sig"])
        if byn.get(k):
            matched.add(id(byn[k].pop())); renamed += 1
        else:
            add_left.append(a)
    rem_left = [r for r in rem if id(r) not in matched]
    rb = {r["base"]: r for r in rem_left}
    ab = {a["base"]: a for a in add_left}
    sig = [b for b in set(rb) & set(ab) if rb[b]["sig"] != ab[b]["sig"]]
    return dict(removed=len(rem_left) - len(sig), added=len(add_left) - len(sig),
                renamed=renamed, sig_changed=len(sig))


def headline_of(counts):
    out = Counter()
    for k, srcs in HEADLINE.items():
        v = sum(counts.get(s, 0) for s in srcs)
        if v:
            out[k] = v
    return out


def main():
    out = []
    for f in sorted(glob.glob("/data/pairout/*.json")):
        d = json.load(open(f))
        if d.get("error") or not d["libs"]:
            continue
        own1, own2 = own_header_basenames(d["source"], d["src1"]), own_header_basenames(d["source"], d["src2"])
        own = (own1 or set()) | (own2 or set()) or None
        p1, p2 = public_names(d["source"], d["src1"]), public_names(d["source"], d["src2"])
        pub = ((p1 or set()) | (p2 or set())) if (p1 and p2) else None

        agg, third, priv = Counter(), Counter(), Counter()
        sym = Counter()
        for lib in d["libs"]:
            rp = os.path.join("/data/reports", f"{d['pair']}__{lib['stem']}.leafh.txt.gz")
            if not os.path.exists(rp):
                continue
            rep = gzip.open(rp, "rt").read()
            o, t, pv, kept, dropped = process_report(rep, own, pub)
            agg += o; third += t; priv += pv
            # Leaf mode drops "base class insertion/deletion"; recover it from
            # the --harmless report where that run was preserved, else from the
            # counts computed at collection time.
            hp = os.path.join("/data/reports", f"{d['pair']}__{lib['stem']}.harm.txt.gz")
            if os.path.exists(hp):
                ho, ht, _, _, _ = process_report(gzip.open(hp, "rt").read(), own, pub)
                for k in ("base_class_added", "base_class_removed"):
                    agg[k] = max(agg.get(k, 0), ho.get(k, 0))
                    third[k] = max(third.get(k, 0), ht.get(k, 0))
            else:
                for k in ("base_class_added", "base_class_removed"):
                    agg[k] = max(agg.get(k, 0), lib["counts"].get(k, 0))
            r = resolve_symbols(kept)
            for k, v in r.items():
                sym[k] += v

        # symbol-level tags come from the resolved entries, not raw marker counts
        for k in ("symbol_added", "symbol_removed", "fn_signature_changed_via_mangling",
                  "symbol_version_renamed"):
            agg.pop(k, None)
        agg["symbol_removed"] = sym["removed"]
        agg["symbol_added"] = sym["added"]
        agg["symbol_version_renamed"] = sym["renamed"]
        agg["fn_signature_changed_via_mangling"] = sym["sig_changed"]
        agg = Counter({k: v for k, v in agg.items() if v})

        out.append(dict(
            source=d["source"], lang=d["lang"], arm=d["arm"], pair=d["pair"],
            up1=d["up1"], up2=d["up2"], nlibs=len(d["libs"]),
            counts=dict(headline_of(agg)),
            counts_thirdparty=dict(headline_of(third)),
            counts_private=dict(headline_of(priv)),
            raw=dict(agg),
            public_filter=pub is not None, own_filter=own is not None,
            soname_changed=any(l["soname1"] != l["soname2"] for l in d["libs"]),
        ))
    json.dump(out, open("/work/results/pairs_public.json", "w"))
    nf = sum(1 for o in out if o["public_filter"] and o["own_filter"])
    print(f"wrote results/pairs_public.json: {len(out)} pairs, {nf} fully filtered")
    # what the filters removed
    t = Counter(); p = Counter()
    for o in out:
        for k, v in o["counts_thirdparty"].items(): t[k] += v
        for k, v in o["counts_private"].items(): p[k] += v
    print("\nevents attributed to THIRD-PARTY headers (excluded):", dict(t))
    print("\nevents on PRIVATE (non-header-declared) symbols (excluded):", dict(p))

main()
