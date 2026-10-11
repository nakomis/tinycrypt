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


def _hand():
    t, v = [], []
    for f in (kb.usb_hand_route, kb.power_hand_route, kb.signal_hand_route):
        a, b = f()
        t += a
        v += b
    return t, v + kb.edge_vias()


def _dsn(tmp_path):
    c = kb.build_key()
    hand_t, hand_v = _hand()
    write_dsn(c, kb.place_key(c), tmp_path / "a.dsn", layers=2, route_area=kb.route_area(),
              fixed_vias=hand_v, fixed_tracks=hand_t, via_keepouts=kb.back_text_boxes(),
              layer_keepouts=kb.layer_keepouts(), skip_nets=("GND",))
    return c, (tmp_path / "a.dsn").read_text()


def test_dsn_two_layer(tmp_path):
    c, text = _dsn(tmp_path)
    assert "(plane" not in text and "In1.Cu" not in text
    assert "(layer F.Cu (type signal))" in text and "(layer B.Cu (type signal))" in text
    assert text.count("(net ") >= len(c.nets)
    assert "(class gnd GND" not in text and '(class power "+3V3" VBUS' in text
    gnd = next(l for l in text.splitlines() if l.strip().startswith("(net GND"))
    assert gnd.count("-") == 1                    # GND is left to the pours: one pin, nothing to route


def test_dsn_boundary_is_the_board(tmp_path):
    _, text = _dsn(tmp_path)
    line = next(l for l in text.splitlines() if "(boundary" in l)
    nums = [int(v) for v in line.replace(")", " ").split()[4:]]
    ys = [-y / 1000 for y in nums[1::2]]
    assert abs(min(ys)) < 1e-6 and abs(max(ys) - kb.L) < 1e-6


def test_dsn_repeated_pad_numbers_get_unique_pin_ids(tmp_path):
    c = kb.build_key()
    write_dsn(c, kb.place_key(c), tmp_path / "b.dsn", layers=2)
    text = (tmp_path / "b.dsn").read_text()
    img = next(l for l in text.splitlines() if "(image U1_img" in l)
    assert img.count(' GND ') == 1 and ' GND_13 ' in img and ' GND_14 ' not in img
    gnd = next(l for l in text.splitlines() if l.strip().startswith("(net GND"))
    assert "U1-GND_13" in gnd and "U1-GND " in gnd and "U1-46" in gnd


def test_dsn_carries_hand_route_and_keepouts(tmp_path):
    _, text = _dsn(tmp_path)
    t, v = _hand()
    assert text.count("(type protect)") == len(t)
    assert text.count("(type fix)") == len(v)
    assert text.count("(via_keepout") == 2 * len(kb.back_text_boxes())
    assert text.count("(keepout (rect B.Cu") == 2              # under the USB pair and Mu's face


def test_full_route_is_drc_clean():
    """Integration: run build.py --board key --route and check its outputs (slow)."""
    subprocess.run(["./.venv/bin/python", "build.py", "--board", "key", "--route"], check=True, cwd=ROOT,
                   capture_output=True)
    out = ROOT / "out" / "key"
    drc = json.loads((out / "drc.json").read_text())
    assert drc["unconnected_items"] == []
    assert [v for v in drc["violations"] if v["severity"] == "error"] == []
    tree = parse((out / "tinycrypt-key.kicad_pcb").read_text())
    # every via clear of Mu's white fur and of all silk text; B.Cu under the USB pair holds only
    # the one VBUS hop
    vias = [find(v, "at")[1:3] for v in find_all(tree, "via")]
    assert not [v for v in vias if kb.on_art(*v, margin=0.3)]
    x0, y0, x1, y1 = kb.USB_ZONE
    under = [s for s in find_all(tree, "segment") if find(s, "layer")[1] == "B.Cu"
             and any(x0 < find(s, k)[1] < x1 and y0 < find(s, k)[2] < y1 for k in ("start", "end"))]
    assert len(under) <= 1
    for name in ("bom.csv", "cpl.csv", "pins.h", "tinycrypt-key-gerbers.zip",
                 "tinycrypt-key-top.png", "tinycrypt-key-bottom.png"):
        assert (out / name).exists(), name
    assert not kb.vias_under_text([type("V", (), {"x": x, "y": y, "size": 0.6}) for x, y in vias])


def test_read_ses_reports_widened_segments(tmp_path):
    ses = tmp_path / "n.ses"
    ses.write_text('(session n (routes (resolution um 1) (network_out (net A (wire (path F.Cu 100 0 0 1000 0))))))')
    rep = {}
    tracks, _ = read_ses(ses, min_width=0.15, report=rep)
    assert tracks[0].width == 0.15 and rep == {"widened": 1, "widened_mm": 1.0}
