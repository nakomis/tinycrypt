import math
import re

from PIL import Image

from board import key_board as kb
from board.art import MIN_FEATURE, art_footprint, drop_holes, load_art, pixel_rects
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
    return C.instances["U1"].part.pins[kb.modpin(gpio)][0]


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
    assert caps == {("10uF", "VBUS"), ("10uF", "+3V3"), ("100nF", "+3V3")}
    assert C.instances["C1"].part.lcsc == "C19702"


def test_module_power_reset_and_every_gnd_pad():
    u = C.instances["U1"].part
    assert net("U1", "3") == "+3V3"
    for pad in u.pins["GND"]:
        assert net("U1", pad) == "GND", pad
    assert net("U1", "45") == "EN" == net("R2", "1") == net("C6", "1") == net("TP1", "1")
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


def test_led_through_330r():
    assert net("D1", "4") == "+3V3" and net("D1", "2") == "GND"
    assert net("D1", "3") == "LED_DIN" == net("R6", "1")
    assert net("R6", "2") == "LED_GPIO" == net("U1", mod(kb.GPIO["LED"]))
    assert C.instances["R6"].part.lcsc == "C25104" and C.instances["R6"].value == "330R"
    assert net("D1", "1") is None


def test_debug_pads_include_en():
    assert [net("TP1", str(n)) for n in range(1, 6)] == ["EN", "UART_RX", "UART_TX", "GND", "+3V3"]
    assert list(kb.DEBUG_LABELS.values()) == ["EN", "RX", "TX", "G", "3V3"]
    assert net("U1", "39") == "UART_TX" and net("U1", "40") == "UART_RX"


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
    assert "EN, RX, TX, G, 3V3" in h


def test_placement_inside_outline_and_no_overlaps():
    refs = list(C.instances)
    for r in refs:
        if r == "U1":
            continue                                       # its antenna overhangs, by design
        a = bbox(P, r)
        assert a[0] >= -1e-9 and a[1] >= -1e-9 and a[2] <= kb.W + 1e-9 and a[3] <= kb.L + 1e-9, r
    for i, a in enumerate(refs):
        for b in refs[i + 1:]:
            A, B = bbox(P, a), bbox(P, b)
            assert A[2] <= B[0] + 1e-9 or B[2] <= A[0] + 1e-9 or A[3] <= B[1] + 1e-9 or B[3] <= A[1] + 1e-9, (a, b)


def test_board_size():
    assert kb.W == 21.0 and abs(kb.L - 34.95) < 1e-9


def test_antenna_overhangs_the_edge_and_the_pads_stay_on_the_board():
    u = bbox(P, "U1")
    assert abs(u[1] - -5.05) < 1e-9                          # the 5.05 mm antenna strip is off the board
    assert abs(P.at["U1"][0] - kb.W / 2) < 1e-9
    assert kb.keepouts() == []
    for n, x0, y0, x1, y1 in kb._pad_boxes(C, P):
        if abs(x1 - x0 - 4.4) < 1e-6:                          # mounting-hole rings
            continue
        assert x0 >= 0.3 and y0 >= 0.3 and x1 <= kb.W - 0.3 and y1 <= kb.L - 0.3, (n, x0, y0, x1, y1)


def test_j1_at_the_far_end_facing_out():
    # J1's housing front (fab line y = +2.83) faces out across the far edge, within 0.5 mm of it
    assert 0 <= kb.L - (P.at["J1"][1] + 2.83) <= 0.5 and P.at["J1"][2] == 0


def test_parts_at_least_1mm_from_the_long_edges_except_the_right_strip():
    exceptions = {"R2", "C6", "R4", "R5", "TP1", "H1", "H2"}     # 2.8 mm strip right of the module
    for r, inst in C.instances.items():
        if r in exceptions or r == "U1":
            continue
        x0, _, x1, _ = bbox(P, r)
        body = 0.25 if inst.part.key[0] in "RC" and inst.part.key[1].isdigit() else 0.0
        assert x0 + body >= 0.9 and kb.W - x1 + body >= 0.9, (r, x0, x1)


def test_mounting_holes_m2_one_per_end_ring_masked_on_the_back():
    for h in ("H1", "H2"):
        pad = C.instances[h].part.footprint.pads[0]
        assert pad.drill == 2.2 and pad.w == 4.4 and pad.kind == "thru_hole"
        assert "F.Mask" in pad.layers and "B.Mask" not in pad.layers
    # H1 just past the module (whose end is all module and antenna), H2 beside J1
    assert P.at["H1"][1] - 2.2 > kb.MOD_Y + 7.7 and P.at["H2"][1] + 2.2 > P.at["J1"][1]


def test_led_beside_the_button():
    assert math.dist(P.at["D1"][:2], P.at["SW1"][:2]) < 5.0


def test_debug_pads_on_the_edge():
    x, _, _ = P.at["TP1"]
    assert abs(x - 19.9) < 1e-9


def _pads():
    pads = {}
    for r, inst in C.instances.items():
        x, y, rot = P.at[r]
        for q in inst.part.footprint.pads:
            dx, dy = rotate(q.x, q.y, rot)
            pads.setdefault((r, q.number), (x + dx, y + dy))
    return pads


def test_hand_route_ends_on_pad_centres():
    pads = _pads()
    for route in (kb.usb_hand_route, kb.power_hand_route, kb.signal_hand_route):
        tracks, _ = route()
        ends = {(t.x1, t.y1) for t in tracks if t.layer == "F.Cu"} | {(t.x2, t.y2) for t in tracks if t.layer == "F.Cu"}
        for e in ends:
            near = [k for k, p in pads.items() if math.dist(p, e) < 0.3]
            for k in near:                 # an end near a pad centre must be exactly on it
                assert math.dist(pads[k], e) < 1e-3 or k[1] == "GND", (route.__name__, e, k)


def test_usb_pair_on_f_cu_and_only_one_b_cu_hop_under_it():
    tracks, vias = kb.usb_hand_route()
    for t in tracks:
        if t.net.startswith("USB"):
            assert t.layer == "F.Cu" and t.width == kb.USB_W
    hops = [t for t in tracks if t.layer == "B.Cu"]
    assert len(hops) == 1 and hops[0].net == "VBUS"
    assert math.dist((hops[0].x1, hops[0].y1), (hops[0].x2, hops[0].y2)) < 2.2
    x0, y0, x1, y1 = kb.USB_ZONE
    for route in (kb.power_hand_route, kb.signal_hand_route):
        for t in route()[0]:
            if t.layer == "B.Cu":
                for x, y in ((t.x1, t.y1), (t.x2, t.y2)):
                    assert not (x0 < x < x1 and y0 < y < y1), t
    assert ("B.Cu", *kb.USB_ZONE) in kb.layer_keepouts()


def test_power_trunks_are_wide():
    tracks, _ = kb.power_hand_route()
    assert tracks and all(t.width >= 0.4 for t in tracks)
    assert {t.net for t in tracks} == {"VBUS", "+3V3"}
    # the 3V3 trunk reaches module pad 3 and U2's VOUT, the VBUS trunk J1.1, C1 and U2's VIN
    pads = _pads()
    ends = {(t.net, round(x, 3), round(y, 3)) for t in tracks for x, y in ((t.x1, t.y1), (t.x2, t.y2))}
    for net, ref, pad in (("+3V3", "U1", "3"), ("+3V3", "U2", "5"), ("VBUS", "J1", "1"), ("VBUS", "C1", "1"),
                          ("VBUS", "U2", "1")):
        x, y = pads[(ref, pad)]
        assert (net, round(x, 3), round(y, 3)) in ends, (ref, pad)


def test_no_b_cu_track_under_mus_face():
    x0, y0, x1, y1 = kb.FACE_ZONE
    for route in (kb.usb_hand_route, kb.power_hand_route, kb.signal_hand_route):
        for t in route()[0]:
            if t.layer == "B.Cu":
                assert not (x0 < (t.x1 + t.x2) / 2 < x1 and y0 < (t.y1 + t.y2) / 2 < y1), t


def test_art_is_mu_at_016_mm_per_pixel_on_the_back():
    png, x, y, h, side, holes = kb.art_key(P)[0]
    img = Image.open(png)
    assert img.size == (112, 129) and side == "back"
    art, px = load_art(png, h)
    assert abs(px - 0.16) < 1e-9 and px >= MIN_FEATURE
    assert x >= 0 and y >= 0 and x + 112 * px <= kb.W and y + h <= kb.L
    assert x > 1.6 + kb.POWER_W / 2                             # clear of the left 3V3 trunk


def test_art_footprint_mirrored_on_b_silks():
    png, x, y, h, side, _ = kb.art_key(P)[0]
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


def test_art_leaves_the_drill_clear():
    png, x, y, h, side, holes = kb.art_key(P)[0]
    img, px = load_art(png, h)
    cleared = drop_holes(img, px, x, y, True, holes)
    p = cleared.load()
    hx, hy, r = holes[0]                                        # H1, under her chest
    for row in range(cleared.height):
        for col in range(cleared.width):
            bx, by = x + (cleared.width - col - 0.5) * px, y + (row + 0.5) * px
            if math.hypot(bx - hx, by - hy) <= r - 0.1:
                assert p[col, row] == 0
    assert sum(1 for v in img.get_flattened_data() if v) > sum(1 for v in cleared.get_flattened_data() if v)


def test_art_features_at_least_015():
    png, x, y, h, *_ = kb.art_key(P)[0]
    img, px = load_art(png, h)
    for x0, x1, y0, y1 in pixel_rects(img):
        assert (x1 - x0) * px >= MIN_FEATURE and (y1 - y0) * px >= MIN_FEATURE


def test_no_fixed_via_on_mu_or_under_text():
    fixed = kb.usb_hand_route()[1] + kb.power_hand_route()[1] + kb.signal_hand_route()[1] + kb.edge_vias()
    assert not [v for v in fixed if kb.on_art(v.x, v.y, margin=0.3)]
    assert not kb.vias_under_text(fixed, kb.back_text_boxes() + kb.front_text_boxes(C, P))


def test_edge_vias_at_the_antenna_end():
    near = [v for v in kb.edge_vias() if v.y <= 2.0]
    assert sum(v.x < 2.8 for v in near) >= 2 and sum(v.x > 18.2 for v in near) >= 2


def test_stitching_avoids_mu_and_the_text():
    assert kb.on_art(kb.ART_CX, 12.0)               # the middle of her face is white
    assert not kb.on_art(1.0, 33.0)                 # the far corner has no art
    boxes = kb.back_text_boxes() + kb.front_text_boxes(C, P)
    vias = kb.stitch_vias(C, P, [], [], avoid=boxes, avoid_at=kb.on_art)
    assert vias and not kb.vias_under_text(vias, boxes)
    assert not any(kb.on_art(v.x, v.y) for v in vias)


def test_pad_extras():
    ex = kb.pad_extra()
    centre = [q for q in C.instances["U1"].part.footprint.pads if q.number == "GND" and abs(q.w - 1.2) < 1e-6]
    assert len(centre) == 9
    for q in centre:
        assert ex["U1"](q) == [["solder_paste_margin", -0.18]]
    # 0.84 mm of paste on a 1.2 mm pad: 49 %, inside Espressif's 45-55 %
    assert 0.45 <= ((1.2 - 2 * 0.18) / 1.2) ** 2 <= 0.55
    gnd = next(q for q in C.instances["U2"].part.footprint.pads if q.number == "2")
    assert ex["U2"](gnd) == [["zone_connect", 2]]
