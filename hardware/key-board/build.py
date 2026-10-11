#!/usr/bin/env python3
"""Board generator: circuit -> placement -> (route) -> board + DRC -> fab outputs.

    ./.venv/bin/python build.py --board key [--route] [--passes N]

Output goes to out/<board>/. Fab outputs (gerbers zip, BOM, CPL, renders, pins.h) are written
only when DRC has no errors and no unconnected items."""
import argparse
import json
import subprocess
from pathlib import Path

from board import key_board as kb
from board.fab import fab, write_bom, write_cpl
from board.kicad_pcb import write_board
from board.cleanup import prune
from board.netlist import check
from board.route import route

HERE = Path(__file__).resolve().parent
KICAD_CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"

# Ordered with JLCPCB "PCB Color: Black" (which gives white silkscreen). Gerbers carry no
# colour; this only makes the KiCad renders match the order.
MASK, SILK = "Black", "White"
LAYERS = 2
BOARDS = {"key": "tinycrypt-key"}


def drc(board, out):
    """Run DRC (refilling zones and saving them into the board); return (errors, warnings)."""
    subprocess.run([KICAD_CLI, "pcb", "drc", "--refill-zones", "--save-board", "--format", "json",
                    "--severity-all", "-o", str(out / "drc.json"), str(board)], check=True,
                   capture_output=True)
    d = json.loads((out / "drc.json").read_text())
    bad = [f"unconnected: {' / '.join(i['description'] for i in u['items'])}" for u in d["unconnected_items"]]
    bad += [f"{v['type']}: {v['description']} "
            f"[{'; '.join(i['description'] for i in v.get('items', []))}]"
            for v in d["violations"] if v["severity"] == "error"]
    warn = [v for v in d["violations"] if v["severity"] != "error"]
    return bad, warn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", choices=sorted(BOARDS), required=True)
    ap.add_argument("--route", action="store_true")
    ap.add_argument("--passes", type=int, default=100)
    a = ap.parse_args()
    stem = BOARDS[a.board]
    out = HERE / "out" / a.board
    tmp = out / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    c = kb.build_key()
    problems = check(c)
    if problems:
        raise SystemExit("\n".join(problems))
    p = kb.place_key(c)
    hand_tracks, hand_vias = kb.usb_hand_route()
    fixed = kb.thermal_vias() + hand_vias
    tracks, vias = list(hand_tracks), list(fixed)
    if a.route:
        routed_tracks, routed = route(c, p, tmp, passes=a.passes, layers=LAYERS, route_area=kb.route_area(),
                                      fixed_vias=fixed, fixed_tracks=hand_tracks,
                                      via_keepouts=kb.back_text_boxes(),
                                      log="/tmp/claude-501/key-board-route.log")
        tracks, vias = prune(c, p, tracks + routed_tracks, fixed + routed, keep=fixed)
        stitch = kb.stitch_vias(c, p, tracks, vias, avoid=kb.back_text_boxes(), avoid_at=kb.on_art)
        print(f"stitching: {len(stitch)} GND vias")
        vias += stitch
    if kb.vias_under_text(vias):
        raise SystemExit(f"vias under the back text: {kb.vias_under_text(vias)}")
    board = out / f"{stem}.kicad_pcb"
    write_board(c, p, board, tracks, vias, layers=LAYERS, silk=kb.silk_key(c, p), art=kb.art_key(p),
                hide_refs=set(c.instances), mask=MASK, silkscreen=SILK,     # silk labels replace ref texts
                keepouts=kb.keepouts())
    bad, warn = drc(board, out)
    kinds = {}
    for v in warn:
        kinds[v["type"]] = kinds.get(v["type"], 0) + 1
    print(f"wrote {board}: {len(bad)} DRC errors/unconnected, {len(warn)} warnings {kinds}")
    if bad:      # kicad-cli exits 0 even when DRC fails
        raise SystemExit("DRC failed:\n" + "\n".join(bad))
    write_bom(c, out / "bom.csv")
    write_cpl(c, p, out / "cpl.csv")
    (out / "pins.h").write_text(kb.pins_h())
    fab(board, out, layers=LAYERS)
    print(f"fab outputs in {out}: bom.csv, cpl.csv, pins.h, {stem}-gerbers.zip, renders; "
          f"order with solder mask {MASK}, silkscreen {SILK}")


if __name__ == "__main__":
    main()
