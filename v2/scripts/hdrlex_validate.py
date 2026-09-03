#!/usr/bin/env python3
"""Validate the lexical header analyser against the same 30-case ground truth.
No compilation needed -- the cases carry their own header sources."""
import os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hdrlex import index_tree, compare
from calib_cases import CASES

rows, fails = [], 0
for name, case in sorted(CASES.items()):
    with tempfile.TemporaryDirectory() as td:
        idx = {}
        for tag in ("v1", "v2"):
            root = os.path.join(td, tag)
            for rel, content in case[tag].items():
                if not rel.startswith("include/"):
                    continue
                p = os.path.join(root, os.path.relpath(rel, "include"))
                os.makedirs(os.path.dirname(p), exist_ok=True)
                open(p, "w").write(content)
            idx[tag] = index_tree(root)
        c = compare(idx["v1"], idx["v2"])
    truth = case["truth"]
    want_body = truth == "inline_body_changed"
    want_macro = truth == "macro_value_changed"
    ok = (c["inline_body_changed"] > 0) == want_body and \
         (c["macro_value_changed"] > 0) == want_macro
    fails += not ok
    rows.append((("PASS" if ok else "FAIL"), name, truth,
                 f"body={c['inline_body_changed']} macro={c['macro_value_changed']} "
                 f"defs={c['defs_common']}"))
w = max(len(r[1]) for r in rows)
for st, n, t, g in rows:
    print(f"{st:4} {n:<{w}}  {t:<32} {g}")
print(f"\n{len(rows)-fails}/{len(rows)} pass")
