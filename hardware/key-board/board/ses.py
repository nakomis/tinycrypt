"""Read a Specctra SES (Freerouting's output) back into Tracks and Vias."""
from pathlib import Path

from board.kicad_pcb import Track, Via
from board.sexpr import find, find_all, parse


def read_ses(path, min_width=0.0, report=None):
    """Tracks and vias from the SES. Freerouting 2.5 narrows some runs (0.09-0.112 mm, up to
    ~11.7 mm long, mostly beside the 2.54 mm header pads) below the class width; 'min_width'
    lifts any such track to the design-rule minimum (DRC then confirms that clearances still
    hold). If 'report' is a dict it receives 'widened' (segments) and 'widened_mm' (length)."""
    # '(string_quote ")' would open a string in our reader; the clause is not needed here.
    tree = parse(Path(path).read_text().replace('(string_quote ")', "(string_quote)"))
    routes = find(tree, "routes")
    res = find(routes, "resolution")
    scale = res[2] * 1000.0 if res[1] == "um" else None      # SES units per mm
    if scale is None:
        raise ValueError(f"unsupported SES unit {res[1]}")
    tracks, vias = [], []
    widened, widened_mm = 0, 0.0
    for net in find_all(find(routes, "network_out"), "net"):
        name = str(net[1])
        for wire in find_all(net, "wire"):
            for p in find_all(wire, "path"):
                layer, width, pts = str(p[1]), p[2] / scale, p[3:]
                xy = [(pts[i] / scale, -pts[i + 1] / scale) for i in range(0, len(pts), 2)]
                for (ax, ay), (bx, by) in zip(xy, xy[1:]):
                    if width < min_width - 1e-9:
                        widened += 1
                        widened_mm += ((ax - bx) ** 2 + (ay - by) ** 2) ** 0.5
                    tracks.append(Track(name, layer, ax, ay, bx, by, round(max(width, min_width), 4)))
        for v in find_all(net, "via"):
            vias.append(Via(name, v[2] / scale, -v[3] / scale))
    if report is not None:
        report.update(widened=widened, widened_mm=widened_mm)
    return tracks, vias
