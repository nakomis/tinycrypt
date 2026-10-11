from board.cleanup import prune
from board.kicad_pcb import Track, Via
from board.netlist import Circuit
from board.placement import Placement


def _two_resistors():
    c = Circuit()
    a, b = c.add("R10K", "R1"), c.add("R10K", "R2")
    c.connect("A", (a, "1"), (b, "1"))
    c.connect("B", (a, "2"), (b, "2"))
    p = Placement((0, 0, 20, 20), {"R1": (5.0, 5.0, 0), "R2": (5.0, 10.0, 0)})
    return c, p


def test_one_layer_via_and_its_stub_are_removed():
    c, p = _two_resistors()
    good = [Track("A", "F.Cu", 4.57, 5.0, 4.57, 10.0, 0.15)]           # pad 1 to pad 1
    stub = Track("A", "F.Cu", 4.57, 7.0, 3.0, 7.0, 0.15)                 # T off the run to a via
    tracks, vias = prune(c, p, good + [stub], [Via("A", 3.0, 7.0)])
    assert tracks == good and vias == []


def test_two_layer_via_and_keep_and_gnd_survive():
    c, p = _two_resistors()
    t = [Track("A", "F.Cu", 4.57, 5.0, 4.0, 7.0, 0.15), Track("A", "B.Cu", 4.0, 7.0, 4.0, 9.0, 0.15),
         Track("A", "F.Cu", 4.0, 9.0, 4.57, 10.0, 0.15)]
    v = [Via("A", 4.0, 7.0), Via("A", 4.0, 9.0), Via("B", 12.0, 12.0), Via("GND", 15.0, 15.0)]
    tracks, vias = prune(c, p, t, v, keep=[v[2]])
    assert tracks == t and vias == v
