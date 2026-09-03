# -*- coding: utf-8 -*-
"""Pure-Python extraction of the parts of a header that get COPIED into clients.

This measures what no ABI tool can see. abidiff compares DWARF types and ELF
symbols, so a change to the body of a header `inline` function, an in-class
method, or a template is invisible to it -- proven by four negative controls in
the calibration suite, which stay silent even with --harmless. The same is true
of a changed object-like macro.

Deliberately lexical rather than libclang-based: headers from a -dev package
rarely parse standalone (missing transitive includes), the whole point is to
compare like with like across two versions of the same file, and dropping the
dependency removes a ~700MB image from the pipeline. Correctness is established
the same way as everything else here -- against the 30-case ground truth.
"""
import hashlib, os, re

HDR_EXT = (".h", ".hpp", ".hh", ".hxx", ".H", ".inl", ".ipp", ".tcc")

_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_STRING = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
_CONT = re.compile(r"\\\n")
_WS = re.compile(r"\s+")

def _strip(text):
    text = _CONT.sub(" ", text)
    text = _BLOCK_COMMENT.sub(" ", text)
    text = _LINE_COMMENT.sub(" ", text)
    return text


def _norm(s):
    return _WS.sub(" ", s).strip()


def _h(s):
    return hashlib.sha1(_norm(s).encode("utf-8", "replace")).hexdigest()[:16]


def _match_braces(text, open_idx):
    """Index just past the '}' closing the brace at open_idx, or None."""
    depth, i, n = 0, open_idx, len(text)
    while i < n:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return None


_MACRO = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]+(\S.*)$", re.M)

_CTRL = {"if", "for", "while", "switch", "catch", "return", "sizeof", "decltype",
         "else", "do", "try", "struct", "class", "union", "enum", "namespace",
         "extern", "static_assert"}
_IDENT_END = re.compile(r"[A-Za-z_0-9~:]+$")
_TAIL_OK = set(" \t\r\n:,&*<>[]()_")

MAX_FILE_BYTES = 4 * 1024 * 1024
LOOKBACK = 4000


def _param_close(text, brace):
    """Left-scan from a '{' for the ')' that closes the PARAMETER list.

    A constructor's member-init list means the nearest ')' is not the one we
    want (`Vec(int a,int b) : x(a), y(b) {`), so depth-0 candidates are gathered
    leftwards while the text between them still looks like an init list, and the
    leftmost is taken. Linear, unlike the regex this replaces, which backtracked
    catastrophically on large real headers.
    """
    k, depth, best, steps = brace - 1, 0, None, 0
    while k >= 0 and steps < LOOKBACK:
        c = text[k]
        if c == ")":
            if depth == 0:
                best = k
                # keep looking left only across init-list-ish text
                j = k - 1
                pd = 1
                while j >= 0 and pd:
                    if text[j] == ")":
                        pd += 1
                    elif text[j] == "(":
                        pd -= 1
                    j -= 1
                if j < 0:
                    return best
                seg_end = j
                m = j
                ok = True
                while m >= 0 and text[m] not in ");{}":
                    if text[m] in ";{}":
                        ok = False; break
                    m -= 1
                if m < 0 or text[m] != ")" or not ok:
                    return best
                k = m
                steps += 1
                continue
            depth -= 1
        elif c == "(":
            depth += 1
        elif c in ";{}":
            return best
        k -= 1
        steps += 1
    return best


def _match_open_paren(text, close):
    depth, k = 0, close
    while k >= 0:
        if text[k] == ")":
            depth += 1
        elif text[k] == "(":
            depth -= 1
            if depth == 0:
                return k
        k -= 1
    return None


def index_header_text(text):
    """-> (definitions {key: body_hash}, macros {name: value_hash})"""
    if len(text) > MAX_FILE_BYTES:
        return {}, {}
    src = _STRING.sub('""', _strip(text))
    defs = {}
    i, n = 0, len(src)
    while i < n:
        if src[i] != "{":
            i += 1
            continue
        close = _param_close(src, i)
        if close is None:
            i += 1
            continue
        opn = _match_open_paren(src, close)
        if opn is None:
            i += 1
            continue
        m = _IDENT_END.search(src[:opn].rstrip())
        if not m:
            i += 1
            continue
        name = m.group(0)
        if name.split("::")[-1] in _CTRL or name in _CTRL:
            i += 1
            continue
        end = _match_braces(src, i)
        if end is None:
            i += 1
            continue
        key = f"{name}({_norm(src[opn + 1:close])})"
        defs[key] = _h(src[i:end])
        i = end
    macros = {}
    for m in _MACRO.finditer(src):
        name, val = m.group(1), m.group(2).strip()
        if name.endswith("_H"):
            continue
        macros[name] = _h(val)
    return defs, macros


def index_tree(root):
    """Index every header under `root`, keyed by path relative to root."""
    defs, macros, nfiles = {}, {}, 0
    for dp, _, fs in os.walk(root):
        for f in fs:
            if not f.endswith(HDR_EXT):
                continue
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, root)
            try:
                text = open(p, errors="replace").read()
            except Exception:
                continue
            nfiles += 1
            d, mc = index_header_text(text)
            for k, v in d.items():
                defs[f"{rel}::{k}"] = v
            for k, v in mc.items():
                macros[f"{rel}::{k}"] = v
    return dict(defs=defs, macros=macros, files=nfiles)


def compare(a, b):
    da, db = a["defs"], b["defs"]
    both = set(da) & set(db)
    body_changed = [k for k in both if da[k] != db[k]]
    ma, mb = a["macros"], b["macros"]
    mboth = set(ma) & set(mb)
    macro_changed = [k for k in mboth if ma[k] != mb[k]]
    return dict(
        headers_v1=a["files"], headers_v2=b["files"],
        defs_v1=len(da), defs_v2=len(db), defs_common=len(both),
        inline_body_changed=len(body_changed),
        inline_def_added=len(set(db) - set(da)),
        inline_def_removed=len(set(da) - set(db)),
        macros_common=len(mboth), macro_value_changed=len(macro_changed),
        examples=[k.split("::")[-1] for k in body_changed[:6]],
    )
