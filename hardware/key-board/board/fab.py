"""JLCPCB fabrication outputs: BOM, CPL (pick and place), gerbers, drill files and renders.

Only circuit instances reach the BOM and CPL, so board-only footprints (the silk portrait)
are excluded by construction; DNP instances and parts without an LCSC number (test pads,
mounting holes) are excluded explicitly, in Python."""
import csv
import shutil
import subprocess
import zipfile
from pathlib import Path

KICAD_CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"


def _fitted(circuit):
    """Instances JLCPCB assembles: not DNP, and a real LCSC part (test pads and mounting holes
    are board-only footprints with no LCSC number)."""
    return {r: i for r, i in circuit.instances.items() if not i.dnp and i.part.lcsc}


def _ref_key(ref):
    head = ref.rstrip("0123456789")
    return head, int(ref[len(head):] or 0)


def write_bom(circuit, path):
    groups = {}
    for ref, inst in _fitted(circuit).items():
        groups.setdefault((inst.value, inst.part.lcsc, inst.part.footprint.name.split(":")[-1]), []).append(ref)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for (value, lcsc, fp), refs in sorted(groups.items(), key=lambda g: _ref_key(sorted(g[1], key=_ref_key)[0])):
            w.writerow([value, ",".join(sorted(refs, key=_ref_key)), fp, lcsc])


def write_cpl(circuit, placement, path):
    """Mid X / Mid Y are the footprint origin (the pad-pattern centre for every part) in mm in
    the GERBER frame, which JLCPCB uses for placement: the same origin as the KiCad board, x
    unchanged and y negated (KiCad's y is down, Gerber's is up). This matches KiCad's own
    position file; it is not relative to the outline."""
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for ref, inst in sorted(_fitted(circuit).items(), key=lambda kv: _ref_key(kv[0])):
            x, y, rot = placement.at[ref]
            w.writerow([ref, f"{x:.4f}", f"{-y:.4f}", "Top",
                        f"{(rot + inst.part.rotation_offset) % 360:g}"])


def fab(board_path, out_dir, layers=2):
    """Gerbers + drill (zipped to <stem>-gerbers.zip) and top/bottom renders in out_dir."""
    board_path, out_dir = Path(board_path), Path(out_dir)
    stem = board_path.stem
    gdir = out_dir / "gerbers"
    shutil.rmtree(gdir, ignore_errors=True)
    gdir.mkdir(parents=True)
    cu = ["F.Cu"] + (["In1.Cu", "In2.Cu"] if layers == 4 else []) + ["B.Cu"]
    lay = cu + ["F.Paste", "F.Mask", "B.Mask", "F.Silkscreen", "B.Silkscreen", "Edge.Cuts"]
    run = lambda *a: subprocess.run([KICAD_CLI, *a], check=True, capture_output=True, text=True)
    run("pcb", "export", "gerbers", "--layers", ",".join(lay), "--subtract-soldermask", "--no-x2",
        "-o", str(gdir) + "/", str(board_path))
    run("pcb", "export", "drill", "--format", "excellon", "--excellon-separate-th",
        "-o", str(gdir) + "/", str(board_path))
    zpath = out_dir / f"{stem}-gerbers.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(gdir.iterdir()):
            if f.suffix != ".gbrjob":
                z.write(f, f.name)
    for side in ("top", "bottom"):
        run("pcb", "render", "--side", side, "-o", str(out_dir / f"{stem}-{side}.png"), str(board_path))
    return zpath


def export_step(board_path, out_dir):
    """STEP model of the assembled board (1.6 mm body with its holes, every part's body from
    footprints/lcsc.3dshapes) and an isometric 3D render of it. Returns the STEP path."""
    board_path, out_dir = Path(board_path), Path(out_dir)
    step = out_dir / f"{board_path.stem}.step"
    r = subprocess.run([KICAD_CLI, "pcb", "export", "step", "--force", "--no-dnp", "-o", str(step), str(board_path)],
                       capture_output=True, text=True)
    log = r.stdout + r.stderr
    if r.returncode != 0 or not step.exists():
        raise RuntimeError(f"STEP export failed:\n{log}")
    missing = [l.strip() for l in log.splitlines() if "not found" in l.lower() or "could not" in l.lower()]
    if missing:
        raise RuntimeError("STEP export is missing models:\n" + "\n".join(missing))
    subprocess.run([KICAD_CLI, "pcb", "render", "--rotate", "-55,0,35", "--zoom", "0.9", "--quality", "high",
                    "--width", "1600", "--height", "1200", "--background", "opaque",
                    "-o", str(out_dir / f"{board_path.stem}-3d.png"), str(board_path)],
                   check=True, capture_output=True)
    return step
