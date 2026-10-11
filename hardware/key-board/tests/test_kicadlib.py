from pathlib import Path
from conftest import KICAD_FP
from board.kicadlib import load_footprint, load_symbol_pins, load_symbol_pin_types

R0402 = KICAD_FP / "Resistor_SMD.pretty" / "R_0402_1005Metric.kicad_mod"
SYM = '''(kicad_symbol_lib (symbol "U" (symbol "U_1_1"
  (pin power_in line (at 0 0 0) (length 2.54) (name "VDD") (number "1"))
  (pin power_in line (at 0 0 0) (length 2.54) (name "VDD") (number "4"))
  (pin bidirectional line (at 0 0 0) (length 2.54) (name "IO0") (number "2")))))'''


def test_load_kicad_stdlib_footprint():
    fp = load_footprint(R0402)
    assert fp.node[0] == "footprint"
    assert sorted(p.number for p in fp.pads) == ["1", "2"]
    p1 = next(p for p in fp.pads if p.number == "1")
    assert p1.kind == "smd" and p1.x < 0 and p1.drill is None
    xmin, ymin, xmax, ymax = fp.courtyard
    assert xmin < p1.x < xmax


def test_symbol_pins_group_shared_names(tmp_path):
    f = tmp_path / "t.kicad_sym"
    f.write_text(SYM)
    assert load_symbol_pins(f, "U") == {"VDD": ["1", "4"], "IO0": ["2"]}
    assert load_symbol_pin_types(f, "U")["2"] == "bidirectional"


def test_legacy_module_head_is_normalised(tmp_path):
    f = tmp_path / "m.kicad_mod"
    f.write_text('(module X (layer F.Cu) (pad 1 thru_hole circle (at 0 0) (size 1 1) (drill 0.7) (layers *.Cu *.Mask)))')
    fp = load_footprint(f)
    assert fp.node[0] == "footprint" and fp.pads[0].drill == 0.7


def test_footprint_without_pads_or_courtyard_loads(tmp_path):
    f = tmp_path / "logo.kicad_mod"
    f.write_text('(footprint "Logo" (layer "F.Cu") (fp_text reference "G***" (at 0 0) (layer "F.SilkS")))')
    fp = load_footprint(f)
    assert fp.pads == [] and fp.courtyard == (0.0, 0.0, 0.0, 0.0)


def test_circle_courtyard_uses_the_radius(tmp_path):
    f = tmp_path / "h.kicad_mod"
    f.write_text('(footprint "H" (layer "F.Cu") (fp_circle (center 0 0) (end 2.45 0) (layer "F.CrtYd"))'
                 ' (pad "1" thru_hole circle (at 0 0) (size 4.4 4.4) (drill 2.2) (layers "*.Cu")))')
    assert load_footprint(f).courtyard == (-2.45, -2.45, 2.45, 2.45)
