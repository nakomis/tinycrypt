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
    # Bare-chip board (CRYPT-24). The chip's footprint is a local copy of the LCSC one ("local/"):
    # exposed pad shrunk to 3.4 mm for Espressif's two-layer power moat, thermal vias on pad 57.
    # The antenna has no symbol: its pins are its pad numbers, because which pad is the feed
    # depends on the side it is fed from (datasheet functions 1 and 2).
    "S3FN8": ("ESP32-S3FN8", "C2913196", "local/QFN-56_ESP32-S3FN8_EP3.4", "ESP32-S3(FN8)", True, 0),
    "XTAL": ("40MHz 10ppm", "C706805", "CRYSTAL-SMD_4P-L2.5-W2.0-BL", "8Z40000055", True, 0),
    "L24N": ("24nH", "C74301", "L0402", "SDCL1005C24NJTDF", True, 0),
    "L2N0": ("2.0nH", "C18216", "L0402", "LQG15HS2N0S02D", True, 0),
    "L2N4": ("2.4nH", "C97997", "L0402", "LQG15HS2N4S02D", True, 0),
    "C1P5": ("1.5pF", "C1552", "C0402", "0402CG1R5C500NT", True, 0),
    "C15P": ("15pF", "C1548", "C0402", "0402CG150J500NT", False, 0),
    "C10N": ("10nF", "C15195", "C0402", "CL05B103KB5NNNC", False, 0),
    "ANT": ("2450AT18D0100E", "C2917718", "ANT-SMD_4P-L3.2-W1.6", None, True, 0),
    "RGB": ("XL-1615RGBC-RF", "C965840", "LED-ARRAY-SMD_4P-L1.6-W1.5-BR_XL-1615RGBC-RF", "XL-1615RGBC-RF", True, 0),
    "R100": ("100R", "C25076", "R0402", "0402WGF1000TCE", False, 0),
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

# 3D models (fetch_models.sh puts them in footprints/lcsc.3dshapes). Each EasyEDA footprint names its
# model and carries its offset and rotation; parts whose footprint names none get one here.
# WS2812B-2020-V6 (C52917434) has no EasyEDA model: the WS2812B-2020 one (C965555) is the same
# 2.0 x 2.0 x 0.8 mm package, and its footprint has the same pin 1-4 layout as ours to within
# 0.05 mm, with zero model offset and rotation.
LCSC_3D = HERE / "footprints" / "lcsc.3dshapes"
MODEL_OVERRIDE = {"LED": "LED-SMD_4P-L2.0-W2.0-H0.8-TL_WS2812B-2020"}
# Model offsets (x, y, z in mm, KiCad's model frame) that replace easyeda2kicad's where its are
# wrong, found from top and side renders: the SOT-23-6 and C0603 offsets push the bodies off their
# pads, the JST model's origin is 2 mm off its centre, and the capacitor models are centred on
# z = 0 (half inside the board), so they are lifted by half their height.
MODEL_OFFSET = {"ESD": (0, 0, 0), "JST5": (-2.0, -0.33, 0),
                "C10U": (0, 0, 0.4), "C100N": (0, 0, 0.25), "C1U": (0, 0, 0.25)}


def _load(key, row):
    value, lcsc, fp_stem, sym, extended, rot = row
    local = fp_stem.startswith("local/") or not lcsc
    fp = load_footprint((LOCAL_FP if local else LCSC_FP) / f"{fp_stem.removeprefix('local/')}.kicad_mod")
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
