"""tinycrypt key, bare-chip variant (CRYPT-24): the ESP32-S3FN8 itself, with its crystal, RF match
and a chip antenna, in place of the ESP32-S3-MINI-1 module, so that every part is Economic-eligible
at JLCPCB. See DESIGN-bare.md for the parts, the circuit and the sources of every value.

Same interface as key_board (build.py calls either). The lower half (USB in, LDO, ATECC, button,
mounting holes, Mu on the back) keeps key_board's placement; the top 13 mm is new.

Board frame (KiCad, y down): x 0..W, y 0..L. The antenna sits on the y = 0 edge, top left, in its
own ground clearance. The chip's RF corner (pin 1, the RF supply pins 2/3, the crystal pins 53/54
and VDDA 55/56) faces it.
"""
from pathlib import Path

from board import key_board as kb
from board.kicad_pcb import Track, Via
from board.netlist import Circuit
from board.placement import Placement, rotate
from board.parts import part

HERE = Path(__file__).resolve().parent.parent
W, L = kb.W, kb.L
CX = W / 2
U1_AT = (12.0, 8.4)                  # chip centre; pin 1 (LNA_IN) at its top-left corner
EP_HALF = 1.7                        # the shrunk exposed pad (footprints/local.pretty)
MOAT = 2.4                           # 3V3 runs this far from the chip centre, between pad and pins

# GPIOs. The left column below the RF pins is crowded (RF supply, CHIP_PU, GPIO0), and the top
# row is the crystal and VDDA, so the LED takes bottom-row GPIOs, SCL the bottom-row pin right of
# the USB pair (GPIO21, down the channel beside it) and SDA and the debug UART right-column ones (UART0 is routed through the GPIO matrix: CONFIG_ESP_CONSOLE_UART_CUSTOM;
# only the ROM's first messages still go to U0TXD, which is not wired). None is a strap other than
# GPIO0 (3, 45 and 46 are left alone) or an in-package flash pin (SPI pins 30-35).
GPIO = {"BUTTON": 0, "LED_R": 10, "LED_G": 11, "LED_B": 12, "SDA": 47, "SCL": 21,
        "USB_DM": 19, "USB_DP": 20, "UART_TX": 33, "UART_RX": 34}
# The chip symbol names some GPIOs by their other function, as Espressif's pin table does.
_CHIPPIN = {15: "XTAL_32K_P", 16: "XTAL_32K_N", 39: "MTCK", 40: "MTDO", 41: "MTDI", 42: "MTMS",
            43: "U0TXD", 44: "U0RXD", 47: "SPICLK_P", 48: "SPICLK_N"}


def gpio_pin(gpio):
    return _CHIPPIN.get(gpio, f"GPIO{gpio}")


RF_W = 1.1          # 50 ohm grounded coplanar on 1.6 mm FR-4 with the pours' 0.2 mm gap (DESIGN-bare.md)
NECK = 0.5          # where the RF line meets a 0402 pad
SIG_W = 0.2
PWR_W = 0.3

J1_PINS, J1_LABELS = kb.J1_PINS, kb.J1_LABELS
DEBUG_PINS, DEBUG_LABELS = kb.DEBUG_PINS, kb.DEBUG_LABELS


def build_key():
    c = Circuit()
    u1 = c.add("S3FN8", "U1")
    u2 = c.add("LDO", "U2")
    u3 = c.add("ESD", "U3")
    u4 = c.add("ATECC", "U4")
    j1 = c.add("JST5", "J1")
    sw = c.add("SW", "SW1")
    d1 = c.add("RGB", "D1")
    tp = c.add("DEBUG", "TP1")
    ae = c.add("ANT", "AE1")
    y1 = c.add("XTAL", "Y1")
    c.add("MH", "H1")
    c.add("MH", "H2")
    gnd, v33, vbus = "GND", "+3V3", "VBUS"

    def cap(key, ref, net, other=gnd):
        k = c.add(key, ref)
        c.connect(net, (k, "1"))
        c.connect(other, (k, "2"))
        return k

    # USB in, as on the module board: J1 -> USBLC6 (flow-through) -> the chip; CC 5k1 to GND.
    for pad, net in J1_PINS.items():
        c.connect(net, (j1, pad))
    c.connect(gnd, (j1, "6"), (j1, "7"))
    c.connect("USB_DM", (u3, "1"), (u3, "6"), (u1, gpio_pin(GPIO['USB_DM'])))
    c.connect("USB_DP", (u3, "3"), (u3, "4"), (u1, gpio_pin(GPIO['USB_DP'])))
    c.connect(vbus, (u3, "5"))
    c.connect(gnd, (u3, "2"))
    c.connect(gnd, (c.add("R5K1", "R1"), "2"))
    c.connect("CC", (c.instances["R1"], "1"))

    # 3.3 V: AP2112K, EN tied to VIN, 10 uF in, 10 uF + 100 nF out.
    c.connect(vbus, (u2, "VIN"), (u2, "EN"))
    c.connect(gnd, (u2, "GND"))
    c.connect(v33, (u2, "VOUT"))
    cap("C10U", "C1", vbus)
    cap("C10U", "C2", v33)
    cap("C100N", "C3", v33)

    # Chip supplies, after the ESP32-S3-MINI-1 reference schematic (datasheet v1.7, figure 8-1):
    # VDD3P3 (2, 3), the RF supply, behind a 2.0 nH series inductor with 10 uF + 1 uF + 100 nF before
    # it and 100 nF at the pins; VDDA (55, 56) 10 nF at the pins (its 1 uF is C11 at pin 46, on the
    # same 3V3, which reaches VDDA under the chip); VDD3P3_CPU (46) 100 nF; VDD3P3_RTC (20) 100 nF;
    # VDD_SPI (29), the in-package flash supply made inside the chip while GPIO45 stays low,
    # 100 nF + 1 uF and nothing else. The exposed pad is GND.
    cap("C10U", "C4", v33)
    cap("C1U", "C5", v33)
    cap("C100N", "C8", v33)
    l1 = c.add("L2N0", "L1")
    c.connect(v33, (l1, "1"))
    c.connect("VDD3P3", (l1, "2"), (u1, "VDD3P3"))
    cap("C100N", "C9", "VDD3P3")
    c.connect(v33, (u1, "VDDA"), (u1, "VDD3P3_CPU"), (u1, "VDD3P3_RTC"))
    cap("C100N", "C10", v33)            # pin 46
    cap("C1U", "C11", v33)              # pin 46, and VDDA through the moat
    cap("C10N", "C12", v33)             # VDDA, at pin 56
    cap("C100N", "C15", v33)            # pin 20
    c.connect("VDD_SPI", (u1, "VDD_SPI"))
    cap("C100N", "C13", "VDD_SPI")
    cap("C1U", "C14", "VDD_SPI")
    c.connect(gnd, (u1, "EP"))

    # CHIP_PU: 10k to 3V3, 1 uF to GND (Espressif's RC power-up delay), and the debug pad.
    c.connect("EN", (u1, "CHIP_PU"), (c.add("R10K", "R2"), "1"), (c.add("C1U", "C6"), "1"))
    c.connect(v33, (c.instances["R2"], "2"))
    c.connect(gnd, (c.instances["C6"], "2"))

    # 40 MHz crystal (+-10 ppm): XTAL_P through 24 nH (Espressif's starting value), 15 pF load caps
    # to tune; pads 2 and 4 are its lid, on GND.
    l3 = c.add("L24N", "L3")
    c.connect("XTAL_P", (u1, "XTAL_P"), (l3, "1"))
    c.connect("XTAL_PL", (l3, "2"), (y1, "1"))
    c.connect("XTAL_N", (u1, "XTAL_N"), (y1, "3"))
    c.connect(gnd, (y1, "GND"))
    cap("C15P", "C18", "XTAL_PL")
    cap("C15P", "C19", "XTAL_N")

    # RF: LNA_IN -> C16 (shunt, chip side) -> L2 (series) -> C17 (shunt, antenna side) -> the antenna.
    # Starting values in Espressif's ranges (C 1.2-1.8 pF, L 2.0-3.0 nH); to be tuned on the board.
    # The antenna is fed from the right: datasheet function 2, pad 4 the feed, pads 1-3 GND.
    l2 = c.add("L2N4", "L2")
    c.connect("LNA_IN", (u1, "LNA_IN"), (l2, "1"))
    cap("C1P5", "C16", "LNA_IN")
    c.connect("RF_ANT", (l2, "2"), (ae, "4"))
    cap("C1P5", "C17", "RF_ANT")
    c.connect(gnd, (ae, "1"), (ae, "2"), (ae, "3"))

    # Button on GPIO0 (presence + download strap), 10k pull-up; diagonal pads B/C, as on key_board.
    c.connect("BUTTON", (u1, gpio_pin(GPIO['BUTTON'])), (sw, "B"), (c.add("R10K", "R3"), "1"))
    c.connect(v33, (c.instances["R3"], "2"))
    c.connect(gnd, (sw, "C"))

    # ATECC608B: VCC 8, GND 4, SDA 5, SCL 6; 4k7 pull-ups; 100 nF.
    c.connect(v33, (u4, "VCC"))
    c.connect(gnd, (u4, "GND"))
    c.connect("SDA", (u4, "SDA"), (u1, gpio_pin(GPIO['SDA'])), (c.add("R4K7", "R4"), "1"))
    c.connect("SCL", (u4, "SCL"), (u1, gpio_pin(GPIO['SCL'])), (c.add("R4K7", "R5"), "1"))
    c.connect(v33, (c.instances["R4"], "2"), (c.instances["R5"], "2"))
    cap("C100N", "C7", v33)

    # Status LED: common-anode RGB on 3V3; each cathode to a GPIO through a resistor (the GPIO sinks).
    # Red's forward voltage is ~2 V, green's and blue's ~3 V, so those get the small resistor.
    c.connect(v33, (d1, "COM+"))
    for colour, pin, key, ref in (("R", "R-", "R330", "R6"), ("G", "G-", "R100", "R7"), ("B", "B-", "R100", "R8")):
        r = c.add(key, ref)
        c.connect(f"LED_{colour}K", (d1, pin), (r, "1"))
        c.connect(f"LED_{colour}", (r, "2"), (u1, gpio_pin(GPIO['LED_' + colour])))

    # Debug pads: EN, the console UART (GPIO matrix), GND, 3V3.
    c.connect("UART_TX", (u1, gpio_pin(GPIO['UART_TX'])))
    c.connect("UART_RX", (u1, gpio_pin(GPIO['UART_RX'])))
    for pad, net in DEBUG_PINS.items():
        c.connect(net, (tp, pad))

    c.connect(gnd, (c.instances["H1"], "1"), (c.instances["H2"], "1"))
    return c


# (x, y, rotation). Below y ~13 everything is new; the rest is key_board's placement.
AT = {
    "U1": (*U1_AT, 0),
    # antenna: fed from the right (rotated 180: pad 4 bottom right), its pads touching the edge strip
    "AE1": (3.0, 1.25, 180),
    # RF match down to pin 1: C17 beside the line, L2 in it, C16 on the stub into pin 1
    "C17": (7.52, 2.6, 0), "L2": (6.6, 4.0, 90), "C16": (7.4, 5.38, 90),
    # VDDA 10 nF at the corner, beside pin 56
    "C12": (8.85, 4.3, 90),
    # crystal above pins 53/54; L3 inline on XTAL_P; load caps at its two signal pads
    "Y1": (10.65, 1.6, 0), "L3": (9.85, 3.76, 90), "C18": (8.7, 1.6, 90), "C19": (13.1, 0.85, 0),
    # pin 46 (VDD3P3_CPU): 100 nF + 1 uF
    "C10": (13.4, 3.75, 90), "C11": (14.2, 3.75, 90),
    # RF supply (pins 2/3): C9 at the pins, L1 inline, then 100 nF + 1 uF + 10 uF on 3V3
    "C9": (7.0, 7.02, 270), "L1": (5.75, 6.6, 0), "C8": (4.55, 7.2, 90), "C5": (3.7, 7.2, 90),
    "C4": (2.6, 7.6, 90),
    # pin 20 (VDD3P3_RTC) and pin 29 (VDD_SPI)
    "C15": (11.4, 13.05, 270), "C13": (16.6, 11.45, 0), "C14": (16.6, 12.2, 0),
    # CHIP_PU RC and the GPIO0 pull-up, on the left below the RF supply
    "R2": (5.2, 9.4, 270), "C6": (4.2, 9.4, 270), "R3": (4.0, 12.2, 90),
    "TP1": kb.AT["TP1"],
    # lower half from key_board; the LED and its three resistors where D1/R6 were
    # the LED at 270: G- and B- on top, under their resistors; R- bottom left, its resistor below
    # it (SW1's 5.1 mm body reaches x 6.15, so nothing goes left of the LED but a track)
    "SW1": kb.AT["SW1"], "D1": (7.65, 18.15, 270),
    "R7": (7.23, 16.05, 90), "R8": (8.3, 16.05, 90), "R6": (7.1, 20.2, 270),
    **{r: kb.AT[r] for r in ("U2", "C1", "C2", "C3", "U3", "J1", "R1", "U4", "C7", "R4", "R5", "H1", "H2")},
}


def place_key(circuit):
    at = {r: AT[r] for r in circuit.instances}
    bodies = {r: i.part.body for r, i in circuit.instances.items()}
    return Placement((0.0, 0.0, W, L), at, bodies)


_KEY = {}


def _part_key(ref):
    if not _KEY:
        _KEY.update({r: i.part.key for r, i in build_key().instances.items()})
    return _KEY[ref]


def pad_xy(ref, number):
    """Board position of a pad centre (the first pad with that number)."""
    x, y, rot = AT[ref]
    q = next(q for q in part(_part_key(ref)).footprint.pads if q.number == number)
    dx, dy = rotate(q.x, q.y, rot)
    return round(x + dx, 4), round(y + dy, 4)


def pin_xy(name):
    """U1's pad for a symbol pin name."""
    return pad_xy("U1", part("S3FN8").pins[name][0])


# Antenna ground clearance (Johanson 2450AT18D0100E datasheet, "standard layout"): 4.0 x 6.0 mm from
# the board edge, no copper on any layer under or around the antenna, except its own pads, which sit
# in notches so that the GND ones reach the pour beside it and the feed reaches the RF line.
ANT_CLEAR = (1.0, 0.0, 5.0, 6.3)


def _antenna_clearance():
    x0, y0, x1, y1 = ANT_CLEAR
    m = 0.05
    notch = []
    for n in ("1", "2", "3", "4"):
        px, py = pad_xy("AE1", n)
        notch.append((px, py))
    # pads: 2 and 3 on the left (x 1.0-2.0), 1 and 4 on the right (x 4.0-5.0)
    (r1x, r1y), (l2x, l2y), (l3x, l3y), (r4x, r4y) = notch
    h = 0.3 + m                          # half the pad's height (0.6) plus margin
    xi_l, xi_r = 2.0 + m, 4.0 - m        # inner edges of the left and right pad columns
    return [(x0, y0), (x1, y0),
            (x1, r1y - h), (xi_r, r1y - h), (xi_r, r1y + h), (x1, r1y + h),
            (x1, r4y - h), (xi_r, r4y - h), (xi_r, r4y + h), (x1, r4y + h),
            (x1, y1), (x0, y1),
            (x0, l3y + h), (xi_l, l3y + h), (xi_l, l3y - h), (x0, l3y - h),
            (x0, l2y + h), (xi_l, l2y + h), (xi_l, l2y - h), (x0, l2y - h)]


def keepouts():
    return [("ANTENNA", _antenna_clearance(), ("F.Cu", "B.Cu"))]


def route_area():
    return [(0.0, 0.0), (W, 0.0), (W, L), (0.0, L)]


# Where the router may put nothing on B.Cu: the whole chip, RF and crystal end (Espressif's two-layer
# rule: an unbroken ground under them), except the right-hand strip, and under the USB pair.
SOLID_GND = (2.3, 0.0, 16.3, 14.6)          # the left edge strip below the antenna carries the 3V3 trunk
USB_ZONE = (9.0, 12.0, 14.3, 23.6)


def layer_keepouts():
    # the antenna clearance on both layers: without it Freerouting joined the GND vias beside the
    # clearance to the antenna's GND pad straight across it
    return [("B.Cu", *SOLID_GND), ("B.Cu", 0.0, 0.0, 2.3, 6.6), ("B.Cu", *USB_ZONE),
            ("F.Cu", *ANT_CLEAR), ("B.Cu", *ANT_CLEAR)]


def clearance_violations(tracks, vias):
    """Tracks and vias inside the antenna clearance, other than the feed's own stub out of its pad
    (inside the pad's notch). KiCad's DRC did not report router tracks inside the ANTENNA rule
    area, so the build checks it itself."""
    x0, y0, x1, y1 = ANT_CLEAR
    feed = pad_xy("AE1", "4")
    bad = []
    for t in tracks:
        if t.net == "RF_ANT" and abs(t.y1 - feed[1]) < 1e-6 and abs(t.y2 - feed[1]) < 1e-6:
            continue
        for k in range(11):
            x, y = t.x1 + (t.x2 - t.x1) * k / 10, t.y1 + (t.y2 - t.y1) * k / 10
            if x0 - t.width / 2 < x < x1 + t.width / 2 and y0 <= y < y1 + t.width / 2:
                bad.append(t)
                break
    bad += [v for v in vias if x0 - v.size / 2 < v.x < x1 + v.size / 2 and v.y < y1 + v.size / 2]
    return bad


def _seg(net, pts, width, layer="F.Cu"):
    return kb._segments(net, layer, pts, width)


def rf_hand_route():
    """The antenna feed to pin 1: RF_ANT (antenna pad 4 -> C17 -> L2) at RF_W, necked at the pads;
    LNA_IN (L2 -> C16 -> pin 1), short, 0.4 mm, 0.2 mm into the pin."""
    feed = pad_xy("AE1", "4")
    l2a, l2b = pad_xy("L2", "2"), pad_xy("L2", "1")      # L2 rotated 90: pad 2 up, pad 1 down
    c17 = pad_xy("C17", "1")
    c16 = pad_xy("C16", "1")
    p1 = pin_xy("LNA_IN")
    x = l2a[0]
    t = _seg("RF_ANT", [feed, (feed[0] + 0.7, feed[1])], 0.6)
    t += _seg("RF_ANT", [(feed[0] + 0.7, feed[1]), (x, feed[1]), (x, 3.0)], RF_W)
    t += _seg("RF_ANT", [(x, 3.0), l2a], NECK)
    t += _seg("RF_ANT", [c17, (x, c17[1])], NECK)
    t += _seg("LNA_IN", [l2b, (x, p1[1]), (p1[0] - 0.6, p1[1])], 0.4)
    t += _seg("LNA_IN", [(p1[0] - 0.6, p1[1]), p1], SIG_W)
    t += _seg("LNA_IN", [c16, (c16[0], p1[1])], SIG_W)
    return t, []


def crystal_hand_route():
    """XTAL_P: pin 54 -> L3 -> Y1 pad 1 (bottom left); XTAL_N: pin 53 straight up between the
    crystal's pad columns into pad 3 (top right). No vias (Espressif)."""
    xp, xn = pin_xy("XTAL_P"), pin_xy("XTAL_N")
    top = xp[1] - 0.33                                    # the pin row's outer edge
    l3a, l3b = pad_xy("L3", "1"), pad_xy("L3", "2")       # L3 at 90: pad 1 down, pad 2 up
    y1p1, y1p3 = pad_xy("Y1", "1"), pad_xy("Y1", "3")
    t = _seg("XTAL_P", [(xp[0], top), (xp[0], top - 0.21), (l3a[0] + 0.17, l3a[1] + 0.15)], SIG_W)
    t += _seg("XTAL_P", [xp, (xp[0], top)], SIG_W)
    t += _seg("XTAL_PL", [l3b, (l3b[0], y1p1[1])], SIG_W)
    c18 = pad_xy("C18", "1")
    t += _seg("XTAL_PL", [c18, (c18[0], y1p1[1]), y1p1], SIG_W)
    t += _seg("XTAL_N", [xn, (xn[0], y1p3[1]), y1p3], SIG_W)
    c19 = pad_xy("C19", "1")
    t += _seg("XTAL_N", [y1p3, c19], SIG_W)
    # the crystal's top-left lid pad sits in a corner the pour barely reaches: tie it to C18's GND
    t += _seg("GND", [pad_xy("Y1", "4"), pad_xy("C18", "2")], PWR_W)
    return t, []


def power_hand_route():
    """Around the chip: VDDA 10 nF to pin 56; the 3V3 moat under the chip from pin 46's inner end
    to VDDA's (55, 56) inner ends (so that VDDA needs no route across the RF line); pin 46's caps;
    the RF supply from pins 2/3 through C9 and L1; pin 20's and pin 29's caps. The moat also runs
    down the left and along the bottom to pin 20's inner end."""
    cx, cy = U1_AT
    t = []
    # VDDA
    p56, p55 = pad_xy("U1", "56"), pad_xy("U1", "55")
    c12 = pad_xy("C12", "1")
    t += _seg("+3V3", [c12, (p56[0], c12[1] + 0.06)], 0.25)
    # the moat: pin 46 inner end -> down to y = cy - MOAT -> left to 55 and 56
    p46 = pad_xy("U1", "46")
    inner = p46[1] + 0.33
    my = round(cy - MOAT, 4)
    for p in (p46, p55, p56):
        t += _seg("+3V3", [(p[0], inner), (p[0], my)], SIG_W)
    t += _seg("+3V3", [(p56[0], my), (p46[0], my)], PWR_W)
    # ... and down the left side and along the bottom to pin 20 (VDD3P3_RTC), so that it needs no
    # route from outside either (Espressif's two-layer scheme: power between the pad and the pins)
    p20 = pad_xy("U1", "20")
    lx, by = round(cx - MOAT, 4), round(cy + MOAT, 4)
    t += _seg("+3V3", [(p56[0], my), (lx, my), (lx, by), (p20[0], by)], PWR_W)
    t += _seg("+3V3", [(p20[0], by), (p20[0], p20[1] - 0.33)], SIG_W)
    # pin 46: up to C10, C10 to C11
    c10, c11 = pad_xy("C10", "1"), pad_xy("C11", "1")
    t += _seg("+3V3", [(p46[0], p46[1] - 0.33), c10], SIG_W)
    t += _seg("+3V3", [c10, c11], PWR_W)
    # RF supply: join pins 2 and 3 at their outer ends, then left through C9 to L1
    p2, p3 = pad_xy("U1", "2"), pad_xy("U1", "3")
    jx = round(p2[0] - 0.19, 4)
    c9, l1 = pad_xy("C9", "1"), pad_xy("L1", "2")
    t += _seg("VDD3P3", [(jx, p2[1]), (jx, p3[1])], PWR_W)
    t += _seg("VDD3P3", [(jx, p3[1]), (c9[0], p3[1]), l1], PWR_W)
    # pin 20 and pin 29
    p20, c15 = pad_xy("U1", "20"), pad_xy("C15", "1")
    t += _seg("+3V3", [p20, c15], SIG_W)
    p29, c13, c14 = pad_xy("U1", "29"), pad_xy("C13", "1"), pad_xy("C14", "1")
    t += _seg("VDD_SPI", [p29, (p29[0] + 0.3, p29[1]), (c13[0], c13[1] - 0.25), c13, c14], SIG_W)
    return t, []


EDGE_X = 20.35          # the right-hand B.Cu 3V3 trunk (0.35 mm from the edge with its via)
LEFT_X = 1.5            # the left-hand one


def trunk_hand_route():
    """3V3 to the two ends the router can't reach through the chip's surroundings:
    left: U2's output -> via -> B.Cu up the left edge -> the RF supply's input caps (C4/C5/C8, then
    L1) and the CHIP_PU and GPIO0 pull-ups; right: debug pad 5 -> via -> B.Cu up the right edge ->
    pin 46's caps (C10/C11), which feed VDDA through the moat."""
    w = PWR_W
    t, v = [], []
    # left
    vout, c3, c2 = kb.pad_xy("U2", "5"), kb.pad_xy("C3", "1"), kb.pad_xy("C2", "1")
    vb = Via("+3V3", LEFT_X, 24.0)
    t += _seg("+3V3", [vout, (2.4, vout[1]), (2.4, vb.y), (vb.x, vb.y), c3], w)
    t += _seg("+3V3", [(2.4, vout[1]), (2.4, c2[1]), c2], w)
    l1, c8, c5, c4 = pad_xy("L1", "1"), pad_xy("C8", "1"), pad_xy("C5", "1"), pad_xy("C4", "1")
    v8 = Via("+3V3", LEFT_X, c4[1])
    t += _seg("+3V3", [l1, (l1[0], c8[1]), c8, c5, (3.2, c5[1]), (3.2, c4[1]), c4, (v8.x, v8.y)], w)
    r2, r3 = pad_xy("R2", "2"), pad_xy("R3", "2")
    rail = 10.45
    v10 = Via("+3V3", LEFT_X, rail)
    t += _seg("+3V3", [r2, (r2[0], rail), (v10.x, rail)], w)
    t += _seg("+3V3", [(r3[0], rail), r3], w)
    # the LED's anode: down, then left to the trunk
    com = pad_xy("D1", "2")
    v20 = Via("+3V3", com[0], 20.1)
    t += _seg("+3V3", [com, (v20.x, v20.y)], w)
    t += _seg("+3V3", [(v20.x, v20.y), (LEFT_X, v20.y)], w, "B.Cu")
    t += _seg("+3V3", [(vb.x, vb.y), (v8.x, v8.y)], w, "B.Cu")
    v += [vb, v8, v10, v20]
    # bottom: key_board's B.Cu trunk under J1, joining the left trunk to the right one, with a via
    # up to U4 and C7
    vu = Via("+3V3", 13.4, 29.2)
    t += _seg("+3V3", [(vb.x, vb.y), (LEFT_X, 29.3), (5.6, 29.3), (8.0, 31.7), (19.6, 31.7),
                       (EDGE_X, 30.95), (EDGE_X, 17.45)], w, "B.Cu")
    t += _seg("+3V3", [(vu.x, 31.7), (vu.x, vu.y)], w, "B.Cu")
    c7, u4v = kb.pad_xy("C7", "1"), kb.pad_xy("U4", "8")
    t += _seg("+3V3", [(vu.x, vu.y), (vu.x, 23.6), c7, u4v], w)
    v += [vu]
    # R4 and R5 (the I2C pull-ups) from a via on the right trunk
    vq = Via("+3V3", EDGE_X, 22.4)
    r4p, r5p = kb.pad_xy("R4", "2"), kb.pad_xy("R5", "2")
    t += _seg("+3V3", [(vq.x, vq.y), (vq.x, r5p[1]), r5p], w)
    t += _seg("+3V3", [(vq.x, r4p[1]), r4p], w)
    v += [vq]
    # right
    tp5, c11 = kb.pad_xy("TP1", "5"), pad_xy("C11", "1")
    vr0, vr1 = Via("+3V3", EDGE_X, 17.45), Via("+3V3", EDGE_X, 2.6)
    t += _seg("+3V3", [tp5, (tp5[0], vr0.y), (vr0.x, vr0.y)], w)
    t += _seg("+3V3", [(vr0.x, vr0.y), (vr1.x, vr1.y)], w, "B.Cu")
    t += _seg("+3V3", [(vr1.x, vr1.y), (14.9, vr1.y), (14.9, c11[1]), c11], w)
    v += [vr0, vr1]
    return t, v


def left_hand_route():
    """CHIP_PU and GPIO0 out of the crowded left column: EN left along y 8.3 to R2/C6 and on to
    debug pad 1; GPIO0 down beside the chip, left to R3 and down to the button."""
    en, b0 = pin_xy("CHIP_PU"), pin_xy(gpio_pin(GPIO["BUTTON"]))
    r2, c6, r3 = pad_xy("R2", "1"), pad_xy("C6", "1"), pad_xy("R3", "1")
    t = _seg("EN", [en, (7.6, en[1]), (7.6, 8.3), (c6[0], 8.3), c6], SIG_W)
    t += _seg("EN", [(r2[0], 8.3), r2], SIG_W)
    # EN to debug pad 1 can't cross the RF corner or the USB pair on F.Cu: down to B.Cu beside C6,
    # down the left, across below the chip (and below Mu's face on the back), up the right strip
    tp1 = kb.pad_xy("TP1", "1")
    va, vb = Via("EN", 6.15, 8.75), Via("EN", 17.4, 5.4)
    t += _seg("EN", [(va.x, 8.3), (va.x, va.y)], SIG_W)
    t += _seg("EN", [(va.x, va.y), (3.6, 11.3), (3.6, 15.0), (vb.x, 15.0), (vb.x, vb.y)], SIG_W, "B.Cu")
    t += _seg("EN", [(vb.x, vb.y), (18.9, vb.y), (18.9, tp1[1]), tp1], SIG_W)
    sw = pad_xy("SW1", "2")
    t += _seg("BUTTON", [b0, (7.95, b0[1]), (7.95, r3[1]), r3, (2.6, r3[1]), (2.6, 16.3), sw], SIG_W)
    return t, [va, vb]


def right_hand_route():
    """The right column fanned out to the debug pads and the ATECC: RX (pin 39) and TX (pin 38)
    into debug pads 2 and 3; SDA (pin 37) down past them, into the one lane between H1 and the
    pads (x 18.85), to U4 and R4; SCL from GPIO21 alongside D+ and down the channel left of H1,
    under it to U4 and R5 (key_board's SCL route)."""
    t = []
    rx, tx, sda = (pin_xy(gpio_pin(GPIO[k])) for k in ("UART_RX", "UART_TX", "SDA"))
    tp2, tp3 = kb.pad_xy("TP1", "2"), kb.pad_xy("TP1", "3")
    t += _seg("UART_RX", [rx, (18.7, rx[1]), (18.7, tp2[1]), tp2], SIG_W)
    t += _seg("UART_TX", [tx, (18.3, tx[1]), (18.3, tp3[1]), tp3], SIG_W)
    u4sda, r4 = kb.pad_xy("U4", "5"), kb.pad_xy("R4", "1")
    t += _seg("SDA", [sda, (17.9, sda[1]), (17.9, 15.9), (18.85, 16.85), (18.85, u4sda[1]),
                      (18.85, r4[1]), r4], SIG_W)
    t += _seg("SDA", [(18.85, u4sda[1]), u4sda], SIG_W)
    scl = pin_xy(gpio_pin(GPIO["SCL"]))
    u4scl, r5 = kb.pad_xy("U4", "6"), kb.pad_xy("R5", "1")
    t += _seg("SCL", [scl, (scl[0], 14.8), (12.65, round(14.8 + scl[0] - 12.65, 4)), (12.65, 21.35),
                      (16.5, 21.35), (u4scl[0], 21.78), u4scl, (u4scl[0], 24.0), (18.96, r5[1]), r5], SIG_W)
    return t, []


def led_hand_route():
    """Each LED cathode to its resistor, and each resistor to its GPIO (the anode is a trunk's)."""
    t = []
    for k, ref, pad in (("B", "R8", "3"), ("G", "R7", "4"), ("R", "R6", "1")):
        t += _seg(f"LED_{k}K", [pad_xy("D1", pad), pad_xy(ref, "1")], SIG_W)
    # pins 15/16/17 (R/G/B) down from the bottom row, each turning left above the next
    for k, ref, y, x in (("R", "R6", 13.0, 6.5), ("G", "R7", 13.5, None), ("B", "R8", 14.0, None)):
        p, r = pin_xy(gpio_pin(GPIO[f"LED_{k}"])), pad_xy(ref, "2")
        if x is None:
            t += _seg(f"LED_{k}", [p, (p[0], y), (r[0], y), r], SIG_W)
        else:
            t += _seg(f"LED_{k}", [p, (p[0], y), (x, y), (x, r[1]), r], SIG_W)
    return t, []


USB_W = kb.USB_W


def usb_hand_route():
    """The USB pair from pins 25/26 down, then diagonally (0.57 mm apart) into U3's top pins 6/4,
    and from U3's bottom pins into J1, as on key_board. U3's VBUS pin (5) sits between the lines, so
    it hops left across D- on B.Cu."""
    dm0, dp0 = pin_xy(gpio_pin(GPIO['USB_DM'])), pin_xy(gpio_pin(GPIO['USB_DP']))
    u1_, u3_, u4_, u6_ = (kb.pad_xy("U3", n) for n in "1346")
    j2, j3 = kb.pad_xy("J1", "2"), kb.pad_xy("J1", "3")
    ya = 14.0
    dm = [dm0, (dm0[0], ya), (u6_[0], round(ya + dm0[0] - u6_[0], 4)), u6_, u1_, j2]
    yb = round(ya + 0.4, 4)
    dp = [dp0, (dp0[0], yb), (u4_[0], round(yb + dp0[0] - u4_[0], 4)), u4_, u3_,
          (u3_[0], round(j3[1] - 2.45, 4)), (j3[0], round(j3[1] - 1.5, 4)), j3]
    t = _seg("USB_DM", dm, USB_W) + _seg("USB_DP", dp, USB_W)
    u5, u2_ = kb.pad_xy("U3", "5"), kb.pad_xy("U3", "2")
    v1 = Via("VBUS", u5[0], round(u5[1] - 2.0, 4))         # key_board's: clear of the back text
    v2 = Via("VBUS", 8.75, round(u5[1] - 1.5, 4))
    g = Via("GND", u2_[0], round(u2_[1] + 1.0, 4))
    t += [Track("VBUS", "F.Cu", *u5, v1.x, v1.y, 0.3), Track("VBUS", "B.Cu", v1.x, v1.y, v2.x, v2.y, 0.3),
          Track("GND", "F.Cu", *u2_, g.x, g.y, 0.3)]
    return t, [v1, v2, g]


def signal_hand_route():
    """Every hand route but the chip-side power and USB (power_hand_route, usb_hand_route)."""
    t, v = [], []
    for f in (rf_hand_route, crystal_hand_route, trunk_hand_route, left_hand_route,
              right_hand_route, led_hand_route):
        a, b = f()
        t += a
        v += b
    return t, v


def edge_vias():
    """GND vias around the antenna clearance and the RF line (a fence), and in the pockets the
    RF and crystal tracks close off."""
    pts = [(0.6, 3.2), (0.6, 4.6), (5.5, 3.2), (5.5, 4.6), (5.5, 0.8), (8.0, 3.45), (8.15, 1.2),
           # pour pockets the routes close off: C9's, C15's, beside debug pad G, right of H1
           (6.45, 7.65), (11.6, 14.4), (18.7, 12.45), (17.6, 21.2)]
    return [Via("GND", x, y) for x, y in pts]


def stitch_vias(circuit, placement, tracks, vias, avoid=(), avoid_at=None):
    """key_board's stitcher, kept out of the antenna clearance (it knows nothing of rule areas)."""
    x0, y0, x1, y1 = ANT_CLEAR
    return kb.stitch_vias(circuit, placement, tracks, vias, avoid=list(avoid) + [(x0, y0, x1, y1)],
                          avoid_at=avoid_at)


on_art = kb.on_art
vias_under_text = kb.vias_under_text
art_key = kb.art_key

TITLE = "tinycrypt key bare v0.1"
BACK_TEXT = (("Mu", 1.0, 0.0), (kb.CREDIT, 1.0, 1.6), (TITLE, 1.0, 3.2))


def back_text_boxes(margin=0.1):
    return [(x0 - margin, y0 - margin, x1 + margin, y1 + margin)
            for x0, y0, x1, y1 in (kb._text_box(t, kb.ART_CX, kb.BACK_TEXT_Y + dy, size) for t, size, dy in BACK_TEXT)]


def silk_key(circuit, placement):
    p = lambda ref, n: kb._pad_xy(circuit, placement, ref, n)
    out = []
    for k, (n, text) in enumerate(J1_LABELS.items()):
        x, y = p("J1", n)
        dx = {"D-": -0.3, "CC": 0.3}.get(text, 0.0)
        out.append((text, round(x + dx, 3), round(y - (1.35 if k % 2 == 0 else 2.35), 3), 0.8))
    for n, text in DEBUG_LABELS.items():
        x, y = p("TP1", n)
        out.append((text, round(x - 1.05, 3), round(y, 3), 0.8, "F.SilkS", 90))
    sx, sy, _ = placement.at["SW1"]
    out.append(("BTN", sx, round(sy + 3.3, 3), 0.8))
    out += [(t, kb.ART_CX, round(kb.BACK_TEXT_Y + dy, 3), size, "B.SilkS") for t, size, dy in BACK_TEXT]
    return out


def front_text_boxes(circuit, placement, margin=0.1):
    out = []
    for t, x, y, size, *rest in silk_key(circuit, placement):
        if rest and rest[0] != "F.SilkS":
            continue
        x0, y0, x1, y1 = kb._text_box(t, x, y, size, rest[1] if len(rest) > 1 else 0)
        out.append((x0 - margin, y0 - margin, x1 + margin, y1 + margin))
    return out


def silk_avoid(circuit, placement):
    return []


def fab_notes():
    x0, y0, x1, y1 = ANT_CLEAR
    return ([("ANTENNA GROUND CLEARANCE: NO COPPER, ANY LAYER", 11.0, -1.0, 0.6, "Dwgs.User")],
            [(x0, y0, x1, y1, "Dwgs.User")])


def pad_extra():
    """U1: the exposed pad (3.4 mm) gets a -0.4 mm paste margin (58 % coverage, against the chip
    floating on it) and joins the pour solidly, as do its thermal vias."""
    from board.sexpr import Sym

    def u1(pad):
        if pad.number == "57":
            extra = [[Sym("zone_connect"), 2]]
            if pad.kind == "smd":
                extra.append([Sym("solder_paste_margin"), -0.4])
            return extra
        return []
    solid = lambda pad: [[Sym("zone_connect"), 2]]
    # The LED's own anode-to-cathode gap is 0.19 mm, under the Power class's 0.2: its pads use the
    # default 0.15 (JLCPCB's minimum is 0.1).
    led = lambda pad: [[Sym("clearance"), 0.15]]
    # The antenna's GND pads sit in notches of its clearance, reached by the pour from one side
    # only, and C18's GND pad is hemmed in by the crystal: solid connections, not thermal spokes.
    gnd_solid = lambda pad: [[Sym("zone_connect"), 2]] if pad.number in ("1", "2", "3") else []
    # GND pads hemmed in by tracks (U2's as on key_board)
    pad2 = lambda pad: [[Sym("zone_connect"), 2]] if pad.number == "2" else []
    return {"U1": u1, "D1": led, "AE1": gnd_solid, "C18": solid, "U2": pad2, "C13": pad2, "C9": pad2, "C6": pad2, "H2": solid}


def pins_h():
    lines = ["/* Generated by hardware/key-board/build.py from board/bare_board.py: do not edit. */",
             "#pragma once", "",
             "/* Bare-chip board (CRYPT-24). Debug pads TP1, from the antenna end: "
             + ", ".join(DEBUG_LABELS.values()) + ".",
             "   EN is CHIP_PU; RX/TX are the console UART on GPIO matrix pins. The LED is a",
             "   common-anode RGB: drive a cathode pin low to light that colour. */", ""]
    for k in ("BUTTON", "LED_R", "LED_G", "LED_B", "SDA", "SCL", "USB_DM", "USB_DP", "UART_TX", "UART_RX"):
        lines.append(f"#define TC_PIN_{k} {GPIO[k]}")
    return "\n".join(lines) + "\n"
