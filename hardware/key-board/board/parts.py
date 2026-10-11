"""Parts catalogue: LCSC/EasyEDA footprints and symbols (plus two local board-only footprints),
keyed by role."""
from dataclasses import dataclass
from pathlib import Path

from board.kicadlib import Footprint, load_footprint, load_symbol_pin_types, load_symbol_pins

HERE = Path(__file__).resolve().parent.parent
LCSC_FP = HERE / "footprints" / "lcsc.pretty"
LCSC_SYM = HERE / "footprints" / "lcsc.kicad_sym"
LOCAL_FP = HERE / "footprints" / "local.pretty"


@dataclass
class PartDef:
    key: str
    value: str
    lcsc: str | None         # None: a board-only footprint (test pads, mounting hole), never in BOM/CPL
    footprint: Footprint
    pins: dict
    pin_types: dict
    extended: bool
    tht: bool
    rotation_offset: float   # degrees added to placement rotation for JLCPCB CPL
    body: tuple              # (xmin, ymin, xmax, ymax): the part's real extent, for placement checks


# key: (value, lcsc, footprint file stem, symbol name, extended, rotation_offset)
# Footprint stems and symbol names are exactly what easyeda2kicad wrote. lcsc None means a local,
# board-only footprint from footprints/local.pretty with pin name = pad number.
_TABLE = {
    "ESP32": ("ESP32-S3-MINI-1-N8", "C2913206", "BULETM-SMD_ESP32-S3-MINI-1-N8", "ESP32-S3-MINI-1-N8", True, 0),
    "ATECC": ("ATECC608B-SSHDA-T", "C1518769", "SOIC-8_L4.9-W3.9-P1.27-LS6.0-BL", "ATECC608B-SSHDA-T", True, 0),
    "JST5": ("SM05B-SRSS-G-TB", "C265419", "CONN-SMD_SM05B-SRSS-G-TB-LF-SN", "SM05B-SRSS-G-TB(LF)(SN)", True, 0),
    "ESD": ("USBLC6-2SC6", "C7519", "SOT-23-6_L2.9-W1.6-P0.95-LS2.8-BL", "USBLC6-2SC6", True, 0),
    "LDO": ("AP2112K-3.3TRG1", "C51118", "SOT-25-5_L2.9-W1.6-P0.95-LS2.8-BL", "AP2112K-3.3TRG1", True, 0),
    "SW": ("TS-1187A-B-A-B", "C318884", "SW-SMD_4P-L5.1-W5.1-P3.70-LS6.5-TL_H1.5", "TS-1187A-B-A-B", False, 0),
    "LED": ("WS2812B-2020-V6", "C52917434", "LED-SMD_4P-L2.2-W2.0-P1.00_WS2815C-2020-4P", "WS2812B-2020-V6", True, 0),
    "R5K1": ("5k1", "C25905", "R0402", "0402WGF5101TCE", False, 0),
    "R10K": ("10k", "C25744", "R0402", "0402WGF1002TCE", False, 0),
    "R4K7": ("4k7", "C25900", "R0402", "0402WGF4701TCE", False, 0),
    "R330": ("330R", "C25104", "R0402", "0402WGF3300TCE", False, 0),
    "C100N": ("100nF", "C1525", "C0402", "CL05B104KO5NNNC", False, 0),
    "C1U": ("1uF", "C52923", "C0402", "CL05A105KA5NQNC", False, 0),
    "C10U": ("10uF", "C19702", "C0603", "CL10A106KP8NNNC", False, 0),
    "DEBUG": ("Debug pads", None, "DebugPads_1x5_P2.54", None, False, 0),
    "MH": ("M2 hole", None, "MountingHole_M2_2.2mm_Pad4.4", None, False, 0),
}

# fetched-name -> our-name, per key. The module symbol calls GPIOn "IOn" and spells the UART0 pins
# TXD0/RXD0 (GPIO43/44), exactly as Espressif's datasheet table does; we keep those names.
ALIASES = {}

# Real extents where the footprint's courtyard is smaller than the part. The EasyEDA module
# footprint's courtyard covers only the pad field (15.4 x 15.4); the module is 15.4 x 20.5 with
# the PCB antenna in the 5.05 mm beyond pads 46-60 (its silkscreen outline: y -12.8 .. 7.7).
# The JST socket's and the SOIC's computed courtyards are padded rectangles; their real extents
# (pads and body) leave room for U4 just above J1's signal pads.
BODY = {"ESP32": (-7.7, -12.8, 7.7, 7.7),
        "JST5": (-3.9, -2.79, 3.9, 3.16),      # tabs x +-3.9, signal pads to y -2.79, housing front 2.83
        "ATECC": (-2.46, -3.5, 2.44, 3.5)}     # body width, pad tips at y +-3.5
# The antenna strip of the module, in footprint coordinates (above the top pad row).
ESP32_ANTENNA = (-7.7, -12.8, 7.7, -7.75)


def _load(key, row):
    value, lcsc, fp_stem, sym, extended, rot = row
    fp = load_footprint((LCSC_FP if lcsc else LOCAL_FP) / f"{fp_stem}.kicad_mod")
    pins = {}
    if sym is None:     # footprint-only part: one passive pin per numbered pad, name = number
        pins = {p.number: [p.number] for p in fp.pads if p.number}
        pin_types = {n: "passive" for n in pins}
    else:
        alias = ALIASES.get(key, {})
        for name, nums in load_symbol_pins(LCSC_SYM, sym).items():
            pins.setdefault(alias.get(name, name), []).extend(nums)
        pin_types = load_symbol_pin_types(LCSC_SYM, sym)
    # Through-hole only if most numbered signal pads are drilled.
    numbered = [p for p in fp.pads if p.number]
    tht = bool(numbered) and sum(p.kind == "thru_hole" for p in numbered) * 2 > len(numbered)
    return PartDef(key, value, lcsc, fp, pins, pin_types, extended, tht, rot, BODY.get(key, fp.courtyard))


# Extended parts whose CPL rotation offset has been checked. EasyEDA-fetched footprints follow
# JLCPCB's own orientation convention, so their offset is 0. The final check is a look at
# JLCPCB's placement preview at order time.
ROTATION_CHECKED = {k for k, row in _TABLE.items() if row[4] and row[5] == 0}

PARTS = {k: _load(k, row) for k, row in _TABLE.items()}


def part(key):
    return PARTS[key]
