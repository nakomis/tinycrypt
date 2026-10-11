"""Loaders for KiCad footprints (.kicad_mod) and symbol libraries (.kicad_sym)."""
from dataclasses import dataclass
from pathlib import Path

from board.sexpr import Sym, find, find_all, parse


@dataclass
class Pad:
    number: str
    kind: str
    shape: str
    x: float
    y: float
    rot: float
    w: float
    h: float
    drill: float | None
    layers: list


@dataclass
class Footprint:
    name: str
    node: list
    pads: list
    courtyard: tuple


def _pad(node):
    at = find(node, "at")
    size = find(node, "size")
    drill = find(node, "drill")
    d = None
    if drill:
        nums = [v for v in drill[1:] if isinstance(v, (int, float))]
        d = float(nums[0]) if nums else None
    return Pad(str(node[1]), str(node[2]), str(node[3]), float(at[1]), float(at[2]),
               float(at[3]) if len(at) > 3 else 0.0, float(size[1]), float(size[2]), d,
               [str(v) for v in find(node, "layers")[1:]])


def _courtyard(node, pads):
    xs, ys = [], []
    for kind in ("fp_line", "fp_rect", "fp_poly", "fp_circle"):
        for g in find_all(node, kind):
            layer = find(g, "layer")
            if not layer or "CrtYd" not in str(layer[1]):
                continue
            if kind == "fp_circle":        # centre + a point on the circle
                c, e = find(g, "center"), find(g, "end")
                r = ((e[1] - c[1]) ** 2 + (e[2] - c[2]) ** 2) ** 0.5
                xs += [c[1] - r, c[1] + r]; ys += [c[2] - r, c[2] + r]
                continue
            for key in ("start", "end"):
                pt = find(g, key)
                if pt:
                    xs.append(float(pt[1])); ys.append(float(pt[2]))
            pts = find(g, "pts")
            for xy in find_all(pts or [], "xy"):
                xs.append(float(xy[1])); ys.append(float(xy[2]))
    if not xs:
        for p in pads:
            xs += [p.x - p.w / 2 - 0.25, p.x + p.w / 2 + 0.25]
            ys += [p.y - p.h / 2 - 0.25, p.y + p.h / 2 + 0.25]
    if not xs:
        return (0.0, 0.0, 0.0, 0.0)
    return (min(xs), min(ys), max(xs), max(ys))


def load_footprint(path):
    node = parse(Path(path).read_text())
    node[0] = Sym("footprint")
    pads = [_pad(p) for p in find_all(node, "pad")]
    return Footprint(str(node[1]), node, pads, _courtyard(node, pads))


def _pins(path, symbol):
    lib = parse(Path(path).read_text())
    for s in find_all(lib, "symbol"):
        if s[1] == symbol:
            stack = [s]
            while stack:
                n = stack.pop()
                for v in n:
                    if isinstance(v, list) and v and v[0] == "pin":
                        yield v
                    elif isinstance(v, list) and v and v[0] == "symbol":
                        stack.append(v)
            return
    raise KeyError(f"symbol {symbol!r} not in {path}")


def load_symbol_pins(path, symbol):
    out = {}
    for pin in _pins(path, symbol):
        out.setdefault(str(find(pin, "name")[1]), []).append(str(find(pin, "number")[1]))
    return out


def load_symbol_pin_types(path, symbol):
    return {str(find(pin, "number")[1]): str(pin[1]) for pin in _pins(path, symbol)}
