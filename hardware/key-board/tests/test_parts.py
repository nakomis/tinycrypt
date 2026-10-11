from board.parts import ESP32_ANTENNA, PARTS, part

# Espressif ESP32-S3-MINI-1 & MINI-1U datasheet v1.7, Table 3-1 (pin definitions):
# pad number -> pin name. The ones this board relies on, plus the GND list.
ESPRESSIF_TABLE = {"3": "3V3", "4": "IO0", "23": "IO19", "24": "IO20", "25": "IO21", "21": "IO17",
                   "34": "IO38", "39": "TXD0", "40": "RXD0", "41": "IO45", "44": "IO46", "45": "EN",
                   "7": "IO3"}
ESPRESSIF_GND = {1, 2, 42, 43} | set(range(46, 61))


def test_every_assembled_part_has_footprint_and_lcsc():
    for key, p in PARTS.items():
        assert p.footprint.pads, key
        if key in ("DEBUG", "MH"):
            assert p.lcsc is None, key            # board-only: never in the BOM or CPL
        else:
            assert p.lcsc.startswith("C"), key


def test_module_pin_table_matches_espressif():
    pins = part("ESP32").pins
    by_pad = {pad: name for name, pads in pins.items() for pad in pads}
    for pad, name in ESPRESSIF_TABLE.items():
        assert by_pad[pad] == name, pad
    for n in ESPRESSIF_GND:
        assert by_pad[str(n)] == "GND", n
    # pads 61-65 (thermal pad + 4 corners) are all numbered "GND" in the EasyEDA footprint
    assert sum(p.number == "GND" for p in part("ESP32").footprint.pads) == 13


def test_usb_pads_are_in_the_bottom_row_centre():
    pads = {p.number: p for p in part("ESP32").footprint.pads}
    assert pads["23"].x == 0.0 and abs(pads["24"].x - 0.85) < 1e-6
    assert pads["23"].y > 7.0 and pads["24"].y > 7.0          # bottom row, away from the antenna


def test_module_body_includes_the_antenna():
    p = part("ESP32")
    assert p.body == (-7.7, -12.8, 7.7, 7.7)                  # 15.4 x 20.5 mm
    top_pads = min(q.y - q.h / 2 for q in p.footprint.pads)
    assert ESP32_ANTENNA[3] < top_pads                       # the antenna strip holds no pads
    assert abs(ESP32_ANTENNA[3] - ESP32_ANTENNA[1] - 5.05) < 1e-6


def test_atecc_and_led_pins():
    a = part("ATECC").pins
    assert a["VCC"] == ["8"] and a["GND"] == ["4"] and a["SDA"] == ["5"] and a["SCL"] == ["6"]
    assert sorted(a["NC"]) == ["1", "2", "3", "7"]
    led = part("LED").pins
    assert led == {"DO": ["1"], "GND": ["2"], "DI": ["3"], "VDD": ["4"]}


def test_ldo_and_esd_pins():
    assert part("LDO").pins == {"VIN": ["1"], "GND": ["2"], "EN": ["3"], "NC": ["4"], "VOUT": ["5"]}
    assert sorted(part("ESD").pins) == ["1", "2", "3", "4", "5", "6"]


def test_local_footprints():
    assert sorted(p.number for p in part("DEBUG").footprint.pads) == ["1", "2", "3", "4", "5"]
    assert part("R330").lcsc == "C25104" and part("R330").value == "330R"


def test_tht_flags():
    assert part("MH").tht and not part("ESP32").tht and not part("JST5").tht
