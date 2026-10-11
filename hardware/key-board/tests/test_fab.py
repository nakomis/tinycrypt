import csv
import subprocess
import zipfile

from board import key_board as kb
from board.fab import KICAD_CLI, fab, write_bom, write_cpl
from board.kicad_pcb import write_board
from board.netlist import Circuit
from board.parts import PARTS, ROTATION_CHECKED
from board.placement import Placement

C = kb.build_key()
P = kb.place_key(C)
BOARD_ONLY = {"TP1", "H1", "H2"}


def _synthetic():
    """A tiny circuit with a DNP instance."""
    c = Circuit()
    a, b = c.add("C100N"), c.add("C100N")
    d = c.add("C100N", dnp=True)
    e = c.add("R10K")
    p = Placement((10.0, 20.0, 40.0, 60.0),
                  {a.ref: (15.0, 50.0, 0.0), b.ref: (20.0, 45.0, 90.0),
                   d.ref: (25.0, 40.0, 0.0), e.ref: (30.0, 30.0, 270.0)})
    return c, p, (a.ref, b.ref, d.ref, e.ref)


def test_bom_groups_and_excludes_dnp(tmp_path):
    c, _, (a, b, d, e) = _synthetic()
    write_bom(c, tmp_path / "bom.csv")
    rows = list(csv.DictReader(open(tmp_path / "bom.csv")))
    assert set(rows[0]) == {"Comment", "Designator", "Footprint", "LCSC Part #"}
    refs = [r for row in rows for r in row["Designator"].split(",")]
    assert sorted(refs) == sorted([a, b, e]) and d not in refs
    assert len(rows) == 2


def test_key_bom_has_every_assembled_part_and_no_board_only_ones(tmp_path):
    write_bom(C, tmp_path / "bom.csv")
    rows = list(csv.DictReader(open(tmp_path / "bom.csv")))
    refs = [r for row in rows for r in row["Designator"].split(",")]
    assert sorted(refs) == sorted(set(C.instances) - BOARD_ONLY)
    assert all(row["LCSC Part #"].startswith("C") for row in rows)
    lcsc = {row["LCSC Part #"] for row in rows}
    assert {"C2913206", "C1518769", "C265419", "C7519", "C51118", "C318884", "C52917434", "C25104"} <= lcsc


def test_cpl_rotation_and_coordinates(tmp_path):
    c, p, (a, b, d, e) = _synthetic()
    write_cpl(c, p, tmp_path / "cpl.csv")
    rows = {r["Designator"]: r for r in csv.DictReader(open(tmp_path / "cpl.csv"))}
    assert d not in rows
    for ref in (a, b, e):
        want = (p.at[ref][2] + c.instances[ref].part.rotation_offset) % 360
        assert abs(float(rows[ref]["Rotation"]) - want) < 0.01
        assert rows[ref]["Layer"] == "Top"
    # Gerber frame: x as in KiCad, y negated, whatever the outline origin
    assert abs(float(rows[a]["Mid X"]) - 15.0) < 1e-6
    assert abs(float(rows[a]["Mid Y"]) - -50.0) < 1e-6


def test_cpl_matches_kicad_position_file(tmp_path):
    """JLCPCB's CPL frame: Mid Y is the negated KiCad y. KiCad's own position export agrees."""
    pcb = tmp_path / "x.kicad_pcb"
    write_board(C, P, pcb, layers=2)
    pos = tmp_path / "pos.csv"
    subprocess.run([KICAD_CLI, "pcb", "export", "pos", "--format", "csv", "--units", "mm",
                    "--side", "front", "-o", str(pos), str(pcb)], check=True, capture_output=True)
    want = {r["Ref"]: r for r in csv.DictReader(open(pos))}
    write_cpl(C, P, tmp_path / "cpl.csv")
    got = {r["Designator"]: r for r in csv.DictReader(open(tmp_path / "cpl.csv"))}
    assert set(got) == set(want) == set(C.instances) - BOARD_ONLY
    for ref, r in got.items():
        assert abs(float(r["Mid X"]) - float(want[ref]["PosX"])) < 0.01, ref
        assert abs(float(r["Mid Y"]) - float(want[ref]["PosY"])) < 0.01, ref
        assert abs((float(r["Rotation"]) - float(want[ref]["Rot"])) % 360) < 0.01, ref


def test_u1_cpl_is_the_module_footprint_centre(tmp_path):
    """JLCPCB places U1 by its footprint origin, the centre of the pad pattern, which the
    antenna overhang does not move: x on the centre line, Mid Y the negated KiCad y."""
    write_cpl(C, P, tmp_path / "cpl.csv")
    u1 = next(r for r in csv.DictReader(open(tmp_path / "cpl.csv")) if r["Designator"] == "U1")
    assert abs(float(u1["Mid X"]) - kb.W / 2) < 1e-6 and abs(float(u1["Mid Y"]) + kb.MOD_Y) < 1e-6


def test_every_extended_part_has_a_checked_offset():
    for key, p in PARTS.items():
        if p.extended:
            assert key in ROTATION_CHECKED, key


def test_footprint_origins_are_pad_pattern_centres():
    for key, p in PARTS.items():
        pads = [q for q in p.footprint.pads if q.number]
        xs, ys = [q.x for q in pads], [q.y for q in pads]
        assert abs((min(xs) + max(xs)) / 2) < 0.05 and abs((min(ys) + max(ys)) / 2) < 0.05, key


def test_fab_outputs(tmp_path):
    pcb = tmp_path / "tinycrypt-key.kicad_pcb"
    write_board(C, P, pcb, layers=2, silk=kb.silk_key(C, P), art=kb.art_key(P), keepouts=kb.keepouts())
    fab(pcb, tmp_path, layers=2)
    names = zipfile.ZipFile(tmp_path / "tinycrypt-key-gerbers.zip").namelist()
    assert any(n.endswith("-F_Cu.gtl") for n in names) and any(n.endswith("-B_Cu.gbl") for n in names)
    assert not any("In1" in n for n in names)
    assert any(n.endswith("-F_Paste.gtp") for n in names)
    assert any(n.endswith("-B_Silkscreen.gbo") for n in names)
    assert any(n.endswith("-PTH.drl") for n in names)
    assert (tmp_path / "tinycrypt-key-top.png").stat().st_size > 1000
    assert (tmp_path / "tinycrypt-key-bottom.png").stat().st_size > 1000


def test_stackup_colours(tmp_path):
    from board.sexpr import find, find_all, parse
    pcb = tmp_path / "x.kicad_pcb"
    write_board(C, P, pcb, layers=2, mask="Black", silkscreen="White")
    st = find(find(parse(pcb.read_text()), "setup"), "stackup")
    col = {l[1]: find(l, "color")[1] for l in find_all(st, "layer") if find(l, "color")}
    assert col == {"F.SilkS": "White", "B.SilkS": "White", "F.Mask": "Black", "B.Mask": "Black"}
