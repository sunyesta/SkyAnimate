"""The deform modifiers (Curve, Cage and Mesh Deform): a shape on another layer bends a drawing as it moves from
where it is on the setup frame. Each takes its own shapes; Change Type switches; old Vector Deforms open as the
right one. A grid by its cells, a closed shape as a cage, an open line along it; Automatic (every point) or Custom weights;
Strength; a point's weight shares it between the deform and the bones; loops do nothing; saved.

Run with:  OpenAnimate --background --python tests/test_vector_deform.py
"""

import math
import os
import shutil
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0


def close(a, b, eps=1e-3):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


def deform(layer, target, shape, kind="MESH_DEFORM"):
    layers.active = layer
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    index = len(layer.modifiers) - 1
    layer.modifier_set(index, layer=target.name, shape=shape)
    if kind == "CAGE_DEFORM":
        layer.modifier_set(index, cage_method="MEAN_VALUE", resample_self=False)
    return index


def key_point(layer, index, pos, frame=10):
    doc.frame_current = frame
    layer.set_point(index, pos)
    doc.frame_current = frame + 1
    doc.frame_current = frame


# --- Grid ---------------------------------------------------------------------------------------------------
grid = new_layer("Grid")
assert grid.add_primitive("GRID", [(-1, -1), (1, 1)], columns=2, rows=2)
art = new_layer("Art")
art.add_curve([(-0.5, 0.0), (0.0, 0.0), (0.5, 0.0)])
art.add_curve([(3.0, 0.0), (3.5, 0.0)])
i = deform(art, grid, grid.shapes[0][0])
m = art.modifiers[i]
assert m["type"] == "MESH_DEFORM" and m["kind"] == "Grid" and m["weights"] == "AUTOMATIC", m
assert m["layer"] == "Grid" and m["strength"] == 1.0

doc.frame_current = 0
assert all(close(a, b[:2]) for a, b in zip(art.evaluated()[0], art.points)), "on the setup frame nothing bends"
# Editing the grid there (before the edit is recorded) bends nothing either.
grid.set_point(4, (0.2, 0.0))
assert all(close(a, b[:2]) for a, b in zip(art.evaluated()[0], art.points)), "editing it on frame 0 bends nothing"
grid.set_point(4, (0.0, 0.0))

# The grid's middle node (row 1, column 1) goes up on frame 10: what is on it goes with it.
key_point(grid, 4, (0.0, 0.5))
pts = art.evaluated()[0]
assert close(pts[1], (0.0, 0.5)), pts
assert 0.0 < pts[0][1] < 0.5 and close((pts[0][0], 0), (-0.5, 0)), pts
assert close(pts[3], (3.0, 0.0)), "outside the grid, a point follows its outline, which hasn't moved"
# The outline's right middle node goes up: points out to the right go up with the side nearest them (the sides
# nearer the corners, which didn't move, count too, the less the nearer the point is to the right side).
key_point(grid, 5, (1.0, 0.3))
pts = art.evaluated()[0]
assert 0.1 < pts[3][1] < 0.35 and 0.05 < pts[4][1] <= pts[3][1] + 1e-3, (
    "Automatic: points past the grid follow it too", pts[3:5])
assert pts[4][0] > pts[3][0] + 0.4, "and keep their spacing"
key_point(grid, 5, (1.0, 0.0))

art.modifier_set(i, strength=0.5)
assert close(art.evaluated()[0][1], (0.0, 0.25)), "Strength scales it"
art.modifier_set(i, strength=1.0)

# Custom weights start from the automatic ones, in a point group named after the modifier.
auto = art.deform_weights(i)
assert auto == [1.0] * 5, "Automatic weights: every point fully"
art.modifier_set(i, weights="CUSTOM")
m = art.modifiers[i]
assert m["weights"] == "CUSTOM" and m["group"] == "Mesh Deform", m
assert art.deform_weights(i) == auto
assert any(g[0] == "Mesh Deform" for g in art.point_groups), art.point_groups
art.modifier_set(i, weights="AUTOMATIC")
doc.frame_current = 0

# --- Cage ---------------------------------------------------------------------------------------------------
cage = new_layer("Cage")
assert cage.add_primitive("BOX", [(-1, -1), (1, 1)])
art2 = new_layer("Art2")
art2.add_curve([(1.0, 0.0), (0.0, 0.0)])
j = deform(art2, cage, cage.shapes[0][0], "CAGE_DEFORM")
assert art2.modifiers[j]["kind"] == "Cage"
# The top right corner (point 2) goes out: a point on the right side stays on it, half way along.
key_point(cage, 2, (1.5, 1.5))
pts = art2.evaluated()[0]
assert close(pts[0], (1.25, 0.25)), pts
assert pts[1][0] > 0.0 and pts[1][1] > 0.0 and pts[1][0] < 0.5, "inside, it follows partly"
doc.frame_current = 0

# --- Cage Deform's Method -----------------------------------------------------------------------------------
# Mean Value (the default), Rigid (as rigid as possible) or Envelope (Harmony's: each side keeps what lies near it at
# its distance from it). Green was one too, and is gone.
assert art2.modifiers[j]["cage_method"] == "MEAN_VALUE"
try:
    art2.modifier_set(j, cage_method="SOFT")
    raise AssertionError("an unknown method is refused")
except ValueError:
    pass
try:
    art2.modifier_set(j, cage_method="GREEN")
    raise AssertionError("Green is gone")
except ValueError:
    pass
box = new_layer("Box")
assert box.add_primitive("BOX", [(-1, -1), (1, 1)])
probe = new_layer("Probe")
STEP = 0.1
rows = [[(-0.85 + STEP * a, -0.85 + STEP * b) for a in range(18)] for b in range(18)]
for row in rows:
    probe.add_curve(row)
k = deform(probe, box, box.shapes[0][0], "CAGE_DEFORM")
probe.modifier_set(k, precision=0.0)


def bent_cells():
    """Each probe cell's area (bent over rest) and how much more it stretches one way than the other."""
    pts = probe.evaluated()[0]
    cells = []
    for b in range(17):
        for a in range(17):
            p, q, r = pts[b * 18 + a], pts[b * 18 + a + 1], pts[(b + 1) * 18 + a]
            jx, jy = ((q[0] - p[0]) / STEP, (q[1] - p[1]) / STEP), ((r[0] - p[0]) / STEP, (r[1] - p[1]) / STEP)
            det = jx[0] * jy[1] - jx[1] * jy[0]
            e = (jx[0] ** 2 + jx[1] ** 2 + jy[0] ** 2 + jy[1] ** 2) / 2
            s1 = math.sqrt(e + math.sqrt(max(e * e - det * det, 0.0)))
            cells.append((det, s1 / (det / s1)))
    return cells


# Moved as a whole, every method carries the inside along exactly (its handles keep where they are from their points,
# so only a shift moves the cage unchanged).
corners = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
moved = [(x + 0.3, y - 0.2) for x, y in corners]
rest_pts = [p for row in rows for p in row]
for method in ("MEAN_VALUE", "RIGID", "ENVELOPE"):
    probe.modifier_set(k, cage_method=method)
    for c, to in enumerate(moved):
        key_point(box, c, to)
    pts = probe.evaluated()[0]
    for p, q in zip(rest_pts, pts):
        assert close(q, (p[0] + 0.3, p[1] - 0.2), 2e-3), (method, p, q)
    doc.frame_current = 0
for c, to in enumerate(corners):
    key_point(box, c, to)
doc.frame_current = 0

# One corner pulled far out: Mean Value squashes the inside by that side; Rigid much less, its outline still on
# the cage; Envelope never folds, its outline on the cage too.
key_point(box, 1, (2.5, -0.5))
found = {}
for method in ("MEAN_VALUE", "RIGID", "ENVELOPE"):
    probe.modifier_set(k, cage_method=method)
    doc.frame_current = 10
    cells = bent_cells()
    found[method] = (min(c[0] for c in cells), max(c[1] for c in cells))
    # A point on the right side, half way up: on the side now (from (2.5, -0.5) to (1, 1)) or off it.
    art2.modifier_set(j, layer="Box", shape=box.shapes[0][0], cage_method=method)
    on = art2.evaluated()[0][0]
    found[method] += (abs((on[0] - 2.5) * 1.5 - (on[1] + 0.5) * -1.5) / math.hypot(1.5, 1.5),)
    doc.frame_current = 0
print("Cage methods (least area, most one-way squash, off the side):", found)
assert found["RIGID"][0] > found["MEAN_VALUE"][0] + 0.1, found
assert found["MEAN_VALUE"][2] < 0.01 and found["RIGID"][2] < 0.01 and found["ENVELOPE"][2] < 0.01, found
assert found["ENVELOPE"][0] > 0.0, ("Envelope never folds", found)

# A band drawn just past the cage keeps its width as that side bends inward (it keeps its distance from the outline,
# rather than being squeezed as mean value coordinates past the outline squeeze it).
dent = new_layer("Dent")
dent.add_curve([(-1, -1), (0, -1), (1, -1), (1, 1), (-1, 1)], closed=True, fill=True)
band = new_layer("Band")
band.add_curve([(-0.3 + 0.1 * a, -1.05) for a in range(7)])
band.add_curve([(-0.3 + 0.1 * a, -1.15) for a in range(7)])
# And one just inside, along the same side.
band.add_curve([(-0.3 + 0.1 * a, -0.97) for a in range(7)])
band.add_curve([(-0.3 + 0.1 * a, -0.87) for a in range(7)])
b = deform(band, dent, dent.shapes[0][0], "CAGE_DEFORM")
band.modifier_set(b, precision=0.0)
key_point(dent, 1, (0.0, -0.4))
inner = {}
for method in ("MEAN_VALUE", "RIGID", "ENVELOPE"):
    band.modifier_set(b, cage_method=method)
    pts = band.evaluated()[0]
    widths = [math.dist(pts[a], pts[a + 7]) for a in range(7)]
    assert all(abs(w - 0.1) < 0.012 for w in widths), (method, widths)
    inner[method] = [math.dist(pts[14 + a], pts[21 + a]) for a in range(7)]
# Inside, only Envelope keeps the band's width where the side pushes in; the others squeeze it.
print("Band inside a side pushed in (width 0.1 at rest):", {m: [round(w, 3) for w in v] for m, v in inner.items()})
assert all(abs(w - 0.1) < 0.015 for w in inner["ENVELOPE"]), inner
assert min(inner["MEAN_VALUE"]) < 0.08, inner
# Nothing folds over at any Blend, the dent's corners closing up included.
dent_probe = new_layer("DentProbe")
for row in rows:
    dent_probe.add_curve(row)
e = deform(dent_probe, dent, dent.shapes[0][0], "CAGE_DEFORM")
dent_probe.modifier_set(e, precision=0.0, cage_method="ENVELOPE")
for blend in (0.0, 0.5, 1.0):
    dent_probe.modifier_set(e, blend=blend)
    doc.frame_current = 10
    pts = dent_probe.evaluated()[0]
    for bb in range(17):
        for a in range(17):
            p, q, r = pts[bb * 18 + a], pts[bb * 18 + a + 1], pts[(bb + 1) * 18 + a]
            assert (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]) > 0.0, (blend, a, bb)
    doc.frame_current = 0
doc.frame_current = 0
art2.modifier_set(j, layer="Cage", shape=cage.shapes[0][0], cage_method="ENVELOPE", blend=0.25)

# --- Precision's points fade in -----------------------------------------------------------------------------
# A band about twice Precision thin (an eyelid along an eye white): as the cage bends further, Precision adds points,
# and each fades in, so the band never jumps thicker for a step (and back) as one comes or goes.
small = new_layer("Small")
small.add_curve([(-0.1, -0.1), (0.1, -0.1), (0.1, 0.1), (-0.1, 0.1)], closed=True, fill=True)
lid = new_layer("Lid")
lid.add_curve([(-0.08, 0.08), (0.0, 0.085), (0.08, 0.08), (0.08, 0.084), (0.0, 0.089), (-0.08, 0.084)], closed=True,
              fill=True)
f = deform(lid, small, small.shapes[0][0], "CAGE_DEFORM")
lid.modifier_set(f, precision=0.002)
areas, counts = [], []
for step in range(81):
    key_point(small, 2, (0.1 + 0.003 * step, 0.1 - 0.0015 * step))
    shown = lid.evaluated()
    areas.append(shown[2][0][4])
    counts.append(len(shown[0]))
doc.frame_current = 0
assert len(set(counts)) > 1, ("Precision adds points somewhere along the way", counts)
sudden = max(abs(areas[i + 1] - 2 * areas[i] + areas[i - 1]) / areas[i] for i in range(1, len(areas) - 1))
assert sudden < 0.003, ("no step jumps (1.6% before points faded in)", sudden)

# --- A grid drawn by hand -----------------------------------------------------------------------------------
# Two cells side by side, made as one would by hand: a line, lines extruded from it, joined with F.
hand = new_layer("Hand")
hand.add_curve([(-1.0, 0.5), (0.0, 0.5), (1.0, 0.5)])


def pick(layer, *points):
    ops.mesh.select_none()
    layer.select_points(list(points))


pick(hand, 1)
assert hand.extrude_selected() == [3]
hand.set_point(3, (0.0, -0.5))
pick(hand, 0)
assert hand.extrude_selected() == [4]
hand.set_point(4, (-1.0, -0.5))
pick(hand, 4, 3)
assert ops.mesh.make_segment() == {'FINISHED'}
pick(hand, 2)
assert hand.extrude_selected() == [5]
hand.set_point(5, (1.0, -0.5))
pick(hand, 5, 3)
assert ops.mesh.make_segment() == {'FINISHED'}
# Corners, as the Shape tool's grid has (straight sides), so the two grids below are alike.
pick(hand, 0, 1, 2, 3, 4, 5)
assert ops.mesh.peak() == {'FINISHED'}
ops.mesh.select_none()
assert hand.shape_count == 1
art4 = new_layer("Art4")
art4.add_curve([(-0.5, 0.0), (0.5, 0.2)])
h = deform(art4, hand, hand.shapes[0][0])
assert art4.modifiers[h]["kind"] == "Grid", art4.modifiers[h]
# The same grid from the Shape tool, its middle bottom node moved alike: both bend the same.
tool = new_layer("ToolGrid")
assert tool.add_primitive("GRID", [(-1.0, -0.5), (1.0, 0.5)], columns=2, rows=1)
art5 = new_layer("Art5")
art5.add_curve([(-0.5, 0.0), (0.5, 0.2)])
deform(art5, tool, tool.shapes[0][0])
key_point(hand, 3, (0.2, -0.8))
key_point(tool, 1, (0.2, -0.8))
by_hand, by_tool = art4.evaluated()[0], art5.evaluated()[0]
assert not close(by_hand[0], (-0.5, 0.0)), "it bends"
assert close(by_hand[0], by_tool[0]) and close(by_hand[1], by_tool[1]), (by_hand[:2], by_tool[:2])
doc.frame_current = 0

# --- Line ---------------------------------------------------------------------------------------------------
line = new_layer("Line")
line.add_curve([(-1.0, 0.0), (1.0, 0.0)])
art3 = new_layer("Art3")
art3.add_curve([(0.0, 0.05), (0.5, 0.05)])
k = deform(art3, line, line.shapes[0][0], "CURVE_DEFORM")
assert art3.modifiers[k]["kind"] == "Line"
# The line moves up: what lay along it comes along, keeping its offset.
key_point(line, 0, (-1.0, 0.3))
key_point(line, 1, (1.0, 0.3))
pts = art3.evaluated()[0]
assert close(pts[0], (0.0, 0.35)) and close(pts[1], (0.5, 0.35)), pts
# Its far end goes down: what lies along it tips with it, the further along the more.
key_point(line, 1, (1.0, -0.7))
pts = art3.evaluated()[0]
assert pts[1][1] < pts[0][1] < 0.35, pts
doc.frame_current = 0

# A box along a line that arches by its handles (its ends stay put): the box bends with it, end to end.
arch = new_layer("Arch")
arch.add_curve([(-1.0, 0.0), (1.0, 0.0)])
box = new_layer("Box")
assert box.add_primitive("BOX", [(-1.0, 0.1), (1.0, 0.3)])
n = deform(box, arch, arch.shapes[0][0], "CURVE_DEFORM")
assert box.deform_weights(n) == [1.0] * 4, "everything level with a line follows it fully, however far across"
doc.frame_current = 10
arch.move_handle(0, 0, "OUT", (-0.6, 0.8))
arch.move_handle(0, 1, "IN", (0.6, 0.8))
doc.frame_current = 11
doc.frame_current = 10
pts = box.evaluated()[0]
assert len(pts) > 4, "Precision (on by default) adds points where the arch bends the box's sides"
# The arch's middle is (0, 0.6), heading right: the box's sides lie 0.1 and 0.3 above it there.
assert any(close(p, (0.0, 0.7), 0.02) for p in pts) and any(close(p, (0.0, 0.9), 0.02) for p in pts), pts
box.modifier_set(n, precision=0.0)
assert len(box.evaluated()[0]) == 4, "Precision off: only the corners move"
# Each side's handles fitted to the side bent finely (with or without Resample Self), so even with only the box's
# corners its area comes out near the true bend's (Precision very fine).
fitted_area = box.evaluated()[2][0][4]
box.modifier_set(n, precision=0.00005)
true_area = box.evaluated()[2][0][4]
assert abs(fitted_area - true_area) < 0.003 * true_area, (fitted_area, true_area)
assert box.modifiers[n]["resample_self"] is False
# Resample Self: the same, whatever Precision says, by splitting each side.
assert box.modifiers[n]["split"] == "COUNT" and box.modifiers[n]["pieces"] == 4
box.modifier_set(n, resample_self=True)
pts, _, shapes = box.evaluated()
assert len(pts) == 16, ("each side in 4 pieces, whatever Precision says", len(pts))
assert abs(shapes[0][4] - true_area) < 0.003 * true_area, (shapes[0][4], true_area)
box.modifier_set(n, resample_self=False, precision=0.002)
doc.frame_current = 0

# --- A line in a Mesh Deform: a wire, everything pinned to it (a bendy bone) ----------------------------------
# The same box on the same arch, in a Mesh Deform: what lies on the line's middle goes to the arch's middle, what
# is beside it stays as far beside it.
box2 = new_layer("Box2")
box2.add_curve([(0.0, 0.0), (0.0, 0.1)])
wn = deform(box2, arch, arch.shapes[0][0], "MESH_DEFORM")
assert box2.modifiers[wn]["kind"] == "Wire", box2.modifiers[wn]
# A line folded back on itself (as drawn in a user's file) is a wire too, not a cell of no size.
folded = new_layer("Folded")
folded.add_curve([(-0.607, 0.156), (0.561, 0.161), (0.053, 0.535)])
fw = deform(box2, folded, folded.shapes[0][0], "MESH_DEFORM")
assert box2.modifiers[fw]["kind"] == "Wire", box2.modifiers[fw]
layers.active = box2
assert ops.object.modifier_remove(index=str(fw)) == {'FINISHED'}
doc.frame_current = 10
pts = box2.evaluated()[0]
assert close(pts[0], (0.0, 0.6), 0.01) and close(pts[1], (0.0, 0.7), 0.01), pts
doc.frame_current = 0
# A line of two segments whose second grows: what is pinned to the first stays where it was (nothing slides along
# the line), where Curve Deform spreads everything along the longer line.
rail = new_layer("Rail")
rail.add_curve([(-1.0, -2.0), (0.0, -2.0), (1.0, -2.0)])
pick(rail, 0, 1, 2)
assert ops.mesh.peak() == {'FINISHED'}
ops.mesh.select_none()
riders = new_layer("Riders")
riders.add_curve([(-0.5, -1.9), (0.5, -1.9)])
pinned = deform(riders, rail, rail.shapes[0][0], "MESH_DEFORM")
sliding = new_layer("Sliders")
sliding.add_curve([(-0.5, -1.9), (0.5, -1.9)])
deform(sliding, rail, rail.shapes[0][0], "CURVE_DEFORM")
key_point(rail, 2, (2.0, -2.0))
held, slid = riders.evaluated()[0], sliding.evaluated()[0]
assert close(held[0], (-0.5, -1.9)), ("pinned to the first segment: it stays", held)
assert held[1][0] > 0.9, ("pinned to the second: it goes along as that segment stretches", held)
assert slid[0][0] > -0.4, ("Curve Deform spreads it along the longer line", slid)
doc.frame_current = 0

# A drawing the same as its warp shape (a circle and a copy of it, the copy's top pulled in): bent, it matches the
# warp exactly, its points, handles and all, with or without Resample Self, and Precision adds nothing. (A point on
# a cell's curved side used to count as outside the cell, its samples' polygon, and follow the outline instead.)
twin_warp = new_layer("TwinWarp")
assert twin_warp.add_primitive("CIRCLE", [(3.0, 3.0), (4.0, 4.0)], fill=False)
twin = new_layer("Twin")
assert twin.add_primitive("CIRCLE", [(3.0, 3.0), (4.0, 4.0)], fill=False)
tw = deform(twin, twin_warp, twin_warp.shapes[0][0], "MESH_DEFORM")
assert twin.modifiers[tw]["kind"] == "Grid", twin.modifiers[tw]
top = max(range(twin_warp.point_count), key=lambda k: twin_warp.points[k][1])
key_point(twin_warp, top, (3.55, 3.6))
for resample in (False, True):
    twin.modifier_set(tw, resample_self=resample)
    got, _, got_shapes = twin.evaluated()
    want, _, want_shapes = twin_warp.evaluated()
    assert len(got) == (len(want) * 4 if resample else len(want)), ("no points added", resample, len(got))
    assert all(close(a, b, 1e-4) for a, b in zip(got, want)), (resample, got, want)
    # (Resample Self's pieces follow the bend between the points, which the wire's one curve there can't.)
    assert abs(got_shapes[0][4] - want_shapes[0][4]) < (2e-3 if resample else 1e-4) * want_shapes[0][4], (
        resample, got_shapes, want_shapes)
    if resample:
        continue  # (The handles evaluated are its pieces'.)
    # The handles too, as evaluated and as the viewport shows them (a cell's corners, where two sides run on in
    # line, included).
    want_h = twin_warp.evaluated_handles(0)
    for shown in (False, True):
        got_h = twin.evaluated_handles(0, shown=shown)
        assert all(close(g[1], h[1], 1e-5) and close(g[2], h[2], 1e-5) for g, h in zip(got_h, want_h)), (
            resample, shown, got_h, want_h)
doc.frame_current = 0

# Local: moving the left point's handle of a one-cell warp (a circle) moves what lies by its left side, and next
# to nothing by its right side, inside the cell or past it (the cell's sides fade out smoothly toward the side
# opposite; points outside follow the sides nearest them).
local_warp = new_layer("LocalWarp")
assert local_warp.add_primitive("CIRCLE", [(6.0, 6.0), (7.0, 7.0)], fill=False)
local_art = new_layer("LocalArt")
local_art.add_curve([(6.27, 6.27), (6.28, 6.28)])   # inside, by the lower left side's middle
local_art.add_curve([(6.81, 6.81), (6.815, 6.815)])  # inside, close by the upper right side
local_art.add_curve([(6.92, 6.85), (6.93, 6.86)])   # outside, past the upper right side
lw = deform(local_art, local_warp, local_warp.shapes[0][0], "MESH_DEFORM")
doc.frame_current = 10
before, outline_before = local_art.evaluated()[0], local_warp.shape_outline(0, 32)[0]
left = min(range(local_warp.point_count), key=lambda k: local_warp.points[k][0])
kind, h_in, h_out = local_warp.curve_handles(0)[left]
for side, h in (("IN", h_in), ("OUT", h_out)):
    if h[1] < local_warp.points[left][1]:  # the handle down toward the lower left side
        local_warp.move_handle(0, left, side, (h[0] + 0.15, h[1] - 0.05))
doc.frame_current = 11
doc.frame_current = 10
after, outline_after = local_art.evaluated()[0], local_warp.shape_outline(0, 32)[0]
dist = lambda a, b: ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5
warp_moved = max(dist(a, b) for a, b in zip(outline_before, outline_after))
moved = [dist(a, b) for a, b in zip(before, after)]
assert warp_moved > 0.02 and moved[0] > warp_moved * 0.2, ("by the lower left side it bends", warp_moved, moved)
assert moved[2] < warp_moved * 0.02 and moved[4] < warp_moved * 0.01, ("by the upper right next to nothing", warp_moved, moved)
doc.frame_current = 0

# Past the outline a point keeps its distance from it: a warp stretched wide doesn't push what lies just above it
# far out (it used to stretch across as much as along, so the drawing swelled as the warp moved).
wide_warp = new_layer("WideWarp")
assert wide_warp.add_primitive("CIRCLE", [(9.0, 9.0), (10.0, 10.0)], fill=False)
above = new_layer("Above")
above.add_curve([(9.5, 10.1), (9.51, 10.1)])  # 0.1 above the top
deform(above, wide_warp, wide_warp.shapes[0][0], "MESH_DEFORM")
pts = wide_warp.points
left = min(range(len(pts)), key=lambda k: pts[k][0])
right = max(range(len(pts)), key=lambda k: pts[k][0])
key_point(wide_warp, left, (pts[left][0] - 1.0, pts[left][1]))
key_point(wide_warp, right, (pts[right][0] + 1.0, pts[right][1]))
outline = wide_warp.shape_outline(0, 64)[0]
q = above.evaluated()[0][0]
gap = min(((q[0] - o[0]) ** 2 + (q[1] - o[1]) ** 2) ** 0.5 for o in outline)
assert 0.07 < gap < 0.13, ("still about 0.1 from the warp, however wide it is", gap, q)
doc.frame_current = 0

# A line with points stacked on one another (a segment of no length, as a piece copied from a fill's outline has)
# bends what it carries as the same line without them: that segment has no way to turn anything with.
for name, pts in (("Clean", [(12.0, 0.0), (12.5, 0.0), (13.0, 0.0)]),
                  ("Stacked", [(12.0, 0.0), (12.5, 0.0), (12.5, 0.0), (13.0, 0.0)])):
    line = new_layer(name + "Line")
    line.add_curve(pts)
    art_l = new_layer(name + "Art")
    art_l.add_curve([(12.45, 0.2), (12.55, 0.2)])
    art_l.add_curve([(12.5, -0.15), (12.6, -0.15)])
    deform(art_l, line, line.shapes[0][0], "MESH_DEFORM")
    key_point(line, len(pts) - 1, (13.0, 0.4))
    globals()[name.lower() + "_bent"] = art_l.evaluated()[0]
doc.frame_current = 0
assert all(close(a, b, 2e-3) for a, b in zip(clean_bent, stacked_bent)), (clean_bent, stacked_bent)

# A thin band hugging the outside of a one-cell warp stays thin when the cell is stretched across that side (it
# used to take the inside's stretch, and swelled).
band_warp = new_layer("BandWarp")
assert band_warp.add_primitive("BOX", [(15.0, 0.0), (16.0, 1.0)])
band = new_layer("Band")
band.add_curve([(16.01, 0.2), (16.03, 0.2), (16.03, 0.8), (16.01, 0.8)], closed=True, fill=True)
bw = deform(band, band_warp, band_warp.shapes[0][0], "MESH_DEFORM")
assert band.modifiers[bw]["kind"] == "Grid", band.modifiers[bw]
area_rest = band.evaluated()[2][0][4]
right = [k for k in range(band_warp.point_count) if band_warp.points[k][0] > 15.5]
for k in right:
    key_point(band_warp, k, (17.0, band_warp.points[k][1]))
area_now = band.evaluated()[2][0][4]
assert area_now < area_rest * 1.3, ("the band stays thin", area_rest, area_now)
doc.frame_current = 0

# --- A loop does nothing ------------------------------------------------------------------------------------
loop = deform(grid, art, art.shapes[0][0], "CURVE_DEFORM")
doc.frame_current = 10
assert close(art.evaluated()[0][1], (0.0, 0.0)), "a loop (each bends the other) does nothing"
layers.active = grid
assert ops.object.modifier_remove(index=str(loop)) == {'FINISHED'}
assert close(art.evaluated()[0][1], (0.0, 0.5)), "without the loop it bends again"
doc.frame_current = 0

# --- Bones and the deform share a point ---------------------------------------------------------------------
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="b", head_x="-2", head_y="-2", tail_x="-1", tail_y="-2", parent="")
ops.layer.new()
boned = layers.active
boned.name = "Boned"
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
boned = next(l for l in layers.all() if l.name == "Boned")
boned.add_curve([(0.0, 0.0), (0.5, 0.0)])
layers.active = boned
ops.mesh.select_none()
boned.select_points([0, 1])
rig.select_bone("b")
assert ops.weight.assign(weight="1.0") == {'FINISHED'}
ops.mesh.select_none()
b = deform(boned, grid, grid.shapes[0][0])
doc.frame_current = 20
rig.bone_set("b", angle=1.0)
doc.frame_current = 21
doc.frame_current = 20
pts = boned.evaluated()[0]
# Both points are fully the deform's: they go where the grid (as frame 10 keyed it) takes them, as Art's
# points there do, and the bone doesn't move them too.
grid_only = art.evaluated()[0]
assert close(pts[0], grid_only[1]) and close(pts[1], grid_only[2]), (pts, grid_only)
boned.modifier_set(b, strength=0.0)
pts = boned.evaluated()[0]
assert not close(pts[1], grid_only[2]) and not close(pts[1], (0.5, 0.0)), "with no deform the bone moves them"
doc.frame_current = 0

# --- Each deform takes its own shapes -------------------------------------------------------------------------
layers.active = art  # beside it, not inside the bone layer
wrong = new_layer("Wrong")
wrong.add_curve([(-0.5, 0.0), (0.5, 0.0)])
w = deform(wrong, grid, grid.shapes[0][0], "CURVE_DEFORM")
m = wrong.modifiers[w]
assert m["kind"] == "" and m["fits"] == "MESH_DEFORM", "a grid isn't a line: Mesh Deform takes it"
doc.frame_current = 10
assert close(wrong.evaluated()[0][0], (-0.5, 0.0)), "so it bends nothing"
wrong.modifier_set(w, type="MESH_DEFORM")
m = wrong.modifiers[w]
assert m["type"] == "MESH_DEFORM" and m["name"] == "Mesh Deform" and m["kind"] == "Grid", m
assert not close(wrong.evaluated()[0][0], (-0.5, 0.0)), "switched, it bends"
doc.frame_current = 0
# A closed four-sided shape fits both: a cage to Cage Deform, one grid cell to Mesh Deform.
c = deform(wrong, cage, cage.shapes[0][0], "CAGE_DEFORM")
assert wrong.modifiers[c]["kind"] == "Cage" and wrong.modifiers[c]["fits"] == "CAGE_DEFORM"
wrong.modifier_set(c, type="MESH_DEFORM")
assert wrong.modifiers[c]["kind"] == "Grid", wrong.modifiers[c]
# An open line fits Mesh Deform too, as a wire (a bendy bone), and Curve Deform; not Cage Deform.
wrong.modifier_set(c, layer="Line", shape=line.shapes[0][0])
assert wrong.modifiers[c]["kind"] == "Wire" and wrong.modifiers[c]["fits"] == "MESH_DEFORM"
wrong.modifier_set(c, type="CAGE_DEFORM")
assert wrong.modifiers[c]["kind"] == "" and wrong.modifiers[c]["fits"] == "CURVE_DEFORM"

# --- Saved --------------------------------------------------------------------------------------------------
art.modifier_set(i, weights="CUSTOM", precision=0.05, resample_self=True)
folder = tempfile.mkdtemp()
try:
    path = os.path.join(folder, "deform.oanim")
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
    art = next(l for l in oanim.data.layers.all() if l.name == "Art")
    m = art.modifiers[i]
    assert m["type"] == "MESH_DEFORM" and m["layer"] == "Grid" and m["kind"] == "Grid", m
    assert m["weights"] == "CUSTOM" and abs(m["precision"] - 0.05) < 1e-6, m
    assert m["resample_self"] is True, m
    saved = next(l for l in oanim.data.layers.all() if l.name == "Art2").modifiers[j]
    assert saved["cage_method"] == "ENVELOPE" and abs(saved["blend"] - 0.25) < 1e-6, saved
    art.modifier_set(i, resample_self=False)
    oanim.data.document.frame_current = 10
    assert abs(art.evaluated()[0][1][1] - 0.5) < 1e-3, "with Precision a point on the moved node still follows it"
    oanim.data.document.frame_current = 0

    # A file from when there was one Vector Deform: each opens as the deform its shape fits, its name with it.
    import zipfile
    old = os.path.join(folder, "old.oanim")
    with zipfile.ZipFile(path) as src, zipfile.ZipFile(old, "w", zipfile.ZIP_DEFLATED) as dst:
        for item in src.infolist():
            data = src.read(item.filename)
            if item.filename.endswith(".json"):
                text = data.decode("utf-8")
                for kind, name in (("CURVE_DEFORM", "Curve Deform"), ("CAGE_DEFORM", "Cage Deform"),
                                   ("MESH_DEFORM", "Mesh Deform")):
                    text = text.replace('"' + kind + '"', '"VECTOR_DEFORM"').replace('"' + name, '"Vector Deform')
                data = text.encode("utf-8")
            dst.writestr(item, data)
    assert ops.wm.open_mainfile(filepath=old, confirmed="1") == {'FINISHED'}
    by_name = {l.name: l for l in oanim.data.layers.all()}
    assert by_name["Art"].modifiers[0]["type"] == "MESH_DEFORM", by_name["Art"].modifiers[0]
    assert by_name["Art2"].modifiers[0]["type"] == "CAGE_DEFORM" and by_name["Art2"].modifiers[0]["name"] == "Cage Deform", by_name["Art2"].modifiers[0]
    assert by_name["Art3"].modifiers[0]["type"] == "CURVE_DEFORM", by_name["Art3"].modifiers[0]
    assert by_name["Art"].modifiers[0]["group"] == "Vector Deform", "its painted weights keep their group"
finally:
    shutil.rmtree(folder, ignore_errors=True)

print("Vector Deform tests passed")
