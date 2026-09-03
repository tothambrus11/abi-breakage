#!/usr/bin/env python3
"""Validate the classifier against the calibration ground truth."""
import json, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from classify import classify_both, headline

# ground-truth label -> headline kind the classifier MUST report.
# None means: abidiff provably cannot see this change (we assert silence).
EXPECT = {
    "struct_field_added":             "field_added_to_struct",
    "struct_field_added_middle":      "field_added_to_struct",
    "struct_field_added_into_padding":"field_added_to_struct",
    "struct_field_type_changed":      "field_type_changed",
    "struct_field_removed":           "field_removed_from_struct",
    "enum_case_added":                "enum_case_added",
    "enum_case_added_widening":       "enum_case_added",
    "enum_case_removed":              "enum_case_removed",
    "function_param_type_changed":    "function_signature_changed",
    "function_param_added":           "function_signature_changed",
    "function_return_type_changed":   "function_signature_changed",
    "function_added":                 "symbol_added",
    "function_removed":               "symbol_removed",
    "vtable_virtual_added_end":       "vtable_changed",
    "vtable_virtual_added_middle":    "vtable_changed",
    "vtable_virtual_removed":         "vtable_changed",
    "class_made_polymorphic":         "vtable_changed",
    "base_class_added":               "base_class_changed",
    "method_added_nonvirtual":        "symbol_added",
    "inline_body_changed":            None,
    "macro_value_changed":            None,
    "function_param_qualifier_changed": None,
    "opaque_impl_changed":            None,
    "none":                           None,
}

d = json.load(open("/work/results/calibration_raw.json"))
rows, fails = [], 0
for name, r in sorted(d.items()):
    m = r["case_meta"]; truth = m["truth"]
    counts, det, summ = classify_both(r["leafh"]["out"], r["harmless"]["out"])
    h = headline(counts)
    want = EXPECT[truth]
    if want is None:
        ok = (len(h) == 0)
        got = ",".join(f"{k}={v}" for k, v in h.most_common()) or "<silent>"
    else:
        ok = want in h
        got = ",".join(f"{k}={v}" for k, v in h.most_common()) or "<silent>"
    if not ok: fails += 1
    rows.append((("PASS" if ok else "FAIL"), name, truth, want or "<must be silent>", got))

w = max(len(r[1]) for r in rows)
print(f"{'':4} {'case':<{w}}  {'ground truth':<32} {'expected kind':<28} classifier output")
print("-"*170)
for st, name, truth, want, got in rows:
    print(f"{st:4} {name:<{w}}  {truth:<32} {want:<28} {got}")
print("-"*170)
print(f"{len(rows)-fails}/{len(rows)} cases pass")
