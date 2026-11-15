#!/usr/bin/env python3
"""Idempotent POSIX/clang conformance patches for vendored official TJS2 core.

Source: jeeb/kirikiri2 @ master, kirikiri2/src/core/tjs2/ (see VENDOR_MANIFEST.json)
The upstream code targets old Borland C++/GCC. Apple clang (C++14) rejects several
constructs. This script applies minimal, auditable fixes. Running it twice is a no-op.

Patches:
  P1 tjsHashSearch.h : dependent-base member lookup needs `inherited::` qualification
  P2 &tTJSVariantClosure(temp) : taking address of a temporary -> named local
  P3 tjsArray.cpp    : `stream>Write` upstream typo -> `stream.Write`
  P4 wcsstr() const  : discard-qualifier -> const_cast
  P5 `const destLength = ...` missing type -> `const tjs_int`
  P6 ptrdiff -> tTJSVariant assignment ambiguous -> explicit (tjs_int) cast
"""
import os, re, sys

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src")
changed = []


def edit(name, fn):
    p = os.path.join(SRC, name)
    # latin-1 is a byte-preserving 1:1 mapping. Upstream files mix UTF-8 and
    # Shift-JIS (Japanese comments); decoding as UTF-8 would fail. All our
    # replacements are pure ASCII, so byte content stays intact.
    s = open(p, encoding="latin-1").read()
    out = fn(s)
    if out != s:
        open(p, "w", encoding="latin-1", newline="").write(out)
        changed.append(name)


# P1 — qualify dependent base members (idempotent: only bare calls)
def p1(s):
    s = s.replace("if(GetCount() > MaxCount)", "if(inherited::GetCount() > MaxCount)")
    s = s.replace("ChopLast(GetCount() - MaxCount);",
                  "inherited::ChopLast(inherited::GetCount() - MaxCount);")
    return s


# P2 — replace `&tTJSVariantClosure(ARGS)` with a named local.
#
# Two shapes exist upstream:
#   (a) single-line  : `dsp->EnumMembers(flag, &tTJSVariantClosure(&cb, NULL), dsp);`
#   (b) wrapped call : `EnumMembers(flag,\n  &tTJSVariantClosure(&cb, NULL), this);`
# In (b) the declaration MUST go before the whole statement, never inside the
# argument list. We compute per-line parenthesis depth (ignoring strings/comments)
# and walk back to the first line of the statement before injecting.
def _paren_depths(lines):
    """Return (depth_before, depth_after) for each line, ignoring string/comment text."""
    before, after = [], []
    depth = 0
    in_block_comment = False
    for ln in lines:
        before.append(depth)
        i = 0
        in_str = False
        while i < len(ln):
            c = ln[i]
            nxt = ln[i + 1] if i + 1 < len(ln) else ""
            if in_block_comment:
                if c == "*" and nxt == "/":
                    in_block_comment = False
                    i += 2
                    continue
                i += 1
                continue
            if in_str:
                if c == "\\":
                    i += 2
                    continue
                if c == '"':
                    in_str = False
                i += 1
                continue
            if c == "/" and nxt == "/":
                break
            if c == "/" and nxt == "*":
                in_block_comment = True
                i += 2
                continue
            if c == '"':
                in_str = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth = max(0, depth - 1)
            i += 1
        after.append(depth)
    return before, after


def p2(s):
    lines = s.split("\n")
    before, after = _paren_depths(lines)
    token = "&tTJSVariantClosure("
    idx = 0
    # Collect (line_index, replacement_line, declaration, stmt_start_index)
    edits = []
    for li, ln in enumerate(lines):
        pos = ln.find(token)
        if pos < 0:
            continue
        start = pos + len(token)
        depth = 1
        j = start
        while j < len(ln) and depth:
            if ln[j] == "(":
                depth += 1
            elif ln[j] == ")":
                depth -= 1
            j += 1
        if depth != 0:
            continue  # expression spans lines in an unexpected way; leave alone
        args = ln[start:j - 1]
        idx += 1
        var = "_closure%d" % idx
        new_ln = ln[:pos] + "&" + var + ln[j:]
        # Walk back to the beginning of this statement: stop after the first
        # preceding line whose *exit* depth is 0 (i.e. a statement boundary).
        stmt = li
        while stmt > 0 and after[stmt - 1] > 0:
            stmt -= 1
        indent = re.match(r"\s*", lines[stmt]).group(0)
        decl = "%stTJSVariantClosure %s(%s);" % (indent, var, args)
        edits.append((li, new_ln, decl, stmt))

    # Rebuild: inject declarations immediately before their statement's first line.
    by_stmt = {}
    for li, new_ln, decl, stmt in edits:
        by_stmt.setdefault(stmt, []).append(decl)
    replace = {li: new_ln for li, new_ln, _, _ in edits}
    out = []
    for li, ln in enumerate(lines):
        for d in by_stmt.get(li, []):
            out.append(d)
        out.append(replace.get(li, ln))
    return "\n".join(out)


def p3(s):
    return s.replace("stream>Write(", "stream.Write(")


def p4(s):
    return s.replace(
        "p = wcsstr(s + start, (const tjs_char*)*pstr);",
        "p = const_cast<tjs_char*>(wcsstr(s + start, (const tjs_char*)*pstr));")


def p5(s):
    return s.replace("const destLength = s_len * count;",
                     "const tjs_int destLength = (tjs_int)(s_len * count);")


def p6(s):
    return s.replace("*result = i - ni->Items.begin();",
                     "*result = (tjs_int)(i - ni->Items.begin());")


edit("tjsHashSearch.h", p1)
for f in ("tjsArray.cpp", "tjsDictionary.cpp", "tjsInterCodeExec.cpp", "tjsNative.cpp"):
    edit(f, p2)
edit("tjsArray.cpp", p3)
edit("tjsInterCodeExec.cpp", lambda s: p5(p4(s)))
edit("tjsArray.cpp", p6)

print("patched files:", changed if changed else "(already clean / no-op)")
