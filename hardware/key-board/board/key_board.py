"""tinycrypt key board (CRYPT-16): ESP32-S3-MINI-1 + ATECC608B + WS2812B + button, fed over a
captive USB cable on a JST-SH socket. See DESIGN.md for the parts and the circuit.

Board frame (KiCad, y down): x 0..W, y 0..L. The board ends where the module's antenna strip
begins, so the antenna (5.05 mm) overhangs the y = 0 edge and every module pad sits on the board.
J1 sits at y = L, its opening facing out across that edge.
"""
import math
from pathlib import Path

from board.kicad_pcb import Track, Via
from board.netlist import Circuit
from board.parts import ESP32_ANTENNA, part
from board.placement import Placement, rotate

HERE = Path(__file__).resolve().parent.parent
W = 21.0
CX = W / 2
MOD_Y = -ESP32_ANTENNA[3]            # 7.75: module origin; its antenna strip ends on the y = 0 edge
ANTENNA = MOD_Y + ESP32_ANTENNA[1]   # -5.05: how far the antenna overhangs the board
L = round(40.0 + ANTENNA, 2)         # 34.95: the v0.1 board less the antenna strip

# GPIO choice (module pad -> net). USB is fixed by the silicon; the rest were picked from the pad
# positions: SDA/SCL from the pads nearest U4 (right side), the LED from the bottom-row pad
# (21) directly above D1. None is a strapping pin (0/3/45/46 other than the button on GPIO0 by design),
# a flash/PSRAM pin (26-32; 33-37 are eFuse-dependent SPI pins) or the octal clock pair (47/48).
GPIO = {"BUTTON": 0, "LED": 17, "SDA": 38, "SCL": 21, "USB_DM": 19, "USB_DP": 20,
        "UART_TX": 43, "UART_RX": 44}
POWER_W = 0.45        # hand-routed power trunks (VBUS and 3V3), mm
# Module pin names for those GPIOs, as Espressif's datasheet (and the EasyEDA symbol) spell them.
_MODPIN = {43: "TXD0", 44: "RXD0"}


def modpin(gpio):
    return _MODPIN.get(gpio, f"IO{gpio}")


# J1 pin order (our choice; printed on the silkscreen): 1 VBUS, 2 D-, 3 D+, 4 CC, 5 GND, the
# usual order for USB pigtails. Pads 6/7 are the shell tabs, soldered to GND.
J1_PINS = {"1": "VBUS", "2": "USB_DM", "3": "USB_DP", "4": "CC", "5": "GND"}
J1_LABELS = {"1": "V", "2": "D-", "3": "D+", "4": "CC", "5": "G"}
# Debug pads, top to bottom along the right edge, next to EN and TXD0/RXD0 (module pads 45, 40,
# 39). EN lets a jumper to G reset the chip; 3V3 comes last, fed from below.
DEBUG_PINS = {"1": "EN", "2": "UART_RX", "3": "UART_TX", "4": "GND", "5": "+3V3"}
DEBUG_LABELS = {"1": "EN", "2": "RX", "3": "TX", "4": "G", "5": "3V3"}


def build_key():
    c = Circuit()
    u1 = c.add("ESP32", "U1")
    u2 = c.add("LDO", "U2")
    u3 = c.add("ESD", "U3")
    u4 = c.add("ATECC", "U4")
    j1 = c.add("JST5", "J1")
    sw = c.add("SW", "SW1")
    d1 = c.add("LED", "D1")
    tp = c.add("DEBUG", "TP1")
    c.add("MH", "H1")
    c.add("MH", "H2")
    gnd, v33, vbus = "GND", "+3V3", "VBUS"

    # USB in: J1 -> USBLC6 (I/O1 = pins 1/6 on D-, I/O2 = pins 3/4 on D+, flow-through) -> module.
    for pad, net in J1_PINS.items():
        c.connect(net, (j1, pad))
    c.connect(gnd, (j1, "6"), (j1, "7"))
    c.connect("USB_DM", (u3, "1"), (u3, "6"), (u1, modpin(GPIO["USB_DM"])))
    c.connect("USB_DP", (u3, "3"), (u3, "4"), (u1, modpin(GPIO["USB_DP"])))
    c.connect(vbus, (u3, "5"))
    c.connect(gnd, (u3, "2"))
    c.connect(gnd, (c.add("R5K1", "R1"), "2"))
    c.connect("CC", (c.instances["R1"], "1"))

    # 3.3 V: AP2112K, EN tied to VIN, 10 uF in, 10 uF + 100 nF out.
    c.connect(vbus, (u2, "VIN"), (u2, "EN"))
    c.connect(gnd, (u2, "GND"))
    c.connect(v33, (u2, "VOUT"))
    for key, ref, net in (("C10U", "C1", vbus), ("C10U", "C2", v33), ("C100N", "C3", v33)):
        cap = c.add(key, ref)
        c.connect(net, (cap, "1"))
        c.connect(gnd, (cap, "2"))

    # Module: 3V3 with 10 uF + 100 nF; EN RC (10k to 3V3, 1 uF to GND); every GND pad.
    c.connect(v33, (u1, "3V3"))
    c.connect(gnd, (u1, "GND"))
    for key, ref in (("C10U", "C4"), ("C100N", "C5")):
        cap = c.add(key, ref)
        c.connect(v33, (cap, "1"))
        c.connect(gnd, (cap, "2"))
    c.connect("EN", (u1, "EN"), (c.add("R10K", "R2"), "1"), (c.add("C1U", "C6"), "1"))
    c.connect(v33, (c.instances["R2"], "2"))
    c.connect(gnd, (c.instances["C6"], "2"))

    # Button on GPIO0 (presence + download strap), 10k pull-up. The switch's contact bars pair
    # its four pads two by two; diagonal pads (2 = B and 3 = C) are on different bars whichever
    # way they pair, so the switch cannot short GPIO0 to GND.
    c.connect("BUTTON", (u1, modpin(GPIO["BUTTON"])), (sw, "B"), (c.add("R10K", "R3"), "1"))
    c.connect(v33, (c.instances["R3"], "2"))
    c.connect(gnd, (sw, "C"))

    # ATECC608B: VCC 8, GND 4, SDA 5, SCL 6 (pins 1-3 and 7 are NC); 4k7 pull-ups; 100 nF.
    c.connect(v33, (u4, "VCC"))
    c.connect(gnd, (u4, "GND"))
    c.connect("SDA", (u4, "SDA"), (u1, modpin(GPIO["SDA"])), (c.add("R4K7", "R4"), "1"))
    c.connect("SCL", (u4, "SCL"), (u1, modpin(GPIO["SCL"])), (c.add("R4K7", "R5"), "1"))
    c.connect(v33, (c.instances["R4"], "2"), (c.instances["R5"], "2"))
    cap = c.add("C100N", "C7")
    c.connect(v33, (cap, "1"))
    c.connect(gnd, (cap, "2"))

    # WS2812B-2020: VDD 3V3 + 100 nF, DIN from a GPIO through 330R (series damping), DOUT
    # unconnected.
    c.connect(v33, (d1, "VDD"))
    c.connect(gnd, (d1, "GND"))
    r6 = c.add("R330", "R6")
    c.connect("LED_DIN", (d1, "DI"), (r6, "1"))
    c.connect("LED_GPIO", (r6, "2"), (u1, modpin(GPIO["LED"])))
    cap = c.add("C100N", "C8")
    c.connect(v33, (cap, "1"))
    c.connect(gnd, (cap, "2"))

    # Debug pads: EN, UART0 (GPIO43/44), GND, 3V3.
    c.connect("UART_TX", (u1, modpin(GPIO["UART_TX"])))
    c.connect("UART_RX", (u1, modpin(GPIO["UART_RX"])))
    for pad, net in DEBUG_PINS.items():
        c.connect(net, (tp, pad))

    # Mounting holes: plated, on GND.
    c.connect(gnd, (c.instances["H1"], "1"), (c.instances["H2"], "1"))
    return c


# (x, y, rotation) of each part. See DESIGN.md "Board" and the module's pad map: left column
# pads 1-15 (GND, GND, 3V3, IO0..IO11) top to bottom, bottom row 16-30 (IO12..IO48) left to right,
# right column 31-45 (IO35..EN) bottom to top, top row 46-60 GND along the y = 0 edge.
# Parts keep at least 1 mm from the long edges (JLCPCB's assembly rails) except R2/C6, R4/R5 and
# the debug pads in the 2.8 mm strip right of the module, and the mounting-hole rings.
AT = {
    "U1": (CX, MOD_Y, 0),
    # left strip: 3V3 decoupling at pad 3 (y 3.5), the GPIO0 pull-up
    "C5": (1.5, 3.0, 90), "C4": (1.5, 5.65, 270), "R3": (1.5, 8.3, 90),
    # right strip: EN RC at pad 45 (y 1.8), then the debug pads (EN, RX, TX, G, 3V3)
    "R2": (19.6, 2.6, 0), "C6": (19.6, 4.3, 180), "TP1": (19.9, 11.0, 0),
    # lower left: button with the LED beside it (one enclosure window), then the LDO and its caps
    "SW1": (3.6, 19.55, 90), "R6": (7.65, 16.2, 0), "D1": (7.65, 18.15, 180), "C8": (7.65, 20.55, 0),
    "U2": (4.3, 26.3, 0), "C3": (1.5, 25.6, 270), "C2": (1.5, 28.1, 270), "C1": (6.8, 27.3, 270),
    # centre: the USB path, module pads 23/24 -> U3 -> J1, kept in a clear channel
    "U3": (CX, 24.55, 0), "J1": (CX, round(L - 3.21, 2), 0), "R1": (16.4, 30.55, 0),
    # lower right: secure element, its decoupling and pull-ups
    "U4": (16.3, 25.25, 0), "C7": (12.9, 22.6, 90), "R4": (19.6, 23.45, 90), "R5": (19.6, 25.6, 90),
    # mounting holes: one just past the module, one beside J1 (off the centre line, the USB path)
    "H1": (16.2, 18.8, 0), "H2": (3.0, 32.15, 0),
}


def place_key(circuit):
    at = {r: AT[r] for r in circuit.instances}
    bodies = {r: i.part.body for r, i in circuit.instances.items()}
    return Placement((0.0, 0.0, W, L), at, bodies)


def keepouts():
    """No copper rule areas: the antenna overhangs the board, so nothing of the board lies under
    or beside it (Espressif's preferred placement, antenna outside the base board)."""
    return []


def route_area():
    return [(0.0, 0.0), (W, 0.0), (W, L), (0.0, L)]


# Areas where the router may put nothing on B.Cu: under the USB pair, and under Mu's face (the
# back portrait), so that no track shows through her silkscreen there.
USB_ZONE = (9.3, 14.75, 11.7, 29.75)
FACE_ZONE = (5.0, 3.5, 17.0, 14.0)


def layer_keepouts():
    return [("B.Cu", *USB_ZONE), ("B.Cu", *FACE_ZONE)]


def edge_vias():
    """GND stitching vias in the side strips at the antenna end, where the module's top-row GND
    pads meet the board edge (outside Mu's outline on the left, in her dark corner on the right)."""
    edge = [(0.8, 0.8), (2.2, 1.0), (18.9, 0.8), (20.2, 0.8)]
    # and into three pockets of F.Cu pour that the power and signal tracks close off
    pockets = [(0.7, 6.2), (0.75, 26.4), (18.5, 3.9), (19.45, 21.75), (4.3, 26.65)]
    return [Via("GND", x, y) for x, y in edge + pockets]


USB_W = 0.2           # USB Full Speed: no impedance control, just short, paired and stub-free


def pad_xy(ref, number):
    """Exact board position of a pad centre (the router wants wire ends on it)."""
    x, y, rot = AT[ref]
    q = next(q for q in part(_KEY[ref]).footprint.pads if q.number == number)
    dx, dy = rotate(q.x, q.y, rot)
    return round(x + dx, 4), round(y + dy, 4)


_KEY = {"R1": "R5K1", "U1": "ESP32", "U2": "LDO", "U3": "ESD", "U4": "ATECC", "J1": "JST5", "C1": "C10U", "C2": "C10U",
        "C3": "C100N", "C4": "C10U", "C5": "C100N", "C6": "C1U", "C7": "C100N", "C8": "C100N", "D1": "LED",
        "R2": "R10K", "R3": "R10K", "R4": "R4K7", "R5": "R4K7", "TP1": "DEBUG"}


def _segments(net, layer, pts, width):
    return [Track(net, layer, a, b, c, d, width) for (a, b), (c, d) in zip(pts, pts[1:]) if (a, b) != (c, d)]


def usb_hand_route():
    """(tracks, vias) for the USB path, routed by hand so that the pair stays together:
    J1 -> U3 (pins 1/3 in, 6/4 out: flow-through) -> module pads 23/24, all on F.Cu.
    U3's VBUS (pin 5) and GND (pin 2) sit between the two lines, so each drops straight to a via.
    VBUS must cross D- to reach the rest of its net, so it hops 1.75 mm on B.Cu to a via left of
    D-, the only B.Cu under the pair."""
    j2, j3 = pad_xy("J1", "2"), pad_xy("J1", "3")
    u1, u3, u4, u6 = (pad_xy("U3", n) for n in "1346")
    m23, m24 = pad_xy("U1", "23"), pad_xy("U1", "24")
    ut, ub = u6[1], u1[1]
    jog = round(ut - 4.4, 4)              # where the pair steps across to the module pads
    dm = [j2, u1, u6, (u6[0], round(jog + 0.95, 4)), (m23[0], jog), m23]
    dp = [j3, (j3[0], round(j3[1] - 1.5, 4)), (u3[0], round(j3[1] - 2.45, 4)), u3, u4, (m24[0], jog), m24]
    tracks = _segments("USB_DM", "F.Cu", dm, USB_W) + _segments("USB_DP", "F.Cu", dp, USB_W)
    u5, u2 = pad_xy("U3", "5"), pad_xy("U3", "2")
    # v1 just below the back portrait (a via would pit her fur), above U3's VBUS pin
    v1, v2 = Via("VBUS", u5[0], round(jog + 2.4, 4)), Via("VBUS", 8.75, round(jog + 2.9, 4))
    gnd = Via("GND", u2[0], round(ub + 1.0, 4))
    tracks += [Track("VBUS", "F.Cu", *u5, v1.x, v1.y, 0.3),
               Track("VBUS", "B.Cu", v1.x, v1.y, v2.x, v2.y, 0.3),
               Track("GND", "F.Cu", *u2, gnd.x, gnd.y, 0.3)]
    return tracks, [v1, v2, gnd]


def power_hand_route():
    """(tracks, vias) for the power trunks, POWER_W wide.

    VBUS: J1.1 -> C1 (10 uF) -> U2 EN -> under U2's GND pin -> U2 VIN, plus the hop from U3's
    VBUS via. 3V3: U2 VOUT -> C3/C2 -> via B -> B.Cu up the left strip (outside Mu's outline) ->
    via T -> C4/C5 -> module pad 3. The right half (U4, C7, R4, R5, the debug pad) is fed from
    the same B.Cu trunk under J1's body, clear of the B.Cu under the USB pair."""
    w = POWER_W
    j1, c1, en, vin = pad_xy("J1", "1"), pad_xy("C1", "1"), pad_xy("U2", "3"), pad_xy("U2", "1")
    _, v2, _ = usb_hand_route()[1]
    t = _segments("VBUS", "F.Cu", [j1, (j1[0], 27.2), (7.9, c1[1]), c1, (5.6, c1[1]), en,
                                   (en[0], 28.6), (vin[0], 28.6), vin], w)
    t += _segments("VBUS", "F.Cu", [(v2.x, v2.y), (v2.x, 26.95), (j1[0], 27.2)], w)

    vout, c3, c2, c4, c5, p3 = (pad_xy("U2", "5"), pad_xy("C3", "1"), pad_xy("C2", "1"),
                                pad_xy("C4", "1"), pad_xy("C5", "1"), pad_xy("U1", "3"))
    via_b, via_t = Via("+3V3", 1.6, 24.0), Via("+3V3", 2.45, 4.2)
    spine = 2.4
    t += _segments("+3V3", "F.Cu", [vout, (spine, vout[1]), (spine, c2[1]), c2], w)
    t += _segments("+3V3", "F.Cu", [(spine, vout[1]), (spine, via_b.y), (via_b.x, via_b.y), c3], w)
    t += _segments("+3V3", "B.Cu", [(via_t.x, via_t.y), (1.6, 5.05), (1.6, 7.2), (1.6, via_b.y)], w)
    t += _segments("+3V3", "F.Cu", [(via_t.x, via_t.y), c4], w)
    t += _segments("+3V3", "F.Cu", [(via_t.x, via_t.y), (via_t.x, p3[1]), p3], w)
    t += _segments("+3V3", "F.Cu", [c5, (via_t.x, p3[1])], w)

    # right half, under J1's body
    via_u, via_q = Via("+3V3", 13.4, 29.2), Via("+3V3", 20.3, 28.4)
    t += _segments("+3V3", "B.Cu", [(1.6, via_b.y), (1.6, 29.3), (5.6, 29.3), (8.0, 31.7), (via_u.x, 31.7),
                                    (19.6, 31.7), (via_q.x, 31.0), (via_q.x, via_q.y)], w)
    t += _segments("+3V3", "B.Cu", [(via_u.x, 31.7), (via_u.x, via_u.y)], w)
    c7, u4v = pad_xy("C7", "1"), pad_xy("U4", "8")
    t += _segments("+3V3", "F.Cu", [(via_u.x, via_u.y), (via_u.x, 23.6), c7, u4v], w)
    r5, r4, tp = pad_xy("R5", "2"), pad_xy("R4", "2"), pad_xy("TP1", "5")
    t += _segments("+3V3", "F.Cu", [(via_q.x, via_q.y), (via_q.x, r5[1]), (via_q.x, r4[1]),
                                    (via_q.x, 16.5), tp], w)
    t += _segments("+3V3", "F.Cu", [(via_q.x, r5[1]), r5], w)
    t += _segments("+3V3", "F.Cu", [(via_q.x, r4[1]), r4], w)
    return t, [via_b, via_t, via_u, via_q]


SIG_W = 0.2           # hand-routed signals


def signal_hand_route():
    """(tracks, vias) for the right-hand signals and the small 3V3 branches, routed by hand in
    lanes between the module's right pad column (x 17.88) and the debug pads (x 19.4):
    RX at x 19.05, TX at 18.65, SDA at 18.25 then 18.85 past H1; SCL down the channel right of
    D+, between H1 and U4, and under U4 to R5. R2's 3V3 comes up the right edge on B.Cu from
    the debug-pad 3V3 lane; R3's 3V3 drops to the left trunk; D1/C8 branch off U2's spine."""
    w = SIG_W
    t = []
    en45, r2en, c6en, tpen = pad_xy("U1", "45"), pad_xy("R2", "1"), pad_xy("C6", "1"), pad_xy("TP1", "1")
    t += _segments("EN", "F.Cu", [en45, (18.4, en45[1]), (r2en[0], 2.25), r2en, c6en, (c6en[0], 4.9), tpen], w)
    rx40, tprx = pad_xy("U1", "40"), pad_xy("TP1", "2")
    t += _segments("UART_RX", "F.Cu", [rx40, (19.05, rx40[1]), (19.05, tprx[1]), tprx], w)
    tx39, tptx = pad_xy("U1", "39"), pad_xy("TP1", "3")
    t += _segments("UART_TX", "F.Cu", [tx39, (18.65, tx39[1]), (18.65, tptx[1]), tptx], w)
    sda, u4sda, r4sda = pad_xy("U1", "34"), pad_xy("U4", "5"), pad_xy("R4", "1")
    t += _segments("SDA", "F.Cu", [sda, (18.25, sda[1]), (18.25, 17.0), (18.85, 17.6), (18.85, u4sda[1]),
                                   (18.85, r4sda[1]), r4sda], w)
    t += _segments("SDA", "F.Cu", [(18.85, u4sda[1]), u4sda], w)
    scl, u4scl, r5scl = pad_xy("U1", "25"), pad_xy("U4", "6"), pad_xy("R5", "1")
    t += _segments("SCL", "F.Cu", [scl, (scl[0], 20.9), (12.65, 21.35), (16.5, 21.35), (u4scl[0], 21.78),
                                   u4scl, (u4scl[0], 24.0), (18.96, r5scl[1]), r5scl], w)

    pw = POWER_W
    vias = []
    # R3 (GPIO0 pull-up): a via at the board edge side, onto the B.Cu trunk under it
    r3 = pad_xy("R3", "2")
    v3 = Via("+3V3", 0.7, 7.2)
    t += _segments("+3V3", "F.Cu", [(v3.x, v3.y), r3], 0.3)
    t += _segments("+3V3", "B.Cu", [(v3.x, v3.y), (1.6, v3.y)], 0.3)
    # R2 (EN pull-up): up the right edge on B.Cu from the debug-pad 3V3 lane
    r2 = pad_xy("R2", "2")
    vs, vr = Via("+3V3", 20.3, 21.6), Via("+3V3", 20.35, 1.75)       # vs below the portrait
    t += _segments("+3V3", "B.Cu", [(vs.x, vs.y), (20.4, 21.5), (20.4, 2.1), (vr.x, vr.y)], 0.3)
    t += _segments("+3V3", "F.Cu", [(vr.x, vr.y), r2], 0.3)
    vias += [v3, vs, vr]
    # D1 and C8: off the 3V3 spine beside U2, above U2 and below SW1
    c8, d1 = pad_xy("C8", "1"), pad_xy("D1", "4")
    t += _segments("+3V3", "F.Cu", [(2.4, 24.0), (2.65, 23.75), (6.2, 23.75), (6.4, 23.55), (6.4, 21.3),
                                    c8, d1], 0.4)
    # CC: down from J1.4 and along under J1's body to R1, low enough to leave the GND pour
    # between J1's pins and U4 open
    cc, r1 = pad_xy("J1", "4"), pad_xy("R1", "1")
    t += _segments("CC", "F.Cu", [cc, (cc[0], 31.3), (15.6, 31.3), r1], w)
    # U1's bottom-right corner GND pad sits in a pocket closed by SDA: tie it to H1's GND ring
    t += _segments("GND", "F.Cu", [(round(CX + 7.0, 4), round(MOD_Y + 7.0, 4)), (17.3, 17.1)], 0.3)
    return t, vias


def _pad_boxes(circuit, placement):
    """(net, xmin, ymin, xmax, ymax) for every copper pad, in board coordinates."""
    out = []
    for ref, inst in circuit.instances.items():
        x, y, rot = placement.at[ref]
        for pad in inst.part.footprint.pads:
            dx, dy = rotate(pad.x, pad.y, rot)
            w, h = (pad.h, pad.w) if round(pad.rot + rot) % 180 == 90 else (pad.w, pad.h)
            out.append((circuit.net_of(ref, pad.number), x + dx - w / 2, y + dy - h / 2,
                        x + dx + w / 2, y + dy + h / 2))
    return out


def _seg_dist(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    t = 0.0 if vx == vy == 0 else max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)))
    return math.hypot(px - ax - t * vx, py - ay - t * vy)


def stitch_vias(circuit, placement, tracks, vias, pitch=1.0, clear=0.5, avoid=(), avoid_at=None):
    """GND stitching vias on a grid, wherever a via clears every pad, track and via by 'clear'
    (beyond its own 0.3 mm radius) so that both pours reach it, lies outside the 'avoid'
    boxes (x0, y0, x1, y1), such as silk text, and avoid_at(x, y) is false (Mu's white fur)."""
    pads = _pad_boxes(circuit, placement)
    placed = list(vias)
    out = []
    r = 0.3
    y = 0.8
    while y < L - 0.7:
        x = 0.8
        while x < W - 0.7:
            ok = all(_box_dist(x, y, b) >= r + clear for b in pads)
            ok = ok and not any(x0 - 0.3 <= x <= x1 + 0.3 and y0 - 0.3 <= y <= y1 + 0.3 for x0, y0, x1, y1 in avoid)
            ok = ok and not (avoid_at and avoid_at(x, y))
            ok = ok and all(_seg_dist(x, y, t.x1, t.y1, t.x2, t.y2) >= r + t.width / 2 + clear for t in tracks)
            ok = ok and all(math.hypot(x - v.x, y - v.y) >= 1.3 for v in placed)
            if ok:
                v = Via("GND", round(x, 3), round(y, 3))
                out.append(v)
                placed.append(v)
            x += pitch
        y += pitch
    return out


def _box_dist(x, y, box):
    _, x0, y0, x1, y1 = box
    return math.hypot(max(x0 - x, 0, x - x1), max(y0 - y, 0, y - y1))


TITLE = "tinycrypt key v0.1"
CREDIT = "Designed by Nakomis/C"
ART_PNG = HERE / "art" / "mu.png"
ART_PX = 0.16                         # mm per pixel: 112 x 129 px -> 17.92 x 20.64 mm
ART_X, ART_TOP = 2.3, 0.4             # left edge (top view) clear of the 3V3 trunk at x 1.6;
                                      # right edge clear of the R2 feed at x 20.4
HOLE_CLEAR = 1.1 + 0.25               # no silk within this radius of a drill centre


def art_key(placement=None):
    """Mu on the back at the module end. H1's ring sits under her (masked on the back), so the
    pixels over its drill are dropped."""
    from PIL import Image
    w, h = Image.open(ART_PNG).size
    holes = [(AT[r][0], AT[r][1], HOLE_CLEAR) for r in ("H1", "H2")]
    return [(ART_PNG, ART_X, ART_TOP, round(h * ART_PX, 3), "back", holes)]


ART_CX = round(ART_X + 112 * ART_PX / 2, 2)    # Mu's centre line; the back text follows it


def on_art(x, y, margin=0.45):
    """True if a via at board (x, y) would land on (or within 'margin' of) a white silk pixel of
    the back portrait. Vias keep to the dark areas, so they don't pit Mu's fur."""
    from PIL import Image
    png, ax, ay, h, *_ = art_key()[0]
    img = Image.open(png).convert("L")
    w = img.width * ART_PX
    px = img.load()
    for ddx in (-margin, 0.0, margin):
        for ddy in (-margin, 0.0, margin):
            lx, ly = x + ddx - ax, y + ddy - ay
            if 0 <= lx < w and 0 <= ly < h:
                col = int((w - lx) / ART_PX)          # mirrored: the art is on the back
                row = int(ly / ART_PX)
                if 0 <= col < img.width and 0 <= row < img.height and px[col, row] >= 128:
                    return True
    return False


def _pad_xy(circuit, placement, ref, number):
    x, y, rot = placement.at[ref]
    pad = next(p for p in circuit.instances[ref].part.footprint.pads if p.number == number)
    dx, dy = rotate(pad.x, pad.y, rot)
    return x + dx, y + dy


# Back text under the portrait: (text, size, offset of its centre below the first line's centre).
BACK_TEXT = (("Mu", 1.0, 0.0), (CREDIT, 1.0, 1.6), (TITLE, 1.0, 3.2))
# First line's centre: below Mu (and hole H1), clear of the USB VBUS via above and U3's GND via
# below.
BACK_TEXT_Y = 22.42


def _text_box(text, x, y, size, rot=0):
    """Extent of KiCad stroke-font text centred at (x, y): measured at about 0.86 x size per
    character, plus the stroke."""
    w, h = 0.86 * size * len(text) + 0.15, 1.2 * size
    if rot % 180 == 90:
        w, h = h, w
    return (x - w / 2, y - h / 2, x + w / 2, y + h / 2)


def back_text_boxes(margin=0.1):
    """Boxes (x0, y0, x1, y1) of the back text, grown by 'margin'. No via may sit under the ink:
    a tented via's hole still shows through it (one once turned "tinycrypt key" into
    "tinycrypt.key"), so the router and the stitcher are kept out of these boxes."""
    return [(x0 - margin, y0 - margin, x1 + margin, y1 + margin)
            for x0, y0, x1, y1 in (_text_box(t, ART_CX, BACK_TEXT_Y + dy, size) for t, size, dy in BACK_TEXT)]


def front_text_boxes(circuit, placement, margin=0.1):
    """The same for the front labels (a via once sat in the "3V3" label)."""
    out = []
    for t, x, y, size, *rest in silk_key(circuit, placement):
        if rest and rest[0] != "F.SilkS":
            continue
        x0, y0, x1, y1 = _text_box(t, x, y, size, rest[1] if len(rest) > 1 else 0)
        out.append((x0 - margin, y0 - margin, x1 + margin, y1 + margin))
    return out


def vias_under_text(vias, boxes=None):
    """Vias whose 0.6 mm pad touches a text box (the back text by default)."""
    boxes = back_text_boxes() if boxes is None else boxes
    return [v for v in vias for x0, y0, x1, y1 in boxes
            if x0 - v.size / 2 <= v.x <= x1 + v.size / 2 and y0 - v.size / 2 <= v.y <= y1 + v.size / 2]


def silk_key(circuit, placement):
    """Silk entries (text, x, y, size[, layer, rotation]). Front: J1 pin labels (staggered between
    J1's pins and U3), debug pad labels, the button label and reference designators where they
    fit, 1.0 mm where there is room and 0.8 mm (JLCPCB's floor) where there isn't. Back: Mu's
    name, the credit and the board name under the portrait."""
    p = lambda ref, n: _pad_xy(circuit, placement, ref, n)
    out = []
    # J1's pins are 1 mm apart, too close for side-by-side labels: stagger them in two rows.
    for k, (n, text) in enumerate(J1_LABELS.items()):
        x, y = p("J1", n)
        dx = {"D-": -0.3, "CC": 0.3}.get(text, 0.0)  # clear of U3's GND via between the lines
        out.append((text, round(x + dx, 3), round(y - (1.35 if k % 2 == 0 else 2.35), 3), 0.8))
    for n, text in DEBUG_LABELS.items():
        x, y = p("TP1", n)
        out.append((text, round(x - 1.05, 3), round(y, 3), 0.8, "F.SilkS", 90))
    sx, sy, _ = placement.at["SW1"]
    out.append(("BTN", sx, round(sy + 3.3, 3), 0.8))       # between SW1's lower pads, below its body
    for ref, x, y, size in (("U2", 4.3, 23.9, 0.8), ("U3", 7.95, 24.55, 1.0), ("U4", 16.3, 29.5, 0.8),
                            ("J1", 16.0, 33.4, 1.0), ("D1", 7.25, 21.9, 0.8)):
        out.append((ref, x, y, size))
    out += [(t, ART_CX, round(BACK_TEXT_Y + dy, 3), size, "B.SilkS") for t, size, dy in BACK_TEXT]
    return out


def silk_avoid(circuit, placement):
    """Boxes where footprint silk lines are dropped: the module outline's right side runs
    through the debug-pad labels."""
    return [b for b, e in zip(front_text_boxes(circuit, placement, margin=0.15),
                              [e for e in silk_key(circuit, placement) if len(e) == 4 or e[4] == "F.SilkS"])
            if e[0] in DEBUG_LABELS.values() and len(e) > 5]


def fab_notes():
    """(text entries, user rectangles) on Dwgs.User: the antenna overhang, drawn and labelled so
    that nobody 'fixes' it."""
    x0, x1 = CX - 7.7, CX + 7.7
    text = [("MODULE ANTENNA OVERHANGS THIS EDGE BY 5.05 mm: INTENTIONAL", CX, -2.5, 0.6, "Dwgs.User")]
    return text, [(x0, ANTENNA, x1, 0.0, "Dwgs.User")]


def pad_extra():
    """Per-pad additions: U1's nine 1.2 mm centre GND pads get a -0.18 mm paste margin (about
    49 % paste coverage; Espressif warns that a full thermal pad lifts the module), and U2's GND
    pin joins the pour solidly rather than through thermal spokes, as do U1's four 0.8 mm corner
    GND pads, which the signal lanes leave room for only one spoke."""
    from board.sexpr import Sym
    def u1(pad):
        if pad.number == "GND" and abs(pad.w - 1.2) < 1e-6:
            return [[Sym("solder_paste_margin"), -0.18]]
        if pad.number == "GND" and abs(pad.w - 0.8) < 1e-6:     # corner pads: hemmed in by tracks
            return [[Sym("zone_connect"), 2]]
        return []
    u2 = lambda pad: [[Sym("zone_connect"), 2]] if pad.number == "2" else []
    return {"U1": u1, "U2": u2}


def pins_h():
    """The firmware header: GPIO numbers as wired on this board."""
    lines = ["/* Generated by hardware/key-board/build.py from board/key_board.py: do not edit. */",
             "#pragma once", "",
             "/* Debug pads TP1, from the antenna end: " + ", ".join(DEBUG_LABELS.values()) + ".",
             "   EN is the chip enable (bridge it to G to reset); RX/TX are UART0. */", ""]
    for k in ("BUTTON", "LED", "SDA", "SCL", "USB_DM", "USB_DP", "UART_TX", "UART_RX"):
        lines.append(f"#define TC_PIN_{k} {GPIO[k]}")
    return "\n".join(lines) + "\n"
