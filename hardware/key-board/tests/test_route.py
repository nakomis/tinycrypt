import json
import subprocess
from pathlib import Path

from board import key_board as kb
from board.dsn import write_dsn
from board.ses import read_ses
from board.sexpr import find, find_all, parse

DATA = Path(__file__).parent / "data"
ROOT = Path(__file__).resolve().parent.parent


def test_read_ses_converts_units_and_flips_y():
    tracks, vias = read_ses(DATA / "mini.ses")
    assert len(tracks) == 2 and len(vias) == 1
    t = tracks[0]
    assert (t.net, t.layer, t.width) == ("GND", "F.Cu", 0.15)
    assert (t.x1, t.y1, t.x2, t.y2) == (100.0, 100.0, 110.0, 100.0)
    assert (vias[0].x, vias[0].y) == (110.0, 105.0)


def _dsn(tmp_path):
    c = kb.build_key()
    hand_t, hand_v = kb.usb_hand_route()
    write_dsn(c, kb.place_key(c), tmp_path / "a.dsn", layers=2, route_area=kb.route_area(),
              fixed_vias=kb.thermal_vias() + hand_v, fixed_tracks=hand_t, via_keepouts=kb.back_text_boxes())
    return c, (tmp_path / "a.dsn").read_text()


def test_dsn_two_layer(tmp_path):
    c, text = _dsn(tmp_path)
    assert "(plane" not in text and "In1.Cu" not in text
    assert "(layer F.Cu (type signal))" in text and "(layer B.Cu (type signal))" in text
    assert text.count("(net ") >= len(c.nets)
    assert "(class gnd GND" in text and '(class power "+3V3" VBUS' in text


def test_dsn_boundary_excludes_the_antenna(tmp_path):
    _, text = _dsn(tmp_path)
    line = next(l for l in text.splitlines() if "(boundary" in l)
    nums = [int(v) for v in line.replace(")", " ").split()[4:]]
    ys = [-y / 1000 for y in nums[1::2]]
    assert abs(min(ys) - kb.ANT_Y) < 1e-6 and abs(max(ys) - kb.L) < 1e-6


def test_dsn_repeated_pad_numbers_get_unique_pin_ids(tmp_path):
    _, text = _dsn(tmp_path)
    img = next(l for l in text.splitlines() if "(image U1_img" in l)
    assert img.count(' GND ') == 1 and ' GND_13 ' in img and ' GND_14 ' not in img
    gnd = next(l for l in text.splitlines() if l.strip().startswith("(net GND"))
    assert "U1-GND_13" in gnd and "U1-GND " in gnd and "U1-46" in gnd


def test_dsn_carries_hand_route_and_keepouts(tmp_path):
    _, text = _dsn(tmp_path)
    assert text.count("(type protect)") == len(kb.usb_hand_route()[0])
    assert text.count("(type fix)") == 6                   # 4 thermal + VBUS + GND
    assert text.count("(via_keepout") == 2 * len(kb.back_text_boxes())


def test_full_route_is_drc_clean():
    """Integration: run build.py --board key --route and check its outputs (slow)."""
    subprocess.run(["./.venv/bin/python", "build.py", "--board", "key", "--route"], check=True, cwd=ROOT,
                   capture_output=True)
    out = ROOT / "out" / "key"
    drc = json.loads((out / "drc.json").read_text())
    assert drc["unconnected_items"] == []
    assert [v for v in drc["violations"] if v["severity"] == "error"] == []
    tree = parse((out / "tinycrypt-key.kicad_pcb").read_text())
    # no track or via in the antenna keep-out, and no filled copper there either
    for s in find_all(tree, "segment"):
        assert find(s, "start")[2] >= kb.ANT_Y and find(s, "end")[2] >= kb.ANT_Y
    for v in find_all(tree, "via"):
        assert find(v, "at")[2] - 0.3 >= kb.ANT_Y
    for z in find_all(tree, "zone"):
        for fp in find_all(z, "filled_polygon"):
            assert min(xy[2] for xy in find_all(find(fp, "pts"), "xy")) >= kb.ANT_Y - 1e-6
    for name in ("bom.csv", "cpl.csv", "pins.h", "tinycrypt-key-gerbers.zip",
                 "tinycrypt-key-top.png", "tinycrypt-key-bottom.png"):
        assert (out / name).exists(), name
    assert not kb.vias_under_text([type("V", (), {"x": find(v, "at")[1], "y": find(v, "at")[2], "size": 0.6})
                                   for v in find_all(tree, "via")])


def test_read_ses_reports_widened_segments(tmp_path):
    ses = tmp_path / "n.ses"
    ses.write_text('(session n (routes (resolution um 1) (network_out (net A (wire (path F.Cu 100 0 0 1000 0))))))')
    rep = {}
    tracks, _ = read_ses(ses, min_width=0.15, report=rep)
    assert tracks[0].width == 0.15 and rep == {"widened": 1, "widened_mm": 1.0}
