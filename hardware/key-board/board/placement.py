"""Placement of parts on the board: positions, rotations and rotated courtyards."""
import math
from dataclasses import dataclass, field


@dataclass
class Placement:
    """outline = (x0, y0, x1, y1) in mm, KiCad coordinates (y down).
    at[ref] = (x, y, rotation_deg); KiCad rotation is counter-clockwise on screen.
    courtyards[ref] = unrotated footprint courtyard (xmin, ymin, xmax, ymax), filled in by
    the placer so that bbox() needs nothing but the Placement itself. It is optional, so
    Placement(outline, at) remains constructible."""
    outline: tuple
    at: dict
    courtyards: dict = field(default_factory=dict)


def rotate(x, y, deg):
    """KiCad rotation (CCW on screen, y down): x' = x cos + y sin, y' = -x sin + y cos."""
    t = math.radians(deg)
    return x * math.cos(t) + y * math.sin(t), -x * math.sin(t) + y * math.cos(t)


def bbox(placement, ref):
    """Axis-aligned bounding box (xmin, ymin, xmax, ymax) of ref's rotated courtyard."""
    x, y, rot = placement.at[ref]
    cx0, cy0, cx1, cy1 = placement.courtyards[ref]
    pts = [rotate(px, py, rot) for px in (cx0, cx1) for py in (cy0, cy1)]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return (x + min(xs), y + min(ys), x + max(xs), y + max(ys))
