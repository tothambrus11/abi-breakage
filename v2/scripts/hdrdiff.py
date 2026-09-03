# -*- coding: utf-8 -*-
"""Measure what abidiff structurally cannot see: changes to bodies that the
C/C++ model copies into every client -- header `inline` functions, in-class
methods, templates, constexpr initialisers, and object-like macros.

These never appear in DWARF type or ELF symbol information, so they are
invisible to abidiff by construction (proven by the calibration suite's
negative controls). They are measured here from the -dev package headers.
"""
import hashlib, os, re
import clang.cindex as ci

# Kinds whose in-header definition gets baked into client object code.
BODY_KINDS = {
    ci.CursorKind.FUNCTION_DECL,
    ci.CursorKind.CXX_METHOD,
    ci.CursorKind.FUNCTION_TEMPLATE,
    ci.CursorKind.CONSTRUCTOR,
    ci.CursorKind.DESTRUCTOR,
    ci.CursorKind.CONVERSION_FUNCTION,
}

# Any entity a client can name. Used to decide whether a symbol that abidiff
# reports as added/removed is part of the PUBLIC API or a private internal that
# merely happens to be exported (dbus exports ~300 `_dbus_*` internals under a
# version-stamped private ELF node; those are not API evolution).
DECL_KINDS = {
    ci.CursorKind.FUNCTION_DECL, ci.CursorKind.CXX_METHOD,
    ci.CursorKind.FUNCTION_TEMPLATE, ci.CursorKind.CONSTRUCTOR,
    ci.CursorKind.DESTRUCTOR, ci.CursorKind.CONVERSION_FUNCTION,
    ci.CursorKind.VAR_DECL,
}

HDR_EXT = (".h", ".hpp", ".hh", ".hxx", ".H", ".inl", ".ipp", "")

def header_files(root):
    out = []
    for dp, _, fs in os.walk(root):
        for f in fs:
            if f.endswith(HDR_EXT[:-1]) or ("." not in f and "/c++/" in dp):
                out.append(os.path.join(dp, f))
    return sorted(out)


def _norm_tokens(cursor):
    """Normalised token spelling of a cursor's extent (whitespace/comments gone)."""
    try:
        toks = [t.spelling for t in cursor.get_tokens()]
    except Exception:
        return None
    return " ".join(toks) if toks else None


def _h(s):
    return hashlib.sha1(s.encode("utf-8", "replace")).hexdigest()[:16] if s else None


def _body_of(cursor):
    for ch in cursor.get_children():
        if ch.kind == ci.CursorKind.COMPOUND_STMT:
            return _norm_tokens(ch)
    return None


def index_headers(include_root, lang="c++", extra_args=(), limit_files=None):
    """Map every client-visible definition in these headers to a body hash."""
    idx = ci.Index.create()
    files = header_files(include_root)
    if limit_files:
        files = files[:limit_files]
    std = "-std=c++17" if lang == "c++" else "-std=gnu11"
    args = ["-x", lang, std, "-I", include_root, "-ferror-limit=0", "-w",
            "-DNDEBUG", *extra_args]
    defs, macros, public = {}, {}, set()
    parsed, failed = 0, 0
    for f in files:
        try:
            tu = idx.parse(f, args=list(args),
                           options=ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
        except Exception:
            failed += 1
            continue
        parsed += 1
        for c in tu.cursor.walk_preorder():
            loc = c.location.file
            if loc is None:
                continue
            path = str(loc.name)
            if not path.startswith(include_root):
                continue                       # skip system / other-package headers
            rel = os.path.relpath(path, include_root)

            if c.kind == ci.CursorKind.MACRO_DEFINITION:
                toks = _norm_tokens(c)
                if toks and " " in toks:       # object-like macro with a value
                    name = c.spelling
                    if not name.startswith("_"):
                        macros[(rel, name)] = _h(toks)
                continue

            if c.kind in DECL_KINDS:
                nm = c.spelling
                if nm:
                    public.add(nm)
                    q, cur = [nm], c.semantic_parent
                    while cur is not None and cur.kind != ci.CursorKind.TRANSLATION_UNIT:
                        if cur.spelling:
                            q.append(cur.spelling)
                        cur = cur.semantic_parent
                    if len(q) > 1:
                        public.add("::".join(reversed(q)))

            if c.kind in BODY_KINDS and c.is_definition():
                body = _body_of(c)
                if body is None:
                    continue
                usr = c.get_usr() or f"{rel}:{c.spelling}"
                decl = f"{c.spelling}|{c.result_type.spelling}|" + ",".join(
                    a.type.spelling for a in c.get_arguments()) if c.kind != ci.CursorKind.DESTRUCTOR else c.spelling
                defs[usr] = dict(rel=rel, name=c.spelling, kind=str(c.kind).split(".")[-1],
                                 decl=_h(decl), body=_h(body), ntok=len(body.split()))
    return dict(defs=defs, macros=macros, public=public,
                files=len(files), parsed=parsed, failed=failed)


def compare(a, b):
    """Compare two header indexes -> counts of client-visible body changes."""
    da, db = a["defs"], b["defs"]
    ka, kb = set(da), set(db)
    both = ka & kb
    body_changed = [k for k in both if da[k]["body"] != db[k]["body"]]
    decl_changed = [k for k in both if da[k]["decl"] != db[k]["decl"]]
    tmpl_changed = [k for k in body_changed if "TEMPLATE" in da[k]["kind"]]

    ma, mb = a["macros"], b["macros"]
    mboth = set(ma) & set(mb)
    macro_changed = [k for k in mboth if ma[k] != mb[k]]

    return dict(
        defs_v1=len(da), defs_v2=len(db), defs_common=len(both),
        inline_body_changed=len(body_changed),
        inline_body_changed_template=len(tmpl_changed),
        inline_decl_changed=len(decl_changed),
        inline_def_added=len(kb - ka), inline_def_removed=len(ka - kb),
        macros_v1=len(ma), macros_v2=len(mb),
        macro_value_changed=len(macro_changed),
        macro_added=len(set(mb) - set(ma)), macro_removed=len(set(ma) - set(mb)),
        examples=[dict(name=da[k]["name"], rel=da[k]["rel"], kind=da[k]["kind"])
                  for k in body_changed[:8]],
    )
