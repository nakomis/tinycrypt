# tinycrypt key case (CRYPT-16)

**Current design: SolidCraft**, `tinycrypt-case.json` (31 Oct 2026; open it in SolidCraft). It rebuilds the
Onshape case below natively and is what was printed in Elegoo ABS-like 3.0 Smoky Black:

- **Shell:** a loft through 46 ellipses, every 2 mm along the length, measured from Onshape's loft (its
  sections are true ellipses to within 0.1 mm, and the volume matches to 0.03 %). SolidCraft can't yet put
  guide rails on a loft that ends in a point, or loft an ellipse to a point, so the profile splines
  (sketch `Profile`) are kept for reference only and don't drive the shape. The +Z tip ends at Z 36, a
  flat inside the cable channel.
- **Parameters** (the design's parameter table): `board_w` 21, `board_cl` 0.25 (both openings are
  `board_w + 2*board_cl` = 21.5 mm), `lip` (−`board_cl`: the front pocket is as wide as the back recess;
  0.6 would clamp the board's edges instead), `board_z` −16, `split_y` 0, `screw_pitch` 12, `screw_gap`
  2.5, `button_gap` 0.1, `led_skin` 1.
- **LED:** no light pipe. A 3.5 mm blind window from inside leaves `led_skin` of resin over D1, so the
  outside stays smooth and the LED shows through translucent resin. (For an opaque resin, cut it through
  and print a pipe.)
- **Split** at `split_y + 0.05 mm`: exactly at the board face, the plane sits on the loft's seam and on
  faces 0.01 mm either side of it, and the kernel can't split there.
- **Export each body on its own** (`export` with `bodies`): an all-bodies STL leaves the split faces as
  loose patches.

The Onshape version is below, kept for reference.

The case is modelled in Onshape, in the public document
[tinycrypt case](https://cad.onshape.com/documents/8f763e3ec3745cee324f3fdb). Its Part Studio holds
Martin's shape (sketches and lofts, copied from his `tinycrypt` document) followed by one custom
feature, **Tinycrypt case**, from the Feature Studio of the same name. `tinycrypt-case.fs` is a copy
of that Feature Studio; Onshape's is the one that runs, so edit there and copy it back here.

The feature takes the shell body and makes four parts:

- **Case back**: a recess for the board with two locating pins through its M2 holes, and four
  counterbores for M2 × 10 screw heads.
- **Case front**: a pocket for the parts (a 0.6 mm lip clamps the board's edges), room for the JST
  plug and the cable's bend, a 3.4 mm cable channel out of the +Z end, the button and LED bores, and
  1.6 mm pilot holes for the screws.
- **Button plunger**: a 3.2 mm shaft with a 5.4 mm flange resting on SW1, trimmed flush with the
  outer surface.
- **LED light pipe**: 2.4 mm, over D1, trimmed flush (print in clear resin, or use a 2.5 mm acrylic rod).

Axes: the length is Z (the cable leaves at +Z), the thickness Y (+Y is the front, with the button and
LED), the width X. The board's top face lies on the split plane. Positions come from
`hardware/key-board` (SW1, D1, J1 and the mounting holes).

## Settings (the feature's dialog)

| Setting | Default | |
|---|---|---|
| Board centre along the length (Z) | −16 mm | sits the board in the widest part of the shape |
| Split plane (board top face) Y | 0 | moving it towards the front shortens the plunger and pipe (about 15 mm now) |
| Screw pitch along the length | 20 mm | |
| Screw offset beside the board | 3.2 mm | |
| Button clearance (per side) | 0.1 mm | sized for the resin printer (Elegoo Mars 5 Ultra); raise to 0.15 if it binds |
| Light pipe clearance (per side) | 0.05 mm | a snug fit |

Fixed in the code: board clearance 0.25 mm, locating pins 2.0 mm in the 2.2 mm holes.

## Checks

- The feature stops with an error if the button or LED falls outside the shape. It doesn't check the
  screws: if the shape gets narrower beside the board, look for counterbores breaking through.
- The flange well and the light pipe bore are only about 0.25 mm apart (SW1 and D1 are 4.3 mm apart
  on the board).
- Fuzzy skin is applied in the slicer, not here.
- Not yet test-printed or fitted to a real board.

## Working on it with Claude

The Onshape **FeatureScript MCP** (`https://fs-mcp.labs.onshape.app/mcp`) edits and tests the Feature
Studio; `nakomis/onshape-mcp` covers the REST side (copying elements, exporting STL). Both use
Martin's Onshape API allowance: **2,500 calls a year on the Free plan**, so test sparingly.
After changing the Feature Studio, the Part Studio's feature has to be pointed at the new
microversion (re-select it in the UI, or update its namespace through the API).
