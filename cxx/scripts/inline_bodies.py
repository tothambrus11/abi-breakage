#!/usr/bin/env python3
"""Supplementary analysis: changes to the bodies of inline functions and of
templates between consecutive releases, recomputed definition by definition
from the two header indexes of every pair. Not part of the main study's
break definitions; it describes what no ABI tool sees.

    scripts/inline_bodies.py [study-dir]  > cxx/SUPPLEMENT_INLINE.md

Definitions are matched by clang USR. A definition is a *template* if it is
a function template or lives inside a class template (USR carries "@FT@" or
"@ST>"); everything else with a body in a header is an *inline* function
(static inline in C, inline/member functions in C++). Fingerprints are
token-level, so any formatting-preserving refactor counts: every number is
an upper bound on semantic change.
"""
import json
import os
import sys
from collections import Counter, defaultdict

BINARY = {
    "field_added_to_struct", "field_removed_from_struct", "field_type_changed",
    "member_offset_changed", "type_size_changed", "base_class_changed",
    "vtable_changed", "function_signature_changed", "symbol_removed",
}


def load(path):
    with open(path) as f:
        return json.load(f)["data"]


def is_template(usr, kind):
    return kind == "FunctionTemplate" or "@FT@" in usr or "@ST>" in usr


def index_name(ws, source, version, lang):
    v = version.replace("/", "_").replace(":", "%")
    return os.path.join(ws, "headers", "index", f"{source}@{v}.{lang}.json")


def pct(a, b):
    return f"{100 * a / b:.1f} %" if b else "n/a"


def main(ws):
    plan = load(os.path.join(ws, "plan.json"))
    summary = load(os.path.join(ws, "summary.json"))
    trans = {t["id"]: t for t in summary["transitions"]}
    jobs = {f"{j['source']}@{j['v1']['upstream']}..{j['v2']['upstream']}": j for j in plan["jobs"]}

    rows = []  # one per transition with both indexes
    changed_defs = []  # (id, usr, kind, is_template, tokens_a, tokens_b, decl_changed, path, name)
    index_cache = {}

    def get_index(path):
        if path not in index_cache:
            index_cache[path] = load(path) if os.path.exists(path) else None
        return index_cache[path]

    for pid, t in trans.items():
        job = jobs.get(pid)
        hp = os.path.join(ws, "headers", "pairs", pid + ".json")
        if not job or not os.path.exists(hp):
            continue
        h = load(hp)
        if not h.get("diff"):
            continue
        lang = h["language"]
        a = get_index(index_name(ws, job["source"], job["v1"]["source_version"], lang))
        b = get_index(index_name(ws, job["source"], job["v2"]["source_version"], lang))
        if a is None or b is None:
            continue
        da, db = a["definitions"], b["definitions"]
        n_inline = sum(1 for u, x in da.items() if not is_template(u, x["kind"]))
        n_tmpl = len(da) - n_inline
        ch_inline = ch_tmpl = 0
        for usr, x in da.items():
            y = db.get(usr)
            if not y or x["body"] == y["body"]:
                continue
            tmpl = is_template(usr, x["kind"])
            if tmpl:
                ch_tmpl += 1
            else:
                ch_inline += 1
            changed_defs.append((pid, usr, x["kind"], tmpl, x["tokens"], y["tokens"],
                                 x["decl"] != y["decl"], x["path"], x["name"]))
        poor = any(c["parsed"] != 0 and max(c.get("with_errors", 0), c["with_fatal_error"]) * 2 > c["parsed"]
                   for c in h["coverage"])
        rows.append({
            "id": pid, "source": t["source"], "language": t["language"], "level": t["level"],
            "defs": len(da), "inline_defs": n_inline, "template_defs": n_tmpl,
            "inline_changed": ch_inline, "template_changed": ch_tmpl,
            "added": h["diff"]["definitions_added"], "removed": h["diff"]["definitions_removed"],
            "strict_break": any(k in BINARY and n > 0 for k, n in t["strict"].items()),
            "poor": poor,
        })

    n = len(rows)
    by_lang = Counter(r["language"] for r in rows)
    with_inline = [r for r in rows if r["inline_defs"]]
    with_tmpl = [r for r in rows if r["template_defs"]]
    ch_i = [r for r in rows if r["inline_changed"]]
    ch_t = [r for r in rows if r["template_changed"]]
    ch_both = [r for r in rows if r["inline_changed"] and r["template_changed"]]
    ch_any = [r for r in rows if r["inline_changed"] or r["template_changed"]]

    out = []
    p = out.append
    p("# Supplement: inline function and template body changes\n")
    p("Recomputed per definition from the header indexes of every transition "
      "(`scripts/inline_bodies.py`). This is a description of header-level "
      "churn that no ABI tool observes; it is **not** folded into the break "
      "rates of RESULTS.md. Token-level fingerprints: every count is an upper "
      "bound on semantic change.\n")
    p("## 1. Coverage\n")
    p("| | all | C | C++ |")
    p("|---|---|---|---|")
    p(f"| transitions with both header indexes | {n} | {by_lang['c']} | {by_lang['cxx']} |")
    for label, sub in (("with >= 1 inline function body", with_inline), ("with >= 1 template body", with_tmpl)):
        c = Counter(r["language"] for r in sub)
        p(f"| {label} | {len(sub)} ({pct(len(sub), n)}) | {c['c']} | {c['cxx']} |")
    poor = sum(r["poor"] for r in rows)
    p(f"| poor header parse coverage (lower bounds) | {poor} ({pct(poor, n)}) | | |")
    p("")
    tot_i = sum(r["inline_defs"] for r in rows)
    tot_t = sum(r["template_defs"] for r in rows)
    p(f"Definitions in the OLD release's headers across all transitions: "
      f"{tot_i} inline functions, {tot_t} template definitions.\n")

    p("## 2. How often do bodies change?\n")
    p("Share of transitions (over those shipping at least one definition of that "
      "kind) in which at least one body changed.\n")
    p("| | inline function bodies | template bodies | either | both |")
    p("|---|---|---|---|---|")
    for lang_label, sel in (("all", lambda r: True), ("C", lambda r: r["language"] == "c"),
                            ("C++", lambda r: r["language"] == "cxx")):
        wi = [r for r in with_inline if sel(r)]
        wt = [r for r in with_tmpl if sel(r)]
        wa = [r for r in rows if sel(r) and (r["inline_defs"] or r["template_defs"])]
        ci = [r for r in ch_i if sel(r)]
        ct = [r for r in ch_t if sel(r)]
        ca = [r for r in ch_any if sel(r)]
        cb = [r for r in ch_both if sel(r)]
        p(f"| {lang_label} | {len(ci)}/{len(wi)} ({pct(len(ci), len(wi))}) | "
          f"{len(ct)}/{len(wt)} ({pct(len(ct), len(wt))}) | "
          f"{len(ca)}/{len(wa)} ({pct(len(ca), len(wa))}) | {len(cb)} |")
    p("")
    p("By release level (transitions with >= 1 changed body of the kind / transitions shipping the kind):\n")
    p("| level | inline | template |")
    p("|---|---|---|")
    for level in ("major", "minor", "patch", "snapshot", "other"):
        wi = [r for r in with_inline if r["level"] == level]
        wt = [r for r in with_tmpl if r["level"] == level]
        ci = sum(1 for r in wi if r["inline_changed"])
        ct = sum(1 for r in wt if r["template_changed"])
        p(f"| {level} | {ci}/{len(wi)} ({pct(ci, len(wi))}) | {ct}/{len(wt)} ({pct(ct, len(wt))}) |")
    p("")

    p("## 3. What changes, and how much?\n")
    inl = [d for d in changed_defs if not d[3]]
    tml = [d for d in changed_defs if d[3]]
    p("| | inline | template |")
    p("|---|---|---|")
    p(f"| changed definitions | {len(inl)} | {len(tml)} |")
    p(f"| body only (declaration fingerprint unchanged) | {sum(1 for d in inl if not d[6])} | {sum(1 for d in tml if not d[6])} |")
    p(f"| body and declaration | {sum(1 for d in inl if d[6])} | {sum(1 for d in tml if d[6])} |")
    for label, lo, hi in (("token count unchanged", 0, 0), ("1-4 tokens", 1, 4), ("5-19 tokens", 5, 19), ("20+ tokens", 20, 10**9)):
        ci = sum(1 for d in inl if lo <= abs(d[5] - d[4]) <= hi)
        ct = sum(1 for d in tml if lo <= abs(d[5] - d[4]) <= hi)
        p(f"| size delta {label} | {ci} | {ct} |")
    med = lambda xs: sorted(xs)[len(xs) // 2] if xs else 0  # noqa: E731
    p(f"| median body size of a changed definition (tokens, old) | {med([d[4] for d in inl])} | {med([d[4] for d in tml])} |")
    p("")
    kinds = Counter(d[2] for d in changed_defs)
    p("Changed definitions by clang cursor kind: " + ", ".join(f"{k} {v}" for k, v in kinds.most_common()) + ".\n")

    p("## 4. Where it concentrates\n")
    per_lib = defaultdict(lambda: [0, 0, 0, 0])  # transitions, inline changed trans, template changed trans, changed defs
    for r in rows:
        x = per_lib[r["source"]]
        x[0] += 1
        x[1] += bool(r["inline_changed"])
        x[2] += bool(r["template_changed"])
        x[3] += r["inline_changed"] + r["template_changed"]
    p("| library | transitions | with inline body change | with template body change | changed definitions |")
    p("|---|---|---|---|---|")
    for lib, x in sorted(per_lib.items(), key=lambda kv: (-kv[1][3], kv[0]))[:20]:
        p(f"| {lib} | {x[0]} | {x[1]} | {x[2]} | {x[3]} |")
    libs_any = sum(1 for x in per_lib.values() if x[1] or x[2])
    p(f"\n{libs_any} of {len(per_lib)} libraries change at least one body in ten releases; "
      f"the top five libraries account for "
      f"{pct(sum(sorted((x[3] for x in per_lib.values()), reverse=True)[:5]), len(changed_defs))} of changed definitions.\n")

    p("## 5. Does body churn coincide with binary breaks?\n")
    p("Strict binary break rate (RESULTS.md definition) among transitions with and without body changes, "
      "over transitions shipping at least one inlinable definition.\n")
    ship = [r for r in rows if r["inline_defs"] or r["template_defs"]]
    a = [r for r in ship if r["inline_changed"] or r["template_changed"]]
    b = [r for r in ship if not (r["inline_changed"] or r["template_changed"])]
    p("| | transitions | strict binary break |")
    p("|---|---|---|")
    p(f"| body changed | {len(a)} | {sum(r['strict_break'] for r in a)} ({pct(sum(r['strict_break'] for r in a), len(a))}) |")
    p(f"| no body change | {len(b)} | {sum(r['strict_break'] for r in b)} ({pct(sum(r['strict_break'] for r in b), len(b))}) |")
    p("")
    p("Definitions added / removed from headers (not body changes): "
      f"{sum(r['added'] for r in rows)} added, {sum(r['removed'] for r in rows)} removed across all transitions; "
      f"{sum(1 for r in rows if r['removed'])} transitions remove at least one inlinable definition.\n")

    p("The two groups are not comparable populations: a release that rewrites "
      "header bodies is usually a large release that also changes exported "
      "symbols and layouts, so the association is confounded by release size "
      "and says nothing causal.\n")
    p("## 6. Examples\n")
    p("The largest body change of each transition, twelve transitions with the largest changes:\n")
    seen = set()
    for d in sorted(changed_defs, key=lambda d: -abs(d[5] - d[4])):
        if d[0] in seen:
            continue
        seen.add(d[0])
        if len(seen) > 12:
            break
        p(f"* `{d[0]}`: {'template' if d[3] else 'inline'} `{d[8]}` in `{d[7]}`, {d[4]} -> {d[5]} tokens"
          f"{', declaration also changed' if d[6] else ''}")
    p("")
    p("## 7. Caveats\n")
    p("* Token-level comparison: renaming a local variable or reformatting a macro-generated "
      "body counts as a change; nothing here says whether behaviour changed.")
    p("* Templates are counted by definition, not by instantiation; a change to one template "
      "reaches every client instantiation of it.")
    p("* Class-template member functions are classified as templates by their USR; a "
      "non-template inline member of a template class is still a template here because "
      "clients instantiate it.")
    p("* Indexes cover the headers libclang could parse with the default flags; "
      f"{poor} transitions have poor parse coverage and their counts are lower bounds.")
    p("* C libraries contribute `static inline` helpers and macro-heavy headers; C++ "
      "libraries contribute member functions defined in class bodies, which is why the "
      "inline column is dominated by C++ member functions.")
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "study")
