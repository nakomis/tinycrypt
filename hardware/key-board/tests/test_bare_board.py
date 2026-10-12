"""The bare-chip board (CRYPT-24): ESP32-S3FN8 + crystal + RF match + chip antenna."""
import json
import math
import re
import subprocess

from conftest import ROOT

from board import bare_board as bb
from board.kicad_pcb import Track, Via
from board.netlist import check
from board.placement import bbox, rotate
from board.sexpr import find, find_all, parse

C = bb.build_key()
P = bb.place_key(C)

# Every LCSC part on the board, checked on its JLCPCB page on 12 Oct 2026 as "Economic and
# Standard" (the point of this board). A new part must be checked and added here.
ECONOMIC = {"C2913196", "C706805", "C74301", "C18216", "C97997", "C1552", "C1548", "C15195",
            "C2917718", "C965840", "C25076", "C1525", "C52923", "C19702", "C25905", "C25744",
            "C25900", "C25104", "C1518769", "C265419", "C7519", "C51118", "C318884"}
STRAPS = {3, 45, 46}                 # GPIO0 is the button, on purpose
FLASH_PINS = {"SPIHD", "SPIWP", "SPICS0", "SPICLK", "SPIQ", "SPID", "SPICS1"}


def net(ref, pad):
    return C.net_of(ref, pad)


def pin_net(name):
    pads = C.instances["U1"].part.pins[name]
    nets = {net("U1", p) for p in pads}
    assert len(nets) == 1, (name, nets)
    return nets.pop()


def test_clean():
    assert check(C) == []


def test_every_part_is_economic_eligible():
    fitted = {i.part.lcsc for i in C.instances.values() if i.part.lcsc and not i.dnp}
    assert fitted <= ECONOMIC, fitted - ECONOMIC
    assert "C2913206" not in fitted and "C52917434" not in fitted     # the module, the WS2812B-2020


def test_usb_on_gpio19_and_gpio20():
    assert pin_net("GPIO19") == "USB_DM" and pin_net("GPIO20") == "USB_DP"
    assert net("J1", "2") == net("U3", "1") == net("U3", "6") == "USB_DM"
    assert net("J1", "3") == net("U3", "3") == net("U3", "4") == "USB_DP"


def test_supplies_after_the_mini_1_reference():
    """ESP32-S3-MINI-1 datasheet v1.7, figure 8-1."""
    assert pin_net("VDD3P3") == "VDD3P3"
    l1 = C.instances["L1"]
    assert l1.value == "2.0nH" and {net("L1", "1"), net("L1", "2")} == {"+3V3", "VDD3P3"}
    assert net("C9", "1") == "VDD3P3" and C.instances["C9"].value == "100nF"
    before = {C.instances[r].value for r in ("C4", "C5", "C8") if net(r, "1") == "+3V3"}
    assert before == {"10uF", "1uF", "100nF"}
    for name in ("VDDA", "VDD3P3_CPU", "VDD3P3_RTC"):
        assert pin_net(name) == "+3V3", name
    assert C.instances["C12"].value == "10nF" and net("C12", "1") == "+3V3"
    # VDD_SPI is the in-package flash's supply, made inside the chip: decoupled and nothing else
    assert pin_net("VDD_SPI") == "VDD_SPI"
    assert {r for r, p in C.nets["VDD_SPI"]} == {"U1", "C13", "C14"}
    assert pin_net("EP") == "GND"


def test_chip_pu_rc():
    assert pin_net("CHIP_PU") == "EN"
    assert C.instances["R2"].value == "10k" and C.instances["C6"].value == "1uF"
    assert net("R2", "1") == net("C6", "1") == "EN" and net("R2", "2") == "+3V3"
    assert net("TP1", "1") == "EN"


def test_crystal():
    assert C.instances["Y1"].value.startswith("40MHz")
    assert pin_net("XTAL_P") == "XTAL_P" and net("L3", "1") == "XTAL_P"
    assert C.instances["L3"].value == "24nH" and net("L3", "2") == net("Y1", "1") == "XTAL_PL"
    assert pin_net("XTAL_N") == net("Y1", "3") == "XTAL_N"
    assert net("Y1", "2") == net("Y1", "4") == "GND"
    assert {net("C18", "1"), net("C19", "1")} == {"XTAL_PL", "XTAL_N"}
    assert C.instances["C18"].value == C.instances["C19"].value == "15pF"


def test_rf_chain():
    """LNA_IN -> C16 shunt -> L2 series -> C17 shunt -> antenna pad 4 (fed from the right)."""
    assert pin_net("LNA_IN") == "LNA_IN" == net("C16", "1") == net("L2", "1")
    assert net("L2", "2") == net("C17", "1") == net("AE1", "4") == "RF_ANT"
    assert net("C16", "2") == net("C17", "2") == "GND"
    assert {C.instances[r].value for r in ("C16", "C17")} == {"1.5pF"} and C.instances["L2"].value == "2.4nH"
    assert [net("AE1", n) for n in "123"] == ["GND"] * 3
    assert {r for r, p in C.nets["RF_ANT"]} == {"L2", "C17", "AE1"}


def test_antenna_on_the_edge_fed_from_the_right():
    feed, gnd1 = bb.pad_xy("AE1", "4"), bb.pad_xy("AE1", "1")
    assert feed[0] > bb.pad_xy("AE1", "3")[0]                # pad 4 on the right
    assert feed[1] > gnd1[1]                                 # and the lower of the right pair
    top = min(bb.pad_xy("AE1", n)[1] for n in "1234") - 0.3
    assert abs(top - 0.3) < 1e-6                             # its outer pads 0.3 mm from the edge


def test_antenna_clearance_is_4_by_6_and_nothing_routes_through_it():
    x0, y0, x1, y1 = bb.ANT_CLEAR
    assert x1 - x0 == 4.0 and y0 == 0.0 and y1 >= 6.0
    (name, pts, layers), = bb.keepouts()
    assert name == "ANTENNA" and layers == ("F.Cu", "B.Cu") and len(pts) == 20
    hand = []
    for f in (bb.usb_hand_route, bb.power_hand_route, bb.signal_hand_route):
        hand.append(f())
    assert bb.clearance_violations([t for h in hand for t in h[0]], [v for h in hand for v in h[1]] + bb.edge_vias()) == []
    # the check catches what KiCad's DRC let through (router tracks across the clearance)
    stray = Track("GND", "F.Cu", 2.5, 4.0, 4.0, 4.0, 0.15)
    assert bb.clearance_violations([stray], []) == [stray]
    assert bb.clearance_violations([], [Via("GND", 3.0, 3.0)]) != []


def test_moat_runs_between_the_pad_and_the_pin_row():
    """3V3 under the chip: 0.2 mm or more from the exposed pad and from the pins' inner ends."""
    cx, cy = bb.U1_AT
    tracks, _ = bb.power_hand_route()
    inner = 3.41 - 0.33
    for t in tracks:
        if t.net != "+3V3":
            continue
        for x, y in ((t.x1, t.y1), (t.x2, t.y2)):
            dx, dy = abs(x - cx), abs(y - cy)
            if max(dx, dy) < inner - 1e-6:                    # under the chip
                assert max(dx, dy) - t.width / 2 >= bb.EP_HALF + 0.2 - 1e-6, t
    ep = next(p for p in C.instances["U1"].part.footprint.pads if p.number == "57" and p.kind == "smd")
    assert ep.w == ep.h == 2 * bb.EP_HALF
    vias = [p for p in C.instances["U1"].part.footprint.pads if p.kind == "thru_hole"]
    assert len(vias) == 9 and {p.number for p in vias} == {"57"} and all("F.Paste" not in p.layers for p in vias)


def test_straps_and_flash_pins_unconnected():
    for g in STRAPS:
        assert net("U1", C.instances["U1"].part.pins[bb.gpio_pin(g)][0]) is None, g
    for name in FLASH_PINS:
        assert pin_net(name) is None, name
    assert pin_net("GPIO0") == "BUTTON" and net("SW1", "2") == "BUTTON"


def test_gpio_choice():
    used = set(bb.GPIO.values())
    assert not used & STRAPS
    assert not used & set(range(26, 33))                     # GPIO26-32: the SPI flash pads
    for k, g in bb.GPIO.items():
        assert pin_net(bb.gpio_pin(g)) is not None, k


def test_led_common_anode_through_resistors():
    assert net("D1", "2") == "+3V3"
    for colour, pad, ref, value in (("R", "1", "R6", "330R"), ("G", "4", "R7", "100R"), ("B", "3", "R8", "100R")):
        assert net("D1", pad) == net(ref, "1") == f"LED_{colour}K"
        assert net(ref, "2") == f"LED_{colour}" == pin_net(bb.gpio_pin(bb.GPIO[f"LED_{colour}"]))
        assert C.instances[ref].value == value


def test_pins_header():
    h = bb.pins_h()
    got = dict(re.findall(r"#define TC_PIN_(\w+) (\d+)", h))
    assert got == {k: str(v) for k, v in bb.GPIO.items()}
    assert "common-anode" in h


def test_placement_inside_outline():
    for r in C.instances:
        a = bbox(P, r)
        assert a[0] >= -1e-9 and a[1] >= -1e-9 and a[2] <= bb.W + 1e-9 and a[3] <= bb.L + 1e-9, r


def test_hand_route_ends_on_pad_centres():
    pads = {}
    for r, inst in C.instances.items():
        x, y, rot = P.at[r]
        for q in inst.part.footprint.pads:
            dx, dy = rotate(q.x, q.y, rot)
            pads.setdefault((r, q.number), (x + dx, y + dy))
    for f in (bb.usb_hand_route, bb.power_hand_route, bb.signal_hand_route):
        tracks, _ = f()
        for t in tracks:
            for e in ((t.x1, t.y1), (t.x2, t.y2)):
                for k, p in pads.items():
                    d = math.dist(p, e)
                    # an end within 0.1 mm of a pad centre is meant to be on it
                    assert d < 1e-3 or d > 0.1 or k[0] == "U1", (f.__name__, e, k)


def test_full_route_is_drc_clean():
    """Integration: build.py --board bare --route (slow)."""
    subprocess.run(["./.venv/bin/python", "build.py", "--board", "bare", "--route"], check=True, cwd=ROOT,
                   capture_output=True)
    out = ROOT / "out" / "bare"
    drc = json.loads((out / "drc.json").read_text())
    assert drc["unconnected_items"] == []
    assert [v for v in drc["violations"] if v["severity"] == "error"] == []
    tree = parse((out / "tinycrypt-bare.kicad_pcb").read_text())
    tracks = [Track(find(s, "net")[1], find(s, "layer")[1], *find(s, "start")[1:3], *find(s, "end")[1:3],
                    find(s, "width")[1]) for s in find_all(tree, "segment")]
    vias = [Via("GND", *find(v, "at")[1:3]) for v in find_all(tree, "via")]
    assert bb.clearance_violations(tracks, vias) == []
    for name in ("bom.csv", "cpl.csv", "pins.h", "tinycrypt-bare-gerbers.zip"):
        assert (out / name).exists(), name
