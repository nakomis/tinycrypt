FeatureScript 3095;
import(path : "onshape/std/common.fs", version : "3095.0");

// tinycrypt key case (CRYPT-16): cuts the key board's pockets, cable exit, button and LED holes
// and four M2 screws into a solid shell, splits it into front and back halves, and adds a button
// plunger and an LED light pipe as separate parts.
//
// Model axes: the shell's length is Z (the cable leaves through the +Z end), its thickness Y
// (the front, with the button and LED, is +Y), its width X. The board lies flat with its top face
// on the split plane (Y = split offset), the board's centre line on X = 0.
// Board positions are from hardware/key-board (KiCad coordinates, mm, Y down, origin at the
// board's top-left; the antenna overhangs 5.05 mm past Y = 0; the board ends at Y = 34.95).

const BOARD_W = 21;
const BOARD_ANT = -5.05;      // antenna end of the module
const BOARD_END = 34.95;      // J1 (cable) end
const BOARD_T = 1.6;
const PARTS_H = 3.3;          // J1 is 2.9 tall; plus clearance
const CL = 0.25;              // fit clearance
const LIP = 0.6;              // front pocket is this much narrower than the board, to clamp it
const SW1 = [3.6, 19.55];
const D1 = [7.65, 18.15];
const J1_X = 10.5;
const HOLES = [[16.2, 18.8], [3.0, 32.15]];   // M2 holes (2.2 mm)

annotation { "Feature Type Name" : "Tinycrypt case" }
export const tinycryptCase = defineFeature(function(context is Context, id is Id, definition is map)
    precondition
    {
        annotation { "Name" : "Shell", "Filter" : EntityType.BODY && BodyType.SOLID, "MaxNumberOfPicks" : 1 }
        definition.shell is Query;

        annotation { "Name" : "Board centre along the length (Z)" }
        isLength(definition.boardZ, { (millimeter) : [-1000, -16, 1000] } as LengthBoundSpec);

        annotation { "Name" : "Split plane (board top face) Y" }
        isLength(definition.splitY, { (millimeter) : [-100, 0, 100] } as LengthBoundSpec);

        annotation { "Name" : "Screw pitch along the length" }
        isLength(definition.screwPitch, { (millimeter) : [5, 20, 200] } as LengthBoundSpec);

        annotation { "Name" : "Screw offset beside the board" }
        isLength(definition.screwGap, { (millimeter) : [2.5, 3.2, 50] } as LengthBoundSpec);

        annotation { "Name" : "Button clearance (per side)" }
        isLength(definition.buttonGap, { (millimeter) : [0, 0.1, 1] } as LengthBoundSpec);

        annotation { "Name" : "Light pipe clearance (per side)" }
        isLength(definition.pipeGap, { (millimeter) : [0, 0.05, 1] } as LengthBoundSpec);
    }
    {
        const mm = millimeter;
        const s = definition.splitY / mm;
        const zc = definition.boardZ / mm;
        const shell = definition.shell;
        if (size(evaluateQuery(context, shell)) != 1)
            throw regenError("Select the shell body.", ["shell"]);
        const bb = evBox3d(context, { "topology" : shell });
        const ymin = bb.minCorner[1] / mm - 5;
        const ymax = bb.maxCorner[1] / mm + 5;
        const zmax = bb.maxCorner[2] / mm + 5;

        // board coordinates to model coordinates (mm)
        const bx = function(x) { return x - BOARD_W / 2; };
        const bz = function(y) { return zc + (y - 15); };
        const v = function(x, y, z) { return vector(x, y, z) * mm; };

        // cut tools: ["box", x0, x1, y0, y1, z0, z1] or ["cyl", x0, y0, z0, x1, y1, z1, r]
        var tools = [];
        // board recess (back half) and parts pocket (front half)
        const z0 = bz(BOARD_ANT) - CL;
        const z1 = bz(BOARD_END) + CL;
        tools = append(tools, ["box", -BOARD_W / 2 - CL, BOARD_W / 2 + CL, s - BOARD_T - CL, s + 0.01, z0, z1]);
        tools = append(tools, ["box", -BOARD_W / 2 + LIP, BOARD_W / 2 - LIP, s - 0.01, s + PARTS_H, z0, z1]);
        // room for the JST plug and the cable's bend, then the cable channel out of the +Z end
        tools = append(tools, ["box", bx(J1_X) - 4.5, bx(J1_X) + 4.5, s, s + 3.6, bz(30), bz(BOARD_END) + 4]);
        tools = append(tools, ["cyl", bx(J1_X), s + 1.2, bz(BOARD_END) - 1, bx(J1_X), s + 1.2, zmax, 1.7]);
        // button: plunger bore and a well for its flange; LED: light pipe bore
        const sw = [bx(SW1[0]), bz(SW1[1])];
        const led = [bx(D1[0]), bz(D1[1])];
        const bg = definition.buttonGap / mm;
        const pg = definition.pipeGap / mm;
        tools = append(tools, ["cyl", sw[0], s, sw[1], sw[0], ymax, sw[1], 1.6 + bg]);
        tools = append(tools, ["cyl", sw[0], s, sw[1], sw[0], s + PARTS_H, sw[1], 2.7 + bg]);
        tools = append(tools, ["cyl", led[0], s, led[1], led[0], ymax, led[1], 1.2 + pg]);
        // four M2 x 10 screws: heads sunk in the back, 1.6 mm pilots in the front
        const sx = BOARD_W / 2 + CL + definition.screwGap / mm;
        const p = definition.screwPitch / mm / 2;
        for (var x in [-sx, sx])
        {
            for (var z in [zc - p, zc + p])
            {
                tools = append(tools, ["cyl", x, ymin, z, x, s + 0.01, z, 1.15]);
                tools = append(tools, ["cyl", x, ymin, z, x, s - 4, z, 2.1]);
                tools = append(tools, ["cyl", x, s - 0.01, z, x, s + 6.5, z, 0.8]);
            }
        }
        // two copies of the uncut shell, to trim the plunger and the light pipe flush with its surface
        opPattern(context, id + "trimButton", { "entities" : shell, "transforms" : [identityTransform()], "instanceNames" : ["1"] });
        opPattern(context, id + "trimPipe", { "entities" : shell, "transforms" : [identityTransform()], "instanceNames" : ["1"] });

        var cuts = [];
        for (var i = 0; i < size(tools); i += 1)
        {
            const t = tools[i];
            const cid = id + ("cut" ~ i);
            if (t[0] == "box")
                fCuboid(context, cid, { "corner1" : v(t[1], t[3], t[5]), "corner2" : v(t[2], t[4], t[6]) });
            else
                fCylinder(context, cid, { "bottomCenter" : v(t[1], t[2], t[3]), "topCenter" : v(t[4], t[5], t[6]), "radius" : t[7] * mm });
            cuts = append(cuts, qCreatedBy(cid, EntityType.BODY));
        }
        opBoolean(context, id + "cut", { "tools" : qUnion(cuts), "targets" : shell,
                    "operationType" : BooleanOperationType.SUBTRACTION });

        // locating pins through the board's mounting holes
        var pins = [shell];
        for (var i = 0; i < size(HOLES); i += 1)
        {
            fCylinder(context, id + ("pin" ~ i), { "bottomCenter" : v(bx(HOLES[i][0]), s - BOARD_T - CL, bz(HOLES[i][1])),
                        "topCenter" : v(bx(HOLES[i][0]), s - 0.1, bz(HOLES[i][1])), "radius" : 1.0 * mm });
            pins = append(pins, qCreatedBy(id + ("pin" ~ i), EntityType.BODY));
        }
        opBoolean(context, id + "pins", { "tools" : qUnion(pins), "operationType" : BooleanOperationType.UNION });

        // split into halves at the board's top face
        opPlane(context, id + "plane", { "plane" : plane(v(0, s, 0), vector(0, 1, 0)), "width" : 1 * meter, "height" : 1 * meter });
        opSplitPart(context, id + "split", { "targets" : shell, "tool" : qCreatedBy(id + "plane", EntityType.FACE) });
        opDeleteBodies(context, id + "deletePlane", { "entities" : qCreatedBy(id + "plane", EntityType.BODY) });
        for (var part in evaluateQuery(context, qUnion([shell, qCreatedBy(id + "split", EntityType.BODY)])))
        {
            const c = evBox3d(context, { "topology" : part });
            const front = (c.minCorner[1] + c.maxCorner[1]) / 2 > s * mm;
            setProperty(context, { "entities" : part, "propertyType" : PropertyType.NAME, "value" : front ? "Case front" : "Case back" });
        }

        // button plunger: 3.2 mm shaft, 5.4 mm flange resting on the switch; LED light pipe 2.4 mm
        // (clear resin). Both run past the surface and are trimmed flush with an uncut copy of the
        // shell. A boolean's result isn't credited to its tools' ids, so parts are found by a point
        // inside them (the case has holes there).
        const copies = qUnion([qCreatedBy(id + "trimButton", EntityType.BODY), qCreatedBy(id + "trimPipe", EntityType.BODY)]);
        const partAt = function(point) { return qSubtraction(qContainsPoint(qAllModifiableSolidBodies(), point), copies); };
        const inPlunger = v(sw[0], s + 2, sw[1]);
        const inPipe = v(led[0], s + 2, led[1]);
        fCylinder(context, id + "shaft", { "bottomCenter" : v(sw[0], s + 1.55, sw[1]), "topCenter" : v(sw[0], ymax, sw[1]), "radius" : 1.6 * mm });
        fCylinder(context, id + "flange", { "bottomCenter" : v(sw[0], s + 1.55, sw[1]), "topCenter" : v(sw[0], s + 2.35, sw[1]), "radius" : 2.7 * mm });
        opBoolean(context, id + "plunger", { "tools" : qUnion([qCreatedBy(id + "shaft", EntityType.BODY), qCreatedBy(id + "flange", EntityType.BODY)]),
                    "operationType" : BooleanOperationType.UNION });
        opBoolean(context, id + "trimPlunger", { "tools" : qUnion([partAt(inPlunger), qCreatedBy(id + "trimButton", EntityType.BODY)]),
                    "operationType" : BooleanOperationType.INTERSECTION });
        fCylinder(context, id + "pipe", { "bottomCenter" : v(led[0], s + 1.12, led[1]), "topCenter" : v(led[0], ymax, led[1]), "radius" : 1.2 * mm });
        opBoolean(context, id + "trimLight", { "tools" : qUnion([qCreatedBy(id + "pipe", EntityType.BODY), qCreatedBy(id + "trimPipe", EntityType.BODY)]),
                    "operationType" : BooleanOperationType.INTERSECTION });
        // both copies are used up now, so only the part itself contains its point
        const named = function(point) { return qContainsPoint(qAllModifiableSolidBodies(), point); };
        setProperty(context, { "entities" : named(inPlunger), "propertyType" : PropertyType.NAME, "value" : "Button plunger" });
        setProperty(context, { "entities" : named(inPipe), "propertyType" : PropertyType.NAME, "value" : "LED light pipe" });
    }, { "shell" : qNothing(), "boardZ" : -16 * millimeter, "splitY" : 0 * millimeter, "screwPitch" : 20 * millimeter, "screwGap" : 3.2 * millimeter, "buttonGap" : 0.1 * millimeter, "pipeGap" : 0.05 * millimeter });
