"""Write a KiCad 10 .kicad_pcb (and its .kicad_pro) from a Circuit and a Placement."""
import copy
import json
import uuid
from dataclasses import dataclass
from pathlib import Path

from board.placement import rotate
from board.sexpr import Sym, dumps, find, find_all

S = Sym
INSET = 0.5          # plane zones stop this far inside the outline
_NS = uuid.UUID("7c0f1e5a-0000-4000-8000-000000000016")

_USER_LAYERS = [
    [32, "B.Adhes", "user", "B.Adhesive"], [33, "F.Adhes", "user", "F.Adhesive"],
    [34, "B.Paste", "user"], [35, "F.Paste", "user"],
    [36, "B.SilkS", "user", "B.Silkscreen"], [37, "F.SilkS", "user", "F.Silkscreen"],
    [38, "B.Mask", "user"], [39, "F.Mask", "user"],
    [40, "Dwgs.User", "user", "User.Drawings"], [41, "Cmts.User", "user", "User.Comments"],
    [44, "Edge.Cuts", "user"], [45, "Margin", "user"],
    [46, "B.CrtYd", "user", "B.Courtyard"], [47, "F.CrtYd", "user", "F.Courtyard"],
    [48, "B.Fab", "user"], [49, "F.Fab", "user"],
]


def _stackup(cu, mask, silkscreen):
    if not (mask or silkscreen):
        return []
    def col(c):
        return [[S("color"), c]] if c else []
    st = [S("stackup"),
          [S("layer"), "F.SilkS", [S("type"), "Top Silk Screen"]] + col(silkscreen),
          [S("layer"), "F.Paste", [S("type"), "Top Solder Paste"]],
          [S("layer"), "F.Mask", [S("type"), "Top Solder Mask"]] + col(mask) + [[S("thickness"), 0.01]]]
    for k, (_, name, _) in enumerate(cu):
        st.append([S("layer"), name, [S("type"), "copper"], [S("thickness"), 0.035]])
        if k < len(cu) - 1:
            st.append([S("layer"), f"dielectric {k + 1}", [S("type"), "core"],
                       [S("thickness"), round(1.51 / (len(cu) - 1), 4)], [S("material"), "FR4"],
                       [S("epsilon_r"), 4.5], [S("loss_tangent"), 0.02]])
    st += [[S("layer"), "B.Mask", [S("type"), "Bottom Solder Mask"]] + col(mask) + [[S("thickness"), 0.01]],
           [S("layer"), "B.Paste", [S("type"), "Bottom Solder Paste"]],
           [S("layer"), "B.SilkS", [S("type"), "Bottom Silk Screen"]] + col(silkscreen),
           [S("copper_finish"), "ENIG"]]
    return [st]


@dataclass
class Track:
    net: str
    layer: str
    x1: float
    y1: float
    x2: float
    y2: float
    width: float


@dataclass
class Via:
    net: str
    x: float
    y: float
    size: float = 0.6
    drill: float = 0.3


class _Uuids:
    """Deterministic uuids, so regenerating the board gives a byte-identical file."""
    def __init__(self):
        self.n = 0

    def __call__(self):
        self.n += 1
        return [S("uuid"), str(uuid.uuid5(_NS, str(self.n)))]


def pin_marks(circuit, placement, marks, inset=2.4):
    """Silkscreen entries (text, x, y, size) for (ref, pad, text, toward) tuples: the text sits
    'inset' mm from that pad's centre, towards the board point 'toward' (an (x, y) pair), clear
    of the part's own outline. Rotation of the part is applied to the pad position."""
    out = []
    for ref, number, text, toward in marks:
        x, y, rot = placement.at[ref]
        pad = next(p for p in circuit.instances[ref].part.footprint.pads if p.number == number)
        dx, dy = rotate(pad.x, pad.y, rot)
        px, py = x + dx, y + dy
        tx, ty = toward
        d = max(((tx - px) ** 2 + (ty - py) ** 2) ** 0.5, 1e-9)
        out.append((text, px + inset * (tx - px) / d, py + inset * (ty - py) / d, 1.0))
    return out


def _modern_arc(arc):
    """Legacy fp_arc (start = centre, end = first point, angle) -> start/mid/end.
    Legacy angles are clockwise on screen, hence the sign flip against placement.rotate."""
    centre, first = find(arc, "start"), find(arc, "end")
    angle = find(arc, "angle")[1]
    rx, ry = first[1] - centre[1], first[2] - centre[2]
    pt = lambda a: [centre[1] + rotate(rx, ry, a)[0], centre[2] + rotate(rx, ry, a)[1]]
    arc[:] = [v for v in arc if not (isinstance(v, list) and v[0] in ("start", "end", "angle"))]
    arc[1:1] = [[S("start"), first[1], first[2]], [S("mid")] + pt(-angle / 2), [S("end")] + pt(-angle)]


def _footprint(inst, x, y, rot, nets, uid, hide_ref=False):
    fp = copy.deepcopy(inst.part.footprint.node)
    fp[0] = S("footprint")
    fp[1] = str(fp[1]).split(":")[-1]      # the library nickname is not configured in the project
    fp[:] = [v for v in fp if not (isinstance(v, list) and v and v[0] in ("model", "tedit", "at", "uuid"))]
    i = next(k for k, v in enumerate(fp) if isinstance(v, list) and v and v[0] == "layer")
    fp[i + 1:i + 1] = [[S("at"), x, y, rot] if rot else [S("at"), x, y], uid()]
    attr = find(fp, "attr")
    if attr is None:
        attr = [S("attr"), S("through_hole" if inst.part.tht else "smd")]
        fp.insert(i + 3, attr)
    if inst.dnp:
        attr.append(S("dnp"))
    for t in find_all(fp, "fp_text"):
        if t[1] == "reference":
            t[2] = inst.ref
            if hide_ref:
                t.insert(t.index(find(t, "layer")) + 1, S("hide"))
        elif t[1] == "value":
            t[2] = inst.value
        if rot:                                      # text angles are absolute on a board
            at = find(t, "at")
            at[:] = at[:3] + [(at[3] if len(at) > 3 else 0) + rot]
    for arc in find_all(fp, "fp_arc"):
        if find(arc, "angle") is not None:
            _modern_arc(arc)
    for pad in find_all(fp, "pad"):
        if rot:                                      # pad angles are absolute on a board
            at = find(pad, "at")
            at[:] = at[:3] + [(at[3] if len(at) > 3 else 0) + rot]
        net = nets.get(str(pad[1]))
        if net is not None:
            pad.append([S("net"), net[0], net[1]])
        pad.append(uid())
    return fp


SILK_EDGE = 0.2      # footprint silk lines are pulled this far inside the board outline


def _clip_silk(fp, x, y, rot, outline):
    """Pull footprint silk line ends that reach the board edge (the module's outline lies on
    it) back inside, so that nothing is printed over the edge."""
    x0, y0, x1, y1 = (outline[0] + SILK_EDGE, outline[1] + SILK_EDGE,
                      outline[2] - SILK_EDGE, outline[3] - SILK_EDGE)
    for g in find_all(fp, "fp_line"):
        if "SilkS" not in str(find(g, "layer")[1]):
            continue
        for key in ("start", "end"):
            pt = find(g, key)
            bx, by = rotate(pt[1], pt[2], rot)
            cx, cy = min(max(x + bx, x0), x1), min(max(y + by, y0), y1)
            lx, ly = rotate(cx - x, cy - y, -rot)
            pt[1:3] = [round(lx, 4), round(ly, 4)]


def _zone(net_id, name, layer, outline, uid):
    x0, y0, x1, y1 = outline
    x0, y0, x1, y1 = x0 + INSET, y0 + INSET, x1 - INSET, y1 - INSET
    return [S("zone"), [S("net"), net_id], [S("net_name"), name], [S("layer"), layer], uid(),
            [S("hatch"), S("edge"), 0.5],
            [S("connect_pads"), [S("clearance"), 0.2]],
            [S("min_thickness"), 0.2],
            [S("fill"), S("yes"), [S("thermal_gap"), 0.3], [S("thermal_bridge_width"), 0.4]],
            [S("polygon"), [S("pts"), [S("xy"), x0, y0], [S("xy"), x1, y0],
                            [S("xy"), x1, y1], [S("xy"), x0, y1]]]]


def _text(text, x, y, size, uid, layer="F.SilkS", rot=0):
    """Silk text; on B.SilkS it is mirrored so that it reads correctly from the back."""
    eff = [S("effects"), [S("font"), [S("size"), size, size], [S("thickness"), max(0.15, round(size * 0.15, 3))]]]
    if layer.startswith("B."):
        eff.append([S("justify"), S("mirror")])
    at = [S("at"), x, y, rot] if rot else [S("at"), x, y]
    return [S("gr_text"), text, at, [S("layer"), layer], uid(), eff]


def _keepout(name, pts, layers, uid):
    """A rule area: no tracks, vias, pads or copper pour on 'layers' inside polygon 'pts'."""
    return [S("zone"), [S("net"), 0], [S("net_name"), ""], [S("layers")] + list(layers), uid(),
            [S("name"), name], [S("hatch"), S("edge"), 0.5], [S("connect_pads"), [S("clearance"), 0]],
            [S("min_thickness"), 0.25],
            [S("keepout"), [S("tracks"), S("not_allowed")], [S("vias"), S("not_allowed")],
             [S("pads"), S("not_allowed")], [S("copperpour"), S("not_allowed")],
             [S("footprints"), S("allowed")]],
            [S("fill"), [S("thermal_gap"), 0.5], [S("thermal_bridge_width"), 0.5]],
            [S("polygon"), [S("pts")] + [[S("xy"), x, y] for x, y in pts]]]


PRO = {
    "meta": {"filename": "board.kicad_pro", "version": 1},
    "board": {"design_settings": {"rules": {
        "min_clearance": 0.15, "min_track_width": 0.15, "min_via_diameter": 0.5,
        "min_through_hole_diameter": 0.3, "min_copper_edge_clearance": 0.3}}},
    "net_settings": {
        "meta": {"version": 3},      # without this KiCad ignores the classes and uses 0.2 mm
        "classes": [
            {"name": "Default", "clearance": 0.15, "track_width": 0.15, "via_diameter": 0.6, "via_drill": 0.3},
            {"name": "Power", "clearance": 0.2, "track_width": 0.3, "via_diameter": 0.6, "via_drill": 0.3}],
        "netclass_patterns": [{"netclass": "Power", "pattern": p} for p in ("+*", "GND", "VBUS")]},
}


def write_board(circuit, placement, path, tracks=(), vias=(), layers=4, silk=(), hide_refs=(), art=(),
                mask=None, silkscreen=None, keepouts=()):
    """Write 'path' (.kicad_pcb) and a .kicad_pro beside it.

    layers=4: F.Cu, In1.Cu (GND zone), In2.Cu (+1V5 zone, only if that net exists), B.Cu.
    layers=2: F.Cu and B.Cu, each with a GND zone.
    silk: (text, x, y, size_mm[, layer[, rotation]]) entries; layer defaults to F.SilkS.
    hide_refs: refs whose reference text is not drawn.
    art: (png_path, x, y, height_mm[, side]) portraits, top-left at (x, y) in board (top-view)
    coordinates; side "back" puts it on B.SilkS, mirrored to read correctly from the back.
    keepouts: (name, [(x, y), ...], layers) rule areas with no copper at all.
    mask, silkscreen: colour names for the stackup, which only affects renders (gerbers carry
    no colour). None for both keeps KiCad's defaults (green mask, white silk)."""
    if layers not in (2, 4):
        raise ValueError("layers must be 2 or 4")
    path = Path(path)
    uid = _Uuids()
    names = sorted(circuit.nets)
    ids = {n: i + 1 for i, n in enumerate(names)}
    cu = [[0, "F.Cu", "signal"]]
    if layers == 4:
        cu += [[1, "In1.Cu", "power"], [2, "In2.Cu", "power"]]
    cu.append([31, "B.Cu", "signal"])
    tree = [S("kicad_pcb"), [S("version"), 20240108], [S("generator"), "tinycrypt_key_board"],
            [S("generator_version"), "8.0"],
            [S("general"), [S("thickness"), 1.6]],
            [S("paper"), "A4"],
            [S("layers")] + [[n, name, S(kind), *alias] for n, name, kind, *alias in cu + _USER_LAYERS],
            [S("setup")] + _stackup(cu, mask, silkscreen) + [[S("pad_to_mask_clearance"), 0]],
            [S("net"), 0, ""]] + [[S("net"), ids[n], n] for n in names]
    x0, y0, x1, y1 = placement.outline
    tree.append([S("gr_rect"), [S("start"), x0, y0], [S("end"), x1, y1],
                 [S("stroke"), [S("width"), 0.1], [S("type"), S("default")]], [S("fill"), S("none")],
                 [S("layer"), "Edge.Cuts"], uid()])
    for ref, inst in circuit.instances.items():
        x, y, rot = placement.at[ref]
        nets = {pad: (ids[n], n) for n in names for (r, pad) in circuit.nets[n] if r == ref}
        fp = _footprint(inst, x, y, rot, nets, uid, ref in hide_refs)
        _clip_silk(fp, x, y, rot, placement.outline)
        tree.append(fp)
    plane = [("GND", "In1.Cu")] if layers == 4 else [("GND", "F.Cu"), ("GND", "B.Cu")]
    if layers == 4:
        plane.append(("+1V5", "In2.Cu"))
    for net, layer in plane:
        if net in ids:
            tree.append(_zone(ids[net], net, layer, placement.outline, uid))
    for name, pts, kl in keepouts:
        tree.append(_keepout(name, pts, kl, uid))
    for t in tracks:
        tree.append([S("segment"), [S("start"), t.x1, t.y1], [S("end"), t.x2, t.y2], [S("width"), t.width],
                     [S("layer"), t.layer], [S("net"), ids[t.net]], uid()])
    for v in vias:
        tree.append([S("via"), [S("at"), v.x, v.y], [S("size"), v.size], [S("drill"), v.drill],
                     [S("layers"), "F.Cu", "B.Cu"], [S("net"), ids[v.net]], uid()])
    if art:
        from board.art import art_footprint
        for k, a in enumerate(art):
            tree.append(art_footprint(*a[:4], name=f"ART{k + 1}", side=a[4] if len(a) > 4 else "front"))
    for entry in silk:
        tree.append(_text(*entry[:4], uid, *entry[4:]))
    path.write_text(dumps(tree).replace(") (", ")\n (") + "\n")
    pro = json.loads(json.dumps(PRO))
    pro["meta"]["filename"] = path.with_suffix(".kicad_pro").name
    path.with_suffix(".kicad_pro").write_text(json.dumps(pro, indent=2) + "\n")
