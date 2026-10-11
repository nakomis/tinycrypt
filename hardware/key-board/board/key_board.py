"""tinycrypt key board (CRYPT-16): ESP32-S3-MINI-1 + ATECC608B + WS2812B + button, fed over a
captive USB cable on a JST-SH socket. See DESIGN.md for the parts and the circuit.

Board frame (KiCad, y down): x 0..W, y 0..L. The module sits at y = 0 with its antenna flush with
that edge; J1 sits at y = L, its opening facing out across that edge.
"""
import math
from pathlib import Path

from board.kicad_pcb import Track, Via
from board.netlist import Circuit
from board.parts import ESP32_ANTENNA, part
from board.placement import Placement, rotate

HERE = Path(__file__).resolve().parent.parent
W, L = 21.0, 40.0
CX = W / 2
MOD_Y = 12.8                         # module origin: its body top (-12.8) lands on the y = 0 edge
ANT_Y = MOD_Y + ESP32_ANTENNA[3]     # 5.05: bottom of the antenna strip = end of the keep-out

# GPIO choice (module pad -> net). USB is fixed by the silicon; the rest were picked from the pad
# positions: SDA/SCL from the pads nearest U4 (right side), the LED from the bottom-row pad
# (21) directly above D1. None is a strapping pin (0/3/45/46 other than the button on GPIO0 by design),
# a flash/PSRAM pin (26-32; 33-37 are eFuse-dependent SPI pins) or the octal clock pair (47/48).
GPIO = {"BUTTON": 0, "LED": 17, "SDA": 38, "SCL": 21, "USB_DM": 19, "USB_DP": 20,
        "UART_TX": 43, "UART_RX": 44}
# Module pin names for those GPIOs, as Espressif's datasheet (and the EasyEDA symbol) spell them.
_MODPIN = {43: "TXD0", 44: "RXD0"}


def modpin(gpio):
    return _MODPIN.get(gpio, f"IO{gpio}")


# J1 pin order (our choice; printed on the silkscreen): 1 VBUS, 2 D-, 3 D+, 4 CC, 5 GND, the
# usual order for USB pigtails. Pads 6/7 are the shell tabs, soldered to GND.
J1_PINS = {"1": "VBUS", "2": "USB_DM", "3": "USB_DP", "4": "CC", "5": "GND"}
J1_LABELS = {"1": "V", "2": "D-", "3": "D+", "4": "CC", "5": "G"}
# Debug pads, top to bottom along the right edge, next to TXD0/RXD0 (module pads 39/40).
DEBUG_PINS = {"1": "UART_RX", "2": "UART_TX", "3": "+3V3", "4": "GND"}
DEBUG_LABELS = {"1": "RX", "2": "TX", "3": "3V3", "4": "G"}


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

    # 3.3 V: AP2112K, EN tied to VIN, 1 uF in, 10 uF + 100 nF out.
    c.connect(vbus, (u2, "VIN"), (u2, "EN"))
    c.connect(gnd, (u2, "GND"))
    c.connect(v33, (u2, "VOUT"))
    for key, ref, net in (("C1U", "C1", vbus), ("C10U", "C2", v33), ("C100N", "C3", v33)):
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

    # WS2812B-2020: VDD 3V3 + 100 nF, DIN from a GPIO, DOUT unconnected.
    c.connect(v33, (d1, "VDD"))
    c.connect(gnd, (d1, "GND"))
    c.connect("LED_DIN", (d1, "DI"), (u1, modpin(GPIO["LED"])))
    cap = c.add("C100N", "C8")
    c.connect(v33, (cap, "1"))
    c.connect(gnd, (cap, "2"))

    # Debug pads: UART0 (GPIO43/44), 3V3, GND.
    c.connect("UART_TX", (u1, modpin(GPIO["UART_TX"])))
    c.connect("UART_RX", (u1, modpin(GPIO["UART_RX"])))
    for pad, net in DEBUG_PINS.items():
        c.connect(net, (tp, pad))

    # Mounting holes: plated, on GND.
    c.connect(gnd, (c.instances["H1"], "1"), (c.instances["H2"], "1"))
    return c


# (x, y, rotation) of each part. See DESIGN.md "Board" and the module's pad map: left column
# pads 1-15 (GND, GND, 3V3, IO0..IO11) top to bottom, bottom row 16-30 (IO12..IO48) left to right,
# right column 31-45 (IO35..EN) bottom to top, top row 46-60 GND under the antenna edge.
AT = {
    "U1": (CX, MOD_Y, 0),
    # left strip beside the module: 3V3 decoupling at pad 3 (y 8.55), GPIO0 pull-up
    "C5": (1.4, 7.4, 90), "C4": (1.4, 10.0, 90), "R3": (1.4, 12.6, 90),
    # right strip: EN RC at pad 45 (y 6.85), then the debug pads level with pads 39/40
    "R2": (19.6, 6.85, 0), "C6": (19.6, 8.6, 0), "TP1": (20.1, 14.81, 0),
    # lower left: button with the LED beside it (one enclosure window), then the LDO and its caps
    "SW1": (3.0, 24.6, 90), "D1": (7.3, 23.2, 0), "C8": (7.3, 25.6, 0),
    "U2": (3.4, 31.2, 90), "C1": (6.6, 32.6, 0), "C2": (2.4, 33.7, 0), "C3": (0.75, 31.2, 90),
    # centre: the USB path, module pads 23/24 -> U3 -> J1, kept in a clear channel
    "U3": (CX, 29.6, 0), "J1": (CX, L - 3.21, 0), "R1": (16.4, 35.6, 0),
    # lower right: secure element, its pull-ups and decoupling
    "U4": (16.3, 30.0, 0), "C7": (12.9, 27.0, 90), "R4": (19.95, 28.2, 90), "R5": (19.95, 30.4, 90),
    # mounting holes: one under the module end, one beside J1 (off the centre line, which is the
    # USB path)
    "H1": (17.0, 23.3, 0), "H2": (3.0, 37.2, 0),
}


def place_key(circuit):
    at = {r: AT[r] for r in circuit.instances}
    bodies = {r: i.part.body for r, i in circuit.instances.items()}
    return Placement((0.0, 0.0, W, L), at, bodies)


def antenna_keepout():
    """The copper keep-out under the module's PCB antenna, out to the board edge on all three
    sides (Espressif: antenna on the board edge, no copper beneath or beside it)."""
    return [(0.0, 0.0), (W, 0.0), (W, ANT_Y), (0.0, ANT_Y)]


def keepouts():
    return [("antenna", antenna_keepout(), ("F.Cu", "B.Cu"))]


def route_area():
    """The router's boundary: the board minus the antenna strip."""
    return [(0.0, ANT_Y), (W, ANT_Y), (W, L), (0.0, L)]


def thermal_vias():
    """GND vias at the inner corners of the module's 3 x 3 thermal pad (where Espressif's land
    pattern puts them), each touching four of the pads, tying them to the B.Cu pour."""
    return [Via("GND", CX + dx, MOD_Y + dy) for dx in (-0.7, 0.7) for dy in (-0.7, 0.7)]


USB_W = 0.2           # USB Full Speed: no impedance control, just short, paired and stub-free


def usb_hand_route():
    """(tracks, vias) for the USB path, routed by hand so that the pair stays together:
    J1 -> U3 (pins 1/3 in, 6/4 out: flow-through) -> module pads 23/24, all on F.Cu.
    U3's VBUS (pin 5) and GND (pin 2) sit between the two lines, so each drops straight to a via."""
    def pad(ref, key, number):           # exact pad centre: the router wants wire ends on it
        x, y, rot = AT[ref]
        q = next(q for q in part(key).footprint.pads if q.number == number)
        dx, dy = rotate(q.x, q.y, rot)
        return round(x + dx, 4), round(y + dy, 4)

    j2, j3 = pad("J1", "JST5", "2"), pad("J1", "JST5", "3")
    u1, u3, u4, u6 = (pad("U3", "ESD", n) for n in "1346")
    m23, m24 = pad("U1", "ESP32", "23"), pad("U1", "ESP32", "24")
    ux, ut, ub = u1[0] + 0.95, u6[1], u1[1]
    jog = round(ut - 4.4, 4)              # where the pair steps across to the module pads
    dm = [j2, u1, u6, (u6[0], round(jog + 0.95, 4)), (m23[0], jog), m23]
    dp = [j3, (j3[0], round(j3[1] - 1.5, 4)), (u3[0], round(j3[1] - 2.45, 4)), u3, u4, (m24[0], jog), m24]
    tracks = []
    for net, pts in (("USB_DM", dm), ("USB_DP", dp)):
        for (a, b), (c, d) in zip(pts, pts[1:]):
            if (a, b) != (c, d):        # U3.1-U3.6 and U3.3-U3.4 are joined under the package too
                tracks.append(Track(net, "F.Cu", round(a, 4), round(b, 4), round(c, 4), round(d, 4), USB_W))
    u5, u2 = pad("U3", "ESD", "5"), pad("U3", "ESD", "2")
    vbus = Via("VBUS", u5[0], round(ut - 2.35, 4))
    gnd = Via("GND", u2[0], round(ub + 1.2, 4))
    tracks += [Track("VBUS", "F.Cu", *u5, vbus.x, vbus.y, 0.3),
               Track("GND", "F.Cu", *u2, gnd.x, gnd.y, 0.3)]
    return tracks, [vbus, gnd]


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


def stitch_vias(circuit, placement, tracks, vias, pitch=2.0, clear=0.55, avoid=(), avoid_at=None):
    """GND stitching vias on a grid, wherever a via clears every pad, track and via by 'clear'
    (beyond its own 0.3 mm radius) so that both pours reach it, lies outside the 'avoid'
    boxes (x0, y0, x1, y1), such as the back silk text, and avoid_at(x, y) is false."""
    pads = _pad_boxes(circuit, placement)
    placed = list(vias)
    out = []
    r = 0.3
    y = ANT_Y + 1.0
    while y < L - 0.8:
        x = 0.9
        while x < W - 0.8:
            ok = all(_box_dist(x, y, b) >= r + clear for b in pads)
            ok = ok and not any(x0 - 0.4 <= x <= x1 + 0.4 and y0 - 0.4 <= y <= y1 + 0.4 for x0, y0, x1, y1 in avoid)
            ok = ok and not (avoid_at and avoid_at(x, y))
            ok = ok and all(_seg_dist(x, y, t.x1, t.y1, t.x2, t.y2) >= r + t.width / 2 + clear for t in tracks)
            ok = ok and all(math.hypot(x - v.x, y - v.y) >= 2 * r + 0.8 for v in placed)
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
ART_PX = 0.16                         # mm per pixel: 112 x 128 px -> 17.92 x 20.48 mm
ART_TOP = 0.4


def art_key(placement):
    """Mu on the back, centred across the board at the antenna end."""
    from PIL import Image
    w, h = Image.open(ART_PNG).size
    return [(ART_PNG, round(CX - w * ART_PX / 2, 3), ART_TOP, round(h * ART_PX, 3), "back")]


def on_art(x, y, margin=0.45):
    """True if a via at board (x, y) would land on (or within 'margin' of) a white silk pixel of
    the back portrait. Stitching vias keep to the dark areas, so they don't pit Mu's fur."""
    from PIL import Image
    png, ax, ay, h, _ = art_key(None)[0]
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
BACK_TEXT = (("Mu", 1.2, 0.0), (CREDIT, 0.8, 1.9), (TITLE, 0.8, 3.4))


# First line's centre: below hole H1 and the USB VBUS via, above U3's GND via.
BACK_TEXT_Y = 27.45


def _text_box(text, x, y, size):
    """Generous extent of KiCad stroke-font text centred at (x, y)."""
    w = 0.9 * size * len(text) + size
    return (x - w / 2, y - 0.75 * size, x + w / 2, y + 0.75 * size)


def back_text_boxes(margin=0.1):
    """Boxes (x0, y0, x1, y1) of the back text, grown by 'margin'. No via may sit under the ink:
    a tented via's hole still shows through it (one once turned "tinycrypt key" into
    "tinycrypt.key"), so the router and the stitcher are kept out of these boxes."""
    return [(x0 - margin, y0 - margin, x1 + margin, y1 + margin)
            for x0, y0, x1, y1 in (_text_box(t, CX, BACK_TEXT_Y + dy, size) for t, size, dy in BACK_TEXT)]


def vias_under_text(vias):
    """Vias whose 0.6 mm pad touches the back text (grown by 0.1 mm)."""
    return [v for v in vias for x0, y0, x1, y1 in back_text_boxes()
            if x0 - v.size / 2 <= v.x <= x1 + v.size / 2 and y0 - v.size / 2 <= v.y <= y1 + v.size / 2]


def silk_key(circuit, placement):
    """Silk entries (text, x, y, size[, layer, rotation]). Front: J1 pin labels (vertical, between
    J1's pins and U3), debug pad labels, the button label and reference designators where they
    fit. Back: Mu's name, the credit and the board name under the portrait."""
    p = lambda ref, n: _pad_xy(circuit, placement, ref, n)
    out = []
    # J1's pins are 1 mm apart, too close for side-by-side labels: stagger them in two rows.
    for k, (n, text) in enumerate(J1_LABELS.items()):
        x, y = p("J1", n)
        out.append((text, x, y - (1.3 if k % 2 == 0 else 2.4), 0.8))
    for n, text in DEBUG_LABELS.items():
        x, y = p("TP1", n)
        out.append((text, x - 1.15, y, 0.8, "F.SilkS", 90))
    sx, sy, _ = placement.at["SW1"]
    out += [("BTN", sx, sy + 4.4, 0.8)]
    for ref, x, y in (("U2", 7.2, 30.4), ("U3", CX, 27.0), ("U4", 16.3, 34.4), ("J1", 15.8, 38.6),
                      ("D1", 7.3, 21.35)):
        out.append((ref, x, y, 0.8))
    # Under Mu: below the mounting hole at the module end, above the one at the J1 end.
    out += [(t, CX, BACK_TEXT_Y + dy, size, "B.SilkS") for t, size, dy in BACK_TEXT]
    return out


def pins_h():
    """The firmware header: GPIO numbers as wired on this board."""
    lines = ["/* Generated by hardware/key-board/build.py from board/key_board.py: do not edit. */",
             "#pragma once", ""]
    names = {"BUTTON": "BUTTON", "LED": "LED", "SDA": "SDA", "SCL": "SCL", "USB_DM": "USB_DM",
             "USB_DP": "USB_DP", "UART_TX": "UART_TX", "UART_RX": "UART_RX"}
    for k, n in names.items():
        lines.append(f"#define TC_PIN_{n} {GPIO[k]}")
    return "\n".join(lines) + "\n"
