import math
import re

from PIL import Image

from board import key_board as kb
from board.art import MIN_FEATURE, art_footprint, load_art, pixel_rects
from board.netlist import check
from board.placement import bbox, rotate
from board.sexpr import find, find_all

C = kb.build_key()
P = kb.place_key(C)
STRAPPING = {0, 3, 45, 46}
FLASH_PSRAM = set(range(26, 33)) | set(range(33, 38)) | {47, 48}   # SPI0/1 pins, octal pins


def net(ref, pad):
    return C.net_of(ref, pad)


def mod(gpio):
    """The module pad carrying GPIO n."""
    name = kb.modpin(gpio)
    return C.instances["U1"].part.pins[name][0]


def test_clean():
    assert check(C) == []


def test_usb_on_gpio19_and_gpio20():
    assert kb.GPIO["USB_DM"] == 19 and kb.GPIO["USB_DP"] == 20
    assert mod(19) == "23" and mod(20) == "24"           # Espressif Table 3-1
    assert net("U1", "23") == "USB_DM" and net("U1", "24") == "USB_DP"


def test_usb_path_through_the_esd_chip():
    assert net("J1", "2") == net("U3", "1") == net("U3", "6") == "USB_DM"
    assert net("J1", "3") == net("U3", "3") == net("U3", "4") == "USB_DP"
    assert net("J1", "1") == net("U3", "5") == net("U2", "1") == net("U2", "3") == "VBUS"
    assert net("U3", "2") == "GND"


def test_j1_pinout_matches_its_labels():
    labels = {"VBUS": "V", "USB_DM": "D-", "USB_DP": "D+", "CC": "CC", "GND": "G"}
    for pad, label in kb.J1_LABELS.items():
        assert labels[net("J1", pad)] == label
    assert net("J1", "6") == net("J1", "7") == "GND"
    assert {net("R1", "1"), net("R1", "2")} == {"CC", "GND"}
    assert C.instances["R1"].value == "5k1"


def test_regulator():
    assert net("U2", "5") == "+3V3" and net("U2", "2") == "GND"
    caps = {(C.instances[r].value, net(r, "1")) for r in ("C1", "C2", "C3")}
    assert caps == {("1uF", "VBUS"), ("10uF", "+3V3"), ("100nF", "+3V3")}


def test_module_power_reset_and_every_gnd_pad():
    u = C.instances["U1"].part
    assert net("U1", "3") == "+3V3"
    for pad in u.pins["GND"]:
        assert net("U1", pad) == "GND", pad
    assert net("U1", "45") == "EN" == net("R2", "1") == net("C6", "1")
    assert net("R2", "2") == "+3V3" and net("C6", "2") == "GND" and C.instances["C6"].value == "1uF"
    assert {C.instances[r].value for r in ("C4", "C5")} == {"10uF", "100nF"}


def test_strapping_pins_unconnected_except_the_button():
    for gpio in (3, 45, 46):
        assert net("U1", mod(gpio)) is None, gpio
    assert net("U1", mod(0)) == "BUTTON" == net("SW1", "2") == net("R3", "1")
    assert net("SW1", "3") == "GND" and net("R3", "2") == "+3V3"
    assert net("SW1", "1") is None and net("SW1", "4") is None       # diagonal pads only


def test_secure_element():
    assert net("U4", "8") == "+3V3" and net("U4", "4") == "GND"
    assert net("U4", "5") == "SDA" == net("U1", mod(kb.GPIO["SDA"])) == net("R4", "1")
    assert net("U4", "6") == "SCL" == net("U1", mod(kb.GPIO["SCL"])) == net("R5", "1")
    for pad in "1237":
        assert net("U4", pad) is None
    assert C.instances["R4"].value == C.instances["R5"].value == "4k7"


def test_led():
    assert net("D1", "4") == "+3V3" and net("D1", "2") == "GND"
    assert net("D1", "3") == "LED_DIN" == net("U1", mod(kb.GPIO["LED"]))
    assert net("D1", "1") is None


def test_debug_pads():
    assert net("U1", "39") == "UART_TX" == net("TP1", "2")
    assert net("U1", "40") == "UART_RX" == net("TP1", "1")
    assert net("TP1", "3") == "+3V3" and net("TP1", "4") == "GND"


def test_gpio_choice_avoids_reserved_pins():
    free = {k: v for k, v in kb.GPIO.items() if k not in ("BUTTON", "USB_DM", "USB_DP", "UART_TX", "UART_RX")}
    for k, g in free.items():
        assert g not in STRAPPING | FLASH_PSRAM | {19, 20, 43, 44}, k
    assert len(set(kb.GPIO.values())) == len(kb.GPIO)


def test_pins_header():
    h = kb.pins_h()
    got = dict(re.findall(r"#define TC_PIN_(\w+) (\d+)", h))
    assert got == {k: str(v) for k, v in kb.GPIO.items()}
    assert got["USB_DM"] == "19" and got["USB_DP"] == "20" and got["BUTTON"] == "0"


def test_placement_inside_outline_and_no_overlaps():
    x0, y0, x1, y1 = P.outline
    refs = list(C.instances)
    for r in refs:
        a = bbox(P, r)
        assert a[0] >= x0 - 1e-9 and a[1] >= y0 - 1e-9 and a[2] <= x1 + 1e-9 and a[3] <= y1 + 1e-9, r
    for i, a in enumerate(refs):
        for b in refs[i + 1:]:
            A, B = bbox(P, a), bbox(P, b)
            assert A[2] <= B[0] + 1e-9 or B[2] <= A[0] + 1e-9 or A[3] <= B[1] + 1e-9 or B[3] <= A[1] + 1e-9, (a, b)


def test_board_size():
    assert 19.0 <= kb.W <= 21.0 and kb.L <= 40.0


def test_module_antenna_flush_with_the_edge_and_j1_at_the_other_end():
    assert bbox(P, "U1")[1] == 0.0                            # module body top on the y = 0 edge
    assert abs(P.at["U1"][0] - kb.W / 2) < 1e-9
    # J1's housing front (fab line y = +2.83) faces out across the far edge, within 0.5 mm of it
    assert 0 <= kb.L - (P.at["J1"][1] + 2.83) <= 0.5 and P.at["J1"][2] == 0


def _pad_boxes():
    return kb._pad_boxes(C, P)


def test_no_pad_in_the_antenna_keepout():
    for n, x0, y0, x1, y1 in _pad_boxes():
        assert y0 >= kb.ANT_Y, (n, y0)


def test_pads_clear_of_the_board_edge():
    for n, x0, y0, x1, y1 in _pad_boxes():
        if n == "GND" and abs(x1 - x0 - 4.4) < 1e-6:       # mounting-hole rings
            continue
        assert x0 >= 0.3 and y0 >= 0.3 and x1 <= kb.W - 0.3 and y1 <= kb.L - 0.3, (n, x0, y0, x1, y1)


def test_mounting_holes_m2_one_per_end():
    for h in ("H1", "H2"):
        pad = C.instances[h].part.footprint.pads[0]
        assert pad.drill == 2.2 and pad.w == 4.4 and pad.kind == "thru_hole"
        x, y, _ = P.at[h]
        assert y - 2.2 > kb.ANT_Y and 2.2 + 0.3 <= x <= kb.W - 2.5
    # H1 just past the module (whose end is all module and antenna), H2 beside J1
    assert kb.MOD_Y + 7.7 < P.at["H1"][1] - 2.2 and P.at["H2"][1] + 2.2 > P.at["J1"][1]


def test_led_beside_the_button():
    assert math.dist(P.at["D1"][:2], P.at["SW1"][:2]) < 5.0


def test_debug_pads_on_the_edge():
    x, _, _ = P.at["TP1"]
    assert kb.W - (x + 0.5) <= 0.5


def test_hand_route_ends_on_pad_centres():
    tracks, vias = kb.usb_hand_route()
    pads = {(r, p.number): (P.at[r][0] + rotate(p.x, p.y, P.at[r][2])[0], P.at[r][1] + rotate(p.x, p.y, P.at[r][2])[1])
            for r in ("J1", "U3", "U1") for p in C.instances[r].part.footprint.pads}
    ends = {(t.x1, t.y1) for t in tracks} | {(t.x2, t.y2) for t in tracks}
    for key in (("J1", "2"), ("J1", "3"), ("U3", "1"), ("U3", "3"), ("U3", "4"), ("U3", "6"), ("U1", "23"), ("U1", "24")):
        assert any(math.dist(pads[key], e) < 1e-6 for e in ends), key
    for t in tracks:
        assert t.net in ("USB_DM", "USB_DP", "VBUS", "GND") and t.layer == "F.Cu"


def test_art_is_mu_at_016_mm_per_pixel_on_the_back():
    png, x, y, h, side = kb.art_key(P)[0]
    img = Image.open(png)
    assert img.size == (112, 128) and side == "back"
    art, px = load_art(png, h)
    assert abs(px - 0.16) < 1e-9 and px >= MIN_FEATURE
    assert x >= 0 and y >= 0 and x + 112 * px <= kb.W and y + h <= kb.L


def test_art_footprint_mirrored_on_b_silks():
    png, x, y, h, side = kb.art_key(P)[0]
    front = art_footprint(png, x, y, h, side="front")
    back = art_footprint(png, x, y, h, side="back")
    assert find(back, "layer")[1] == "B.Cu" and "board_only" in find(back, "attr")
    polys_f = find_all(front, "fp_poly")
    polys_b = find_all(back, "fp_poly")
    assert len(polys_f) == len(polys_b) > 0
    assert all(find(p, "layer")[1] == "B.SilkS" for p in polys_b)
    w = 112 * 0.16
    for pf, pb in zip(polys_f, polys_b):
        xf = sorted(xy[1] for xy in find_all(find(pf, "pts"), "xy"))
        xb = sorted(xy[1] for xy in find_all(find(pb, "pts"), "xy"))
        assert abs(xf[0] - (w - xb[-1])) < 1e-3 and abs(xf[-1] - (w - xb[0])) < 1e-3


def test_art_features_at_least_015():
    png, x, y, h, _ = kb.art_key(P)[0]
    img, px = load_art(png, h)
    for x0, x1, y0, y1 in pixel_rects(img):
        assert (x1 - x0) * px >= MIN_FEATURE and (y1 - y0) * px >= MIN_FEATURE


def test_stitching_avoids_mu_and_the_text():
    assert kb.on_art(kb.CX, 12.0)                 # the middle of his face is white
    assert not kb.on_art(1.0, 39.0)               # the far corner has no art
    vias = kb.stitch_vias(C, P, [], [], avoid=kb.back_text_boxes(), avoid_at=kb.on_art)
    assert vias and not kb.vias_under_text(vias)
    assert not any(kb.on_art(v.x, v.y) for v in vias)
