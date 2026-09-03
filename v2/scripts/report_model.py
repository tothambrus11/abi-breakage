# -*- coding: utf-8 -*-
"""One place that turns abidiff reports into final, filtered change counts.

Three corrections are applied, all of them generic (no per-library config) and
all of them found by inspecting real output rather than assumed:

1. Header ownership. A library's public API drags in types it does not own --
   glibc's `_IO_FILE`, libstdc++'s `std::tuple`/`allocator`. When the build
   toolchain moves, abidiff attributes that layout churn to the library. In v1
   this accounted for ~30% of all changed-type blocks. Types are attributed to
   their declaring header and split into own vs third-party.

2. Private ELF version nodes. A symbol printed as `_dbus_abort@@LIBDBUS_PRIVATE_1.15.2`
   is private by the library's own declaration. dbus exports 657 such internals
   against 240 public symbols, so a routine release reads as "651 public
   functions removed". libabigail's --drop-private-types does NOT cover this --
   it drops private *types* only (verified against dbus 1.15.2 -> 1.15.4).

3. Mass version-renaming. ICU suffixes every symbol (`foo_72` -> `foo_73`), so
   one release looks like ~5,600 independent removals plus ~5,600 additions.
   That is a naming policy, counted separately rather than left to swamp totals.
"""
import os, re
from collections import Counter, defaultdict
from classify import (parse_tree, ENTRY_RE, TYPEBLK_LOC_RE, TYPEBLK_RE,
                      _base_name, _count_subtree, HEADLINE, is_private_symbol)


def header_basenames(*include_roots):
    out = set()
    for root in include_roots:
        if root and os.path.isdir(root):
            for _, _, fs in os.walk(root):
                out.update(fs)
    return out or None


def _digits_blind(x):
    return re.sub(r"\d+", "#", x or "")


def _scan(report, own):
    """-> own_counts, thirdparty_counts, private_counts, public_entries"""
    own_c, third_c, priv_c = Counter(), Counter(), Counter()
    kept = {"A": [], "D": []}
    for node in parse_tree(report.splitlines()).children:
        m_loc = TYPEBLK_LOC_RE.match(node.text)
        m_any = m_loc or TYPEBLK_RE.match(node.text)
        if m_any:
            f = m_loc.group(2) if m_loc else None
            c = _count_subtree(node, Counter())
            if own is None or f is None or f in own:
                own_c += c
            else:
                third_c += c
            continue
        for sub in node.walk():
            em = ENTRY_RE.match(sub.text)
            if not em:
                continue
            rec = dict(kind=em.group(1), sig=em.group(3), sym=em.group(4),
                       base=_base_name(em.group(3)))
            c = _count_subtree(sub, Counter())
            if is_private_symbol(rec["sym"]):
                priv_c += c
            else:
                own_c += c
                if rec["kind"] in kept:
                    kept[rec["kind"]].append(rec)
    return own_c, third_c, priv_c, kept


def _resolve_symbols(kept):
    rem, add = kept["D"], kept["A"]
    byn = defaultdict(list)
    for r in rem:
        byn[_digits_blind(r["sym"] or r["sig"])].append(r)
    renamed, matched, add_left = 0, set(), []
    for a in add:
        k = _digits_blind(a["sym"] or a["sig"])
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


def _headline(counts):
    out = Counter()
    for k, srcs in HEADLINE.items():
        v = sum(counts.get(s, 0) for s in srcs)
        if v:
            out[k] = v
    return out


def summarize(leafh_out, harm_out, own):
    own_c, third_c, priv_c, kept = _scan(leafh_out, own)
    # --leaf-changes-only omits "base class insertion/deletion" lines that the
    # plain --harmless report does emit (calibration case cxx_base_class_added).
    h_own, h_third, _, _ = _scan(harm_out, own)
    for k in ("base_class_added", "base_class_removed"):
        own_c[k] = max(own_c.get(k, 0), h_own.get(k, 0))
        third_c[k] = max(third_c.get(k, 0), h_third.get(k, 0))

    sym = _resolve_symbols(kept)
    for k in ("symbol_added", "symbol_removed",
              "fn_signature_changed_via_mangling", "symbol_version_renamed"):
        own_c.pop(k, None)
    own_c["symbol_removed"] = sym["removed"]
    own_c["symbol_added"] = sym["added"]
    own_c["symbol_version_renamed"] = sym["renamed"]
    own_c["fn_signature_changed_via_mangling"] = sym["sig_changed"]
    own_c = Counter({k: v for k, v in own_c.items() if v})
    return dict(
        counts=dict(own_c),
        headline=dict(_headline(own_c)),
        headline_thirdparty=dict(_headline(third_c)),
        headline_private=dict(_headline(priv_c)),
    )
