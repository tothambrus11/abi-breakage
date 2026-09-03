#!/usr/bin/env python3
"""Validate the header analyser against the same 30-case ground truth."""
import sys, os
sys.path.insert(0, "/work/scripts")
from hdrdiff import index_headers, compare
from calib_cases import CASES

EXPECT_BODY = {"inline_body_changed"}
EXPECT_MACRO = {"macro_value_changed"}
rows, fails = [], 0
for name, case in sorted(CASES.items()):
    lang = "c" if case["lang"] == "c" else "c++"
    r1 = f"/data/calib/{name}/v1/include"
    r2 = f"/data/calib/{name}/v2/include"
    a = index_headers(r1, lang); b = index_headers(r2, lang)
    c = compare(a, b)
    truth = case["truth"]
    want_body = truth in EXPECT_BODY
    want_macro = truth in EXPECT_MACRO
    got_body = c["inline_body_changed"] > 0
    got_macro = c["macro_value_changed"] > 0
    ok = (got_body == want_body) and (got_macro == want_macro)
    if not ok: fails += 1
    rows.append((("PASS" if ok else "FAIL"), name, truth,
                 f"body={c['inline_body_changed']} macro={c['macro_value_changed']} "
                 f"defs={c['defs_common']} declchg={c['inline_decl_changed']}"))
w = max(len(r[1]) for r in rows)
for st, n, t, g in rows:
    print(f"{st:4} {n:<{w}}  {t:<32} {g}")
print(f"\n{len(rows)-fails}/{len(rows)} pass")
