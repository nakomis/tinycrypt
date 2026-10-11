"""Minimal KiCad s-expression reader/writer.

Bare atoms parse to Sym, quoted strings to str, numbers to int/float, so a
round trip keeps KiCad's quoting.
"""
import re

_TOKEN = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))', re.S)


class Sym(str):
    """A bare (unquoted) atom."""


def _atom(text):
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return Sym(text)


def parse(text):
    stack, cur = [], None
    for m in _TOKEN.finditer(text):
        lp, rp, qs, at = m.groups()
        if lp:
            new = []
            if cur is not None:
                cur.append(new)
                stack.append(cur)
            cur = new
        elif rp:
            if not stack:
                return cur
            cur = stack.pop()
        elif qs is not None:
            cur.append(re.sub(r'\\(.)', r'\1', qs))
        elif at:
            cur.append(_atom(at))
    raise ValueError("unbalanced s-expression")


def _fmt(v):
    if isinstance(v, list):
        return dumps(v)
    if isinstance(v, Sym):
        return str(v)
    if isinstance(v, str):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if isinstance(v, float):
        s = f"{v:.6f}".rstrip("0").rstrip(".")
        return s if s not in ("-0", "") else "0"
    return str(v)


def dumps(node):
    return "(" + " ".join(_fmt(v) for v in node) + ")"


def find(node, head):
    for v in node:
        if isinstance(v, list) and v and v[0] == head:
            return v
    return None


def find_all(node, head):
    return [v for v in node if isinstance(v, list) and v and v[0] == head]
