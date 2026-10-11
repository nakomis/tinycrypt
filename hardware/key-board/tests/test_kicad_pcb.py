import json
import subprocess

import pytest
from conftest import KICAD_CLI

from board import key_board as kb
from board.kicad_pcb import Track, Via, write_board
from board.netlist import Circuit
from board.placement import Placement
from board.sexpr import find, find_all, parse

BAD = ("courtyards_overlap", "malformed_courtyard", "invalid_outline", "lib_footprint_issues")


def _drc(path, out):
    r = subprocess.run([KICAD_CLI, "pcb", "drc", "--format", "json", "--severity-all",
                        "-o", str(out), str(path)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return json.loads(out.read_text())


def _key(tmp_path, **kw):
    c = kb.build_key()
    pl = kb.place_key(c)
    pcb = tmp_path / "key.kicad_pcb"
    write_board(c, pl, pcb, layers=2, silk=kb.silk_key(c, pl), art=kb.art_key(pl),
                keepouts=kb.keepouts(), **kw)
    return c, pl, pcb


def test_kicad_parses_unrouted_key_board(tmp_path):
    _, _, pcb = _key(tmp_path)
    drc = _drc(pcb, tmp_path / "drc.json")
    assert drc["unconnected_items"], "an unrouted board must report unconnected items"
    assert [v for v in drc["violations"] if v["type"] in BAD] == []


def test_two_layer_gnd_pours_and_no_rule_area(tmp_path):
    _, _, pcb = _key(tmp_path)
    tree = parse(pcb.read_text())
    cu = [l[1] for l in find(tree, "layers")[1:] if l[2] in ("signal", "power")]
    assert cu == ["F.Cu", "B.Cu"]
    zones = find_all(tree, "zone")
    assert [(find(z, "net_name")[1], find(z, "layer")[1]) for z in zones] == [("GND", "F.Cu"), ("GND", "B.Cu")]


def test_keepout_rule_area_written(tmp_path):
    c = kb.build_key()
    pl = kb.place_key(c)
    pcb = tmp_path / "k.kicad_pcb"
    write_board(c, pl, pcb, layers=2, keepouts=[("band", [(0, 0), (21, 0), (21, 1), (0, 1)], ("F.Cu", "B.Cu"))])
    keep = [z for z in find_all(parse(pcb.read_text()), "zone") if find(z, "keepout") is not None]
    assert len(keep) == 1 and find(keep[0], "layers")[1:] == ["F.Cu", "B.Cu"]
    rules = {r[0]: r[1] for r in find(keep[0], "keepout")[1:]}
    assert rules == {"tracks": "not_allowed", "vias": "not_allowed", "pads": "not_allowed",
                     "copperpour": "not_allowed", "footprints": "allowed"}


def test_pad_extras_and_fab_note_written(tmp_path):
    notes, rects = kb.fab_notes()
    _, _, pcb = _key(tmp_path, pad_extra=kb.pad_extra(), user_rects=rects)
    text = pcb.read_text()
    assert text.count("(solder_paste_margin -0.18)") == 9 and text.count("(zone_connect 2)") == 5
    tree = parse(text)
    r = [g for g in find_all(tree, "gr_rect") if find(g, "layer")[1] == "Dwgs.User"]
    assert len(r) == 1 and abs(find(r[0], "start")[2] - kb.ANTENNA) < 1e-9
    assert "INTENTIONAL" in notes[0][0] and notes[0][4] == "Dwgs.User"


def test_bad_layer_count(tmp_path):
    c = kb.build_key()
    with pytest.raises(ValueError):
        write_board(c, kb.place_key(c), tmp_path / "x.kicad_pcb", layers=3)


def test_pro_file_read_by_drc(tmp_path):
    """The 0.15 mm default clearance in .kicad_pro must reach DRC (net_settings.meta.version 3):
    two pads 0.1 mm apart violate it, and the report states the 0.15 mm limit."""
    c = Circuit()
    a = c.add("C100N", "C1")
    b = c.add("C100N", "C2")
    c.connect("A", (a, "1"), (b, "1"))
    c.connect("B", (a, "2"), (b, "2"))
    pl = Placement((0.0, 0.0, 10.0, 10.0), {"C1": (3.0, 5.0, 0.0), "C2": (4.44, 5.0, 0.0)})
    pcb = tmp_path / "x.kicad_pcb"
    write_board(c, pl, pcb, layers=2)
    pro = json.loads((tmp_path / "x.kicad_pro").read_text())
    assert pro["net_settings"]["meta"]["version"] == 3
    assert pro["board"]["design_settings"]["rules"]["min_clearance"] == 0.15
    drc = _drc(pcb, tmp_path / "drc.json")
    clr = [v for v in drc["violations"] if v["type"] == "clearance"]
    assert clr and "0.1500 mm" in clr[0]["description"]


def test_dnp_flag_on_synthetic_dnp(tmp_path):
    c = Circuit()
    a = c.add("C100N", "C1", dnp=True)
    b = c.add("C100N", "C2")
    c.connect("A", (a, "1"), (b, "1"))
    pl = Placement((0.0, 0.0, 10.0, 10.0), {"C1": (3.0, 5.0, 0.0), "C2": (7.0, 5.0, 0.0)})
    pcb = tmp_path / "x.kicad_pcb"
    write_board(c, pl, pcb, layers=2)
    attrs = [find(f, "attr") for f in find_all(parse(pcb.read_text()), "footprint")]
    assert sum(1 for a in attrs if "dnp" in a) == 1


def test_tracks_and_vias_written(tmp_path):
    _, _, pcb = _key(tmp_path, tracks=[Track("GND", "F.Cu", 5, 30, 8, 30, 0.3)], vias=[Via("GND", 9, 30)])
    tree = parse(pcb.read_text())
    assert len(find_all(tree, "segment")) == 1 and len(find_all(tree, "via")) == 1


def test_back_text_is_mirrored_and_front_text_is_not(tmp_path):
    _, _, pcb = _key(tmp_path)
    texts = find_all(parse(pcb.read_text()), "gr_text")
    back = [t for t in texts if find(t, "layer")[1] == "B.SilkS"]
    front = [t for t in texts if find(t, "layer")[1] == "F.SilkS"]
    assert {t[1] for t in back} == {"Mu", kb.CREDIT, kb.TITLE}
    assert all(find(find(t, "effects"), "justify") == ["justify", "mirror"] for t in back)
    assert all(find(find(t, "effects"), "justify") is None for t in front)
    assert {"V", "D-", "D+", "CC", "G", "EN", "RX", "TX", "3V3", "BTN"} <= {t[1] for t in front}


def test_module_silk_pulled_inside_the_edge(tmp_path):
    _, _, pcb = _key(tmp_path)
    tree = parse(pcb.read_text())
    u1 = next(f for f in find_all(tree, "footprint") if find(f, "at")[1:3] == [kb.CX, kb.MOD_Y])
    for g in find_all(u1, "fp_line"):
        if "SilkS" in find(g, "layer")[1]:
            for key in ("start", "end"):
                assert kb.MOD_Y + find(g, key)[2] >= 0.2 - 1e-9


def test_every_assembled_footprint_has_a_3d_model(tmp_path):
    """Every non-DNP part JLCPCB places carries a (model ...) pointing at a STEP file in
    footprints/lcsc.3dshapes (through ${KIPRJMOD}); board-only footprints (test pads, holes, art)
    have none. If the models have been fetched, each named file exists."""
    import os
    from board.parts import LCSC_3D
    c, pl, pcb = _key(tmp_path)
    tree = parse(pcb.read_text())
    refs = {}
    for fp in find_all(tree, "footprint"):
        ref = next((p[2] for p in find_all(fp, "property") if p[1] == "Reference"), None)
        ref = ref or next((t[2] for t in find_all(fp, "fp_text") if t[1] == "reference"), None)
        refs[ref] = find_all(fp, "model")
    fitted = [r for r, i in c.instances.items() if i.part.lcsc and not i.dnp]
    assert fitted
    for r in fitted:
        models = refs[r]
        assert len(models) == 1, r
        path = models[0][1]
        assert path.startswith("${KIPRJMOD}/") and path.endswith(".step"), (r, path)
        local = (tmp_path / path.replace("${KIPRJMOD}/", "")).resolve()
        assert local.parent == LCSC_3D.resolve(), (r, local)
        if LCSC_3D.is_dir() and any(LCSC_3D.glob("*.step")):
            assert local.exists(), (r, local.name)
    for r in ("TP1", "H1", "H2"):
        assert refs[r] == [], r


def test_model_offset_corrections_written(tmp_path):
    from board.parts import MODEL_OFFSET
    c, pl, pcb = _key(tmp_path)
    tree = parse(pcb.read_text())
    for fp in find_all(tree, "footprint"):
        ref = next((t[2] for t in find_all(fp, "fp_text") if t[1] == "reference"), None)
        if ref in c.instances and c.instances[ref].part.key in MODEL_OFFSET:
            got = find(find(find(fp, "model"), "offset"), "xyz")[1:]
            assert got == list(MODEL_OFFSET[c.instances[ref].part.key]), ref
