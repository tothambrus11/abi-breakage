# -*- coding: utf-8 -*-
"""Classify an abidiff report into ABI change kinds.

Input is the report from:  abidiff --leaf-changes-only --harmless ...
Leaf mode reports each changed TYPE exactly once, independent of how many
functions reference it, so counts are per-event rather than per-reference.

The report is a strict indentation tree, so we parse it as one instead of
grepping, which keeps counting nodes (e.g. "there are data member changes:",
which carries no number) attached to their children.
"""
import re
from collections import Counter, defaultdict

# --------------------------------------------------------------- tree parsing

class Node:
    __slots__ = ("text", "indent", "children")
    def __init__(self, text, indent):
        self.text, self.indent, self.children = text, indent, []
    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()

def parse_tree(lines):
    root = Node("", -1)
    stack = [root]
    for raw in lines:
        if not raw.strip():
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        node = Node(raw.strip(), indent)
        while len(stack) > 1 and stack[-1].indent >= indent:
            stack.pop()
        stack[-1].children.append(node)
        stack.append(node)
    return root

# --------------------------------------------------------------- summary head

SUMMARY_RE = {
    "leaf_artifacts":   re.compile(r"^Leaf changes summary: (\d+) artifacts? changed"),
    "leaf_types":       re.compile(r"^Changed leaf types summary: (\d+) .*leaf type"),
    "fn_removed":       re.compile(r"functions summary: (\d+) Removed"),
    "fn_changed":       re.compile(r"functions summary: \d+ Removed, (\d+) Changed"),
    "fn_added":         re.compile(r"functions summary: \d+ Removed, \d+ Changed, (\d+) Added"),
}

# ------------------------------------------------------- change-kind patterns
# Each entry: tag -> (regex on node text, how to count)
#   "n"      : take group(1) as the count
#   "child"  : count the node's children (for the numberless "there are ..." form)
#   "1"      : one occurrence per matching node

COUNTING = [
    # ---- struct / class data members
    ("struct_field_added",        re.compile(r"^(\d+) data member insertions?:?$"), "n"),
    ("struct_field_added",        re.compile(r"^there are data member insertions?:?$"), "child"),
    ("struct_field_removed",      re.compile(r"^(\d+) data member deletions?:?$"), "n"),
    ("struct_field_removed",      re.compile(r"^there are data member deletions?:?$"), "child"),
    # "N data member change:" / "there are data member changes:" is only a
    # container -- its children say whether the field changed TYPE or merely
    # shifted OFFSET. Counting the container conflates the two, so we match the
    # children instead. Two phrasings exist: default mode and leaf mode.
    ("struct_field_type_changed", re.compile(r"^type of '[^']+' changed:?$"), "1"),
    ("struct_field_type_changed", re.compile(r"^type '[^']*' of '[^']+' changed:?$"), "1"),
    # ---- enums
    ("enum_case_added",           re.compile(r"^(\d+) enumerator insertions?:?$"), "n"),
    ("enum_case_added",           re.compile(r"^there are enumerator insertions?:?$"), "child"),
    ("enum_case_removed",         re.compile(r"^(\d+) enumerator deletions?:?$"), "n"),
    ("enum_case_removed",         re.compile(r"^there are enumerator deletions?:?$"), "child"),
    ("enum_case_changed",         re.compile(r"^(\d+) enumerator changes?"), "n"),
    # ---- inheritance
    ("base_class_added",          re.compile(r"^(\d+) base class insertions?:?$"), "n"),
    ("base_class_removed",        re.compile(r"^(\d+) base class deletions?:?$"), "n"),
    # ---- member functions (virtual-ness resolved separately via voffset)
    ("member_fn_added",           re.compile(r"^(\d+) member function insertions?:?$"), "n"),
    ("member_fn_removed",         re.compile(r"^(\d+) member function deletions?:?$"), "n"),
    # ---- vtable, from libabigail's explicit notes
    ("vtable_entry_added",        re.compile(r"^note that this adds a new entry to the vtable of (?:class|struct) (.+)$"), "1"),
    ("vtable_entry_removed",      re.compile(r"^note that this removes an entry from the vtable of (?:class|struct) (.+)$"), "1"),
    ("vtable_created",            re.compile(r"^note that a vtable was added to (?:class|struct) (.+)$"), "1"),
    ("vtable_removed",            re.compile(r"^note that the vtable of (?:class|struct) (.+) was removed"), "1"),
    ("vtable_offset_changed",     re.compile(r"^the vtable offset of .+ changed from (\d+) to (\d+)$"), "1"),
    # ---- function signature internals
    ("fn_param_type_changed",     re.compile(r"^parameter \d+ of type '.+' (?:has sub-type )?changed:?$"), "1"),
    ("fn_param_added",            re.compile(r"^parameter \d+ of type '.+' was added$"), "1"),
    ("fn_param_removed",          re.compile(r"^parameter \d+ of type '.+' was removed$"), "1"),
    ("fn_return_type_changed",    re.compile(r"^return type changed:?$"), "1"),
    # ---- layout
    ("type_size_changed",         re.compile(r"^type size changed from (\d+) to (\d+) \(in bits\)$"), "1"),
    ("type_alignment_changed",    re.compile(r"^type alignment changed from (\d+) to (\d+)$"), "1"),
    ("member_offset_changed",     re.compile(r"^'.+' offset changed from (\d+) to (\d+) \(in bits\)"), "1"),
    ("member_offset_changed",     re.compile(r"^and offset changed from (\d+) to (\d+) \(in bits\)"), "1"),
    ("method_became_virtual",     re.compile(r"^method .+ is now declared virtual$"), "1"),
]

# Top-level [A]/[D]/[C] entries
ENTRY_RE = re.compile(r"^\[(A|D|C)\] '(\w[\w ]*?) (.+?)'(?:\s+\{(.+?)\})?(?: at .*)?$")
# changed-type block header:  'struct Point at lib.h:3:1' changed:
TYPEBLK_RE = re.compile(r"^'((?:struct|class|union|enum|typedef)[^']*?)(?: at [^']*)?' changed:$")
# Same, but keeping the declaring file. A library's public API drags in types
# it does not own (glibc's _IO_FILE, libstdc++'s std::tuple/allocator); when the
# build toolchain changes, those show up as "the library changed its layout".
TYPEBLK_LOC_RE = re.compile(
    r"^'((?:struct|class|union|enum|typedef)[^']*?) at ([^':]+):\d+:\d+' changed:$")


def _base_name(sig):
    """'void Counter::bump(int)' -> 'Counter::bump'  (name without params/return)."""
    s = sig.split("(")[0].strip()
    return s.split(" ")[-1] if " " in s else s


def classify(report):
    """Return (counts, detail) for one abidiff leaf report."""
    lines = report.splitlines()
    counts = Counter()
    detail = defaultdict(list)

    # --- summary header
    summary = {}
    for line in lines[:8]:
        for key, rx in SUMMARY_RE.items():
            m = rx.search(line)
            if m:
                summary[key] = int(m.group(1))

    root = parse_tree(lines)

    # --- counting patterns
    for node in root.walk():
        for tag, rx, how in COUNTING:
            m = rx.match(node.text)
            if not m:
                continue
            if how == "n":
                counts[tag] += int(m.group(1))
            elif how == "child":
                counts[tag] += len(node.children)
            else:
                counts[tag] += 1
            if tag.startswith("vtable"):
                detail["vtable_classes"].append(m.group(1) if m.groups() else "")
            break  # one tag per node

    # --- added / removed / changed top-level entries, for C++ mangled-name
    #     signature changes, which surface as a [D]+[A] pair, not as [C].
    removed, added = {}, {}
    for node in root.walk():
        m = ENTRY_RE.match(node.text)
        if not m:
            continue
        kind, entity, sig, sym = m.group(1), m.group(2), m.group(3), m.group(4)
        rec = dict(entity=entity, sig=sig, sym=sym, base=_base_name(sig))
        if kind == "A":
            counts["symbol_added"] += 1
            added[rec["base"]] = rec
            detail["added"].append(rec)
        elif kind == "D":
            counts["symbol_removed"] += 1
            removed[rec["base"]] = rec
            detail["removed"].append(rec)
        else:
            counts["symbol_changed"] += 1
            detail["changed"].append(rec)

    # A name that is both removed and added with a different signature is a
    # signature change that the mangler turned into remove+add (C++).
    for base in set(removed) & set(added):
        if removed[base]["sig"] != added[base]["sig"]:
            counts["fn_signature_changed_via_mangling"] += 1
            detail["sig_changed"].append((removed[base]["sig"], added[base]["sig"]))

    # Some libraries version-suffix every symbol (ICU: foo_72 -> foo_76), which
    # turns an entire release into "everything removed, everything added". That
    # is a naming policy, not N thousand independent ABI decisions, so we detect
    # and count it separately instead of letting it swamp the totals.
    def _digits_blind(x):
        return re.sub(r"\d+", "#", x or "")
    rem_norm = defaultdict(list)
    for rec in detail.get("removed", []):
        rem_norm[_digits_blind(rec["sym"] or rec["sig"])].append(rec)
    renamed = 0
    for rec in detail.get("added", []):
        k = _digits_blind(rec["sym"] or rec["sig"])
        if rem_norm.get(k):
            rem_norm[k].pop()
            renamed += 1
    if renamed:
        counts["symbol_version_renamed"] = renamed

    # --- changed types touched
    for node in root.walk():
        m = TYPEBLK_RE.match(node.text)
        if m:
            counts["types_changed"] += 1
            detail["changed_types"].append(m.group(1))

    return counts, dict(detail), summary


# --------------------------------------------------- roll-up to headline kinds
# The five kinds the study asks about, plus the ones the data forces us to add.

HEADLINE = {
    "field_added_to_struct": ["struct_field_added"],
    "field_removed_from_struct": ["struct_field_removed"],
    "field_type_changed": ["struct_field_type_changed"],
    "enum_case_added": ["enum_case_added"],
    "enum_case_removed": ["enum_case_removed"],
    "function_signature_changed": [
        "fn_param_type_changed", "fn_param_added", "fn_param_removed",
        "fn_return_type_changed", "fn_signature_changed_via_mangling",
    ],
    "vtable_changed": [
        "vtable_entry_added", "vtable_entry_removed", "vtable_created",
        "vtable_removed", "vtable_offset_changed", "method_became_virtual",
    ],
    "base_class_changed": ["base_class_added", "base_class_removed"],
    "symbol_added": ["symbol_added"],
    "symbol_removed": ["symbol_removed"],
    "member_offset_changed": ["member_offset_changed"],
    "type_size_changed": ["type_size_changed"],
}

def headline(counts):
    out = Counter()
    for k, srcs in HEADLINE.items():
        v = sum(counts.get(s, 0) for s in srcs)
        if v:
            out[k] = v
    return out


# --------------------------------------------------------------- merged view
# Calibration showed --leaf-changes-only omits "base class insertion/deletion"
# lines that the default report does emit. Leaf stays primary (it reports each
# changed type once); we take base-class facts from the default report.
LEAF_BLIND = ("base_class_added", "base_class_removed")

def classify_both(leaf_out, default_out):
    c_leaf, d_leaf, s_leaf = classify(leaf_out)
    c_def, d_def, _ = classify(default_out)
    merged = Counter(c_leaf)
    for tag in LEAF_BLIND:
        merged[tag] = max(c_leaf.get(tag, 0), c_def.get(tag, 0))
    merged = Counter({k: v for k, v in merged.items() if v})
    detail = dict(d_leaf)
    detail["default_mode_types"] = d_def.get("changed_types", [])
    return merged, detail, s_leaf


# ------------------------------------------------- location-scoped classification

def _count_subtree(node, counts):
    for n in node.walk():
        for tag, rx, how in COUNTING:
            m = rx.match(n.text)
            if not m:
                continue
            if how == "n":
                counts[tag] += int(m.group(1))
            elif how == "child":
                counts[tag] += len(n.children)
            else:
                counts[tag] += 1
            break
    return counts


def classify_scoped(report):
    """Split a leaf report into per-changed-type blocks, each with its header.

    Returns (type_blocks, unscoped) where type_blocks is a list of
    {type, file, counts} and `unscoped` holds markers that are not inside any
    type block (vtable notes on added/removed methods, signature changes).
    """
    root = parse_tree(report.splitlines())
    blocks, unscoped = [], Counter()
    for node in root.children:
        m = TYPEBLK_LOC_RE.match(node.text)
        if m:
            c = _count_subtree(node, Counter())
            blocks.append(dict(type=m.group(1), file=m.group(2),
                               counts={k: v for k, v in c.items() if v}))
            continue
        m2 = TYPEBLK_RE.match(node.text)
        if m2:
            c = _count_subtree(node, Counter())
            blocks.append(dict(type=m2.group(1), file=None,
                               counts={k: v for k, v in c.items() if v}))
            continue
        _count_subtree(node, unscoped)
    return blocks, unscoped
