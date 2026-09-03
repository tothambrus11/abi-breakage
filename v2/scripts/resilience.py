# -*- coding: utf-8 -*-
"""Map observed ABI change kinds to what a Swift/Hylo-style resilient boundary
would and would not absorb.

A resilient boundary buys, at a runtime cost:
  * opaque struct/class layout   -> field offsets and type size fetched at run
                                    time instead of baked into the client
  * non-frozen enums             -> clients cannot assume the case list is
                                    closed; they carry a default path
  * resilient method dispatch    -> vtable slots resolved indirectly, so adding
                                    or reordering methods does not shift indices
  * no cross-module inlining unless explicitly opted in (@inlinable / @frozen)

It buys nothing against changes that are also *source*-level API breaks:
removing a function, or changing its signature. No indirection saves a caller
whose call no longer type-checks.
"""

ABSORBED = {
    # layout opacity
    "field_added_to_struct":      "opaque layout",
    "field_removed_from_struct":  "opaque layout",
    "field_type_changed":         "opaque layout",
    "member_offset_changed":      "opaque layout",
    "type_size_changed":          "opaque layout",
    "base_class_changed":         "opaque layout",
    # enum resilience
    "enum_case_added":            "non-frozen enum",
    "enum_case_removed":          "non-frozen enum",
    # dispatch resilience
    "vtable_changed":             "resilient dispatch",
    # inlining opt-in (measured from headers, invisible to abidiff)
    "inline_body_changed":        "no implicit cross-module inlining",
    "macro_value_changed":        "no implicit cross-module inlining",
}

NOT_ABSORBED = {
    "function_signature_changed": "source-level API break; indirection cannot help",
    "symbol_removed":             "source-level API break; indirection cannot help",
}

NEUTRAL = {
    "symbol_added":               "additive; already compatible without resilience",
    "symbol_version_renamed":     "library renames all symbols per release by policy",
}

def verdict(kind):
    if kind in ABSORBED:     return "absorbed"
    if kind in NOT_ABSORBED: return "not_absorbed"
    return "neutral"

def mechanism(kind):
    return ABSORBED.get(kind) or NOT_ABSORBED.get(kind) or NEUTRAL.get(kind, "")
