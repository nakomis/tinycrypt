"""Post-route cleanup: drop vias that join only one layer and the track stubs that lead to
nothing (Freerouting leaves a few of both when it rips up and reroutes)."""
from board.placement import rotate

EPS = 0.01


def pad_shapes(circuit, placement):
    """(net, layers, xmin, ymin, xmax, ymax) for every copper pad, in board coordinates."""
    out = []
    for ref, inst in circuit.instances.items():
        x, y, rot = placement.at[ref]
        for pad in inst.part.footprint.pads:
            dx, dy = rotate(pad.x, pad.y, rot)
            w, h = (pad.h, pad.w) if round(pad.rot + rot) % 180 == 90 else (pad.w, pad.h)
            layers = {"F.Cu", "B.Cu"} if pad.kind == "thru_hole" else {"F.Cu"}
            out.append((circuit.net_of(ref, pad.number), layers, x + dx - w / 2, y + dy - h / 2,
                        x + dx + w / 2, y + dy + h / 2))
    return out


def _on_pad(net, layer, x, y, pads):
    return any(n == net and layer in ls and x0 - EPS <= x <= x1 + EPS and y0 - EPS <= y <= y1 + EPS
               for n, ls, x0, y0, x1, y1 in pads)


def _same(a, b):
    return abs(a[0] - b[0]) < EPS and abs(a[1] - b[1]) < EPS


def _on_segment(p, t):
    (px, py), (ax, ay), (bx, by) = p, (t.x1, t.y1), (t.x2, t.y2)
    vx, vy = bx - ax, by - ay
    k = 0.0 if vx == vy == 0 else max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)))
    return ((px - ax - k * vx) ** 2 + (py - ay - k * vy) ** 2) ** 0.5 < EPS


def prune(circuit, placement, tracks, vias, keep=(), pour_nets=("GND",)):
    """Return (tracks, vias) without one-layer vias and dangling track ends. Vias in 'keep'
    (hand-placed ones) and vias on 'pour_nets' (which join a copper pour on the other layer)
    are never removed."""
    pads = pad_shapes(circuit, placement)
    keep_xy = [(v.x, v.y) for v in keep]
    tracks, vias = list(tracks), list(vias)
    changed = True
    while changed:
        changed = False
        for v in list(vias):
            if v.net in pour_nets or any(_same((v.x, v.y), k) for k in keep_xy):
                continue
            layers = {t.layer for t in tracks if t.net == v.net and _on_segment((v.x, v.y), t)}
            layers |= {l for l in ("F.Cu", "B.Cu") if _on_pad(v.net, l, v.x, v.y, pads)}
            if len(layers) < 2:
                vias.remove(v)
                changed = True
        for t in list(tracks):
            for end in ((t.x1, t.y1), (t.x2, t.y2)):
                joined = _on_pad(t.net, t.layer, *end, pads)
                joined = joined or any(v.net == t.net and _same((v.x, v.y), end) for v in vias)
                joined = joined or any(o is not t and o.net == t.net and o.layer == t.layer and
                                       _on_segment(end, o) for o in tracks)
                if not joined:
                    tracks.remove(t)
                    changed = True
                    break
    return tracks, vias
