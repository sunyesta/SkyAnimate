"""Resample Self really splits: a deform cuts each segment as the Resample modifier does and bends the pieces, on every
frame (frame 0 too, where it looks the same), leaving the drawing's own points, keys and handles alone. New cages start
on Envelope with it on. A deform's Custom weights reach the cuts; a cage's or mesh's shape is read by its own points.
The cuts and the split are remembered, so after any change a layer that has split before must come out exactly as a twin
splitting for the first time, for the deform and for the Resample modifier alike.

Run with:  OpenAnimate --background --python tests/test_resample_self.py
"""
import math
import os
import tempfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


def add_deform(layer, target, kind="CAGE_DEFORM", **settings):
    layers.active = layer
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    index = len(layer.modifiers) - 1
    layer.modifier_set(index, layer=target.name, shape=target.shapes[0][0], **settings)
    return index


def worst_gap(a, b):
    return max(math.dist(p, q) for p, q in zip(a, b))


cage = new_layer("Cage")
cage.add_curve([(-1, -1), (1, -1), (1, 1), (-1, 1)], closed=True, fill=True)
rest = [tuple(p[:2]) for p in cage.points]
line = new_layer("Line")
line.add_curve([(0, 0.5), (1, 0.5)])

# --- What a new one starts as -----------------------------------------------------------------------------------------
art = new_layer("Art")
art.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)], closed=True, fill=True)
stored = [tuple(p[:2]) for p in art.points]
i = add_deform(art, cage)
m = art.modifiers[i]
assert m["cage_method"] == "ENVELOPE" and m["resample_self"] is True, m
assert m["split"] == "COUNT" and m["pieces"] == 4, m
for kind in ("CURVE_DEFORM", "MESH_DEFORM"):
    layers.active = art
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    other = art.modifiers[-1]
    assert other["resample_self"] is False and other["cage_method"] == "MEAN_VALUE", other
    assert ops.object.modifier_remove(index=str(len(art.modifiers) - 1)) == {'FINISHED'}
layers.active = art
assert ops.object.modifier_add(type="FOLLOW_DEFORM") == {'FINISHED'}
assert art.modifiers[-1]["cage_method"] == "ENVELOPE", "it goes where the cage it rides on puts things"
assert ops.object.modifier_remove(index=str(len(art.modifiers) - 1)) == {'FINISHED'}

# --- Frame 0: split, and just the same to look at -----------------------------------------------------------------------
assert doc.frame_current == 0
points = art.evaluated()[0]
assert len(points) == 16, len(points)
assert [tuple(p[:2]) for p in points[:4]] == stored, "its own points first, where they are"
assert art.point_count == 4 and [tuple(p[:2]) for p in art.points] == stored, "nothing added to the drawing"
area_at_rest = art.evaluated()[2][0][4]
art.modifier_set(i, resample_self=False)
# (Its area is summed in straight pieces, which more points fill a little better; test_resample_deform measures the line.)
assert abs(area_at_rest - art.evaluated()[2][0][4]) < 0.003, "the split is exact"
art.modifier_set(i, resample_self=True)

# --- Bent: as the Resample modifier and the deform together --------------------------------------------------------------
doc.frame_current = 10
bend = [(rest[0][0] - 0.3, rest[0][1]), (rest[1][0] + 0.4, rest[1][1] - 0.2), (rest[2][0], rest[2][1] + 0.3), rest[3]]
for k, q in enumerate(bend):
    cage.set_point(k, q)
self_points, _, self_shapes = art.evaluated()
assert len(self_points) == 16, "the same pieces on every frame"
assert art.point_count == 4 and [tuple(p[:2]) for p in art.points] == stored, "no keys, nothing moved"
doc.frame_current = 0
split = new_layer("Split")
split.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)], closed=True, fill=True)
assert ops.object.modifier_add(type="RESAMPLE") == {'FINISHED'}
split.modifier_set(0, split="COUNT", pieces=4)
add_deform(split, cage, resample_self=False, precision=0.0)
doc.frame_current = 10
split_points, _, split_shapes = split.evaluated()
assert len(split_points) == 16
assert worst_gap(self_points, split_points) < 1e-5, worst_gap(self_points, split_points)
assert abs(self_shapes[0][4] - split_shapes[0][4]) < 1e-5, (self_shapes, split_shapes)
# Between its points the line follows the bend, which one curve to a side can't.
pieces = worst_gap(self_points[4:], [tuple(p[:2]) for p in self_points[:12]])
assert pieces > 0.01, "the cuts bent"

# By Length: as many pieces as the setup pose's length takes, whatever the pose.
art.modifier_set(i, split="LENGTH", spacing=0.3)
assert len(art.evaluated()[0]) == 16, "four pieces of 0.25 a side"
art.modifier_set(i, spacing=0.6)
assert len(art.evaluated()[0]) == 8, "two pieces a side"
doc.frame_current = 0
assert len(art.evaluated()[0]) == 8
art.modifier_set(i, spacing=2.0)
assert len(art.evaluated()[0]) == 4, "Spacing longer than every segment: nothing to split"
art.modifier_set(i, split="COUNT")
doc.frame_current = 10

# Off, and Strength 0 keeps the pieces.
art.modifier_set(i, resample_self=False, precision=0.0)
assert len(art.evaluated()[0]) == 4
art.modifier_set(i, resample_self=True, strength=0.0)
assert len(art.evaluated()[0]) == 16
art.modifier_set(i, strength=1.0)

# --- Custom weights reach the cuts ---------------------------------------------------------------------------------------
doc.frame_current = 0
held = new_layer("Held")
held.add_curve([(-0.5, 0.0), (0.0, 0.0), (0.5, 0.0)])
h = add_deform(held, cage)
held.modifier_set(h, weights="CUSTOM")
ops.mesh.select_none()
held.select_points([0, 1, 2])
assert ops.mesh.point_group_assign(weight="0") == {'FINISHED'}
doc.frame_current = 10
for got, want in zip(held.evaluated()[0], [(-0.5 + 0.25 * k, 0.0) for k in (0, 2, 4)]):
    assert math.dist(got[:2], want) < 1e-5, "its own points stay"
pts = held.evaluated()[0]
assert len(pts) == 9
assert all(abs(p[1]) < 1e-5 for p in pts), ("weight 0 at both ends: its cuts stay too", pts)
doc.frame_current = 0

# And those a Resample modifier above the deform cut, or Precision.
above = new_layer("Above")
above.add_curve([(-0.5, 0.0), (0.0, 0.0), (0.5, 0.0)])
assert ops.object.modifier_add(type="RESAMPLE") == {'FINISHED'}
above.modifier_set(0, split="COUNT", pieces=4)
a = add_deform(above, cage, resample_self=False, precision=0.0)
above.modifier_set(a, weights="CUSTOM")
ops.mesh.select_none()
above.select_points([0, 1, 2])
assert ops.mesh.point_group_assign(weight="0") == {'FINISHED'}
doc.frame_current = 10
pts = above.evaluated()[0]
assert len(pts) == 9 and all(abs(p[1]) < 1e-5 for p in pts), ("the modifier's cuts stay too", pts)
doc.frame_current = 0

ops.layer.new()
half = layers.active
half.name = "Half"
half.add_curve([(-0.5, -0.8), (0.5, -0.8)])
full = new_layer("Full")
full.add_curve([(-0.5, -0.8), (0.5, -0.8)])
add_deform(full, cage, resample_self=False, precision=0.0005)
h = add_deform(half, cage, resample_self=False, precision=0.0005)
half.modifier_set(h, weights="CUSTOM")
ops.mesh.select_none()
half.select_points([0, 1])
assert ops.mesh.point_group_assign(weight="0.5") == {'FINISHED'}
doc.frame_current = 10
half_pts, full_pts = half.evaluated()[0], full.evaluated()[0]
assert len(half_pts) > 2 and len(full_pts) == len(half_pts), (len(half_pts), len(full_pts))
for own, (p, q) in enumerate(zip(half_pts, full_pts)):
    # Half way from where it was on the line to where the whole deform puts it, the cuts as the two ends.
    moved, whole = abs(p[1] + 0.8), abs(q[1] + 0.8)
    assert moved <= whole * 0.6 + 1e-3, ("half weight: its cuts move half way too", own, p, q)
doc.frame_current = 0

# --- A group's cage splits the drawings inside --------------------------------------------------------------------------
ops.layer.new_group()
group = layers.active
group.name = "Group"
inner = new_layer("Inner")
inner.add_curve([(-0.4, 0.1), (0.4, 0.1)])
assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
inner = next(l for l in layers.all() if l.name == "Inner")
add_deform(group, cage)
assert group.modifiers[0]["resample_self"] is True
assert len(inner.evaluated()[0]) == 5, inner.evaluated()[0]
doc.frame_current = 10
assert len(inner.evaluated()[0]) == 5
doc.frame_current = 0

# --- A shape read by another deform keeps its own points -----------------------------------------------------------------
warp = new_layer("Warp")
warp.add_curve([(-2, -2), (2, -2), (2, 2), (-2, 2)], closed=True, fill=True)
add_deform(warp, cage)
rider = new_layer("Rider")
rider.add_curve([(-0.2, 0.0), (0.2, 0.0)])
add_deform(rider, warp, cage_method="MEAN_VALUE", resample_self=False, precision=0.0)
assert len(warp.evaluated()[0]) == 16 and len(rider.evaluated()[0]) == 2
doc.frame_current = 10
assert len(rider.evaluated()[0]) == 2
was = rider.evaluated()[0]
assert worst_gap(was, [(-0.2, 0.0), (0.2, 0.0)]) > 1e-3, ("the rider follows the warp it reads", was)
doc.frame_current = 0

# --- Saved ------------------------------------------------------------------------------------------------------------
art.modifier_set(i, split="LENGTH", spacing=0.2, pieces=7)
folder = tempfile.mkdtemp()
try:
    path = os.path.join(folder, "resample_self.oanim")
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
    art = next(l for l in layers.all() if l.name == "Art")
    m = art.modifiers[i]
    assert m["resample_self"] is True and m["split"] == "LENGTH", m
    assert abs(m["spacing"] - 0.2) < 1e-6 and m["pieces"] == 7, m
finally:
    if os.path.exists(path):
        os.remove(path)
    os.rmdir(folder)
art.modifier_set(i, split="COUNT", pieces=4)

# --- Remembered cuts and splits: never stale -------------------------------------------------------------------------
doc.frame_current = 0
cage = next(l for l in layers.all() if l.name == "Cage")
LINE = [(-0.6, 0.0), (-0.1, 0.4), (0.3, -0.3), (0.7, 0.1)]
twins = 0


def make(name, kind):
    """A line split by a Resample modifier or a deform's Resample Self, by length."""
    layer = new_layer(name)
    layer.add_curve(LINE)
    if kind == "RESAMPLE":
        assert ops.object.modifier_add(type="RESAMPLE") == {'FINISHED'}
        layer.modifier_set(0, split="LENGTH", spacing=0.2)
    else:
        add_deform(layer, cage, split="LENGTH", spacing=0.2)
    return layer


def same_as_fresh(layer, kind, what):
    """`layer` (which has split before) against a new twin, built the same way on frame 0 and given the same changes."""
    global twins
    twins += 1
    frame = doc.frame_current
    doc.frame_current = 0
    twin = make("Twin%d" % twins, kind)
    m = layer.modifiers[0]
    twin.modifier_set(0, split=m["split"], spacing=m["spacing"], pieces=m["pieces"])
    for k, p in setup.get(layer.id, []):
        twin.set_point(k, p)
    # (Leaving a frame records what was done on it.)
    doc.frame_current = 1
    doc.frame_current = frame
    for k, p in enumerate(layer.points):
        if frame != 0:
            twin.set_point(k, tuple(p[:2]))
    a, b = layer.evaluated(), twin.evaluated()
    assert len(a[0]) == len(b[0]), (what, kind, len(a[0]), len(b[0]))
    assert worst_gap(a[0], b[0]) < 1e-5, (what, kind, worst_gap(a[0], b[0]))
    twin_id = twin.id
    doc.frame_current = 0
    layers.active = twin
    assert ops.layer.delete() == {'FINISHED'}
    assert not any(l.id == twin_id for l in layers.all())
    doc.frame_current = frame
    return a[0]


setup = {}
for kind in ("RESAMPLE", "CAGE_DEFORM"):
    doc.frame_current = 0
    layer = make("Remembered " + kind, kind)
    first = same_as_fresh(layer, kind, "start")

    # The setup pose changes: a segment longer, so more pieces and new cuts.
    layer.set_point(3, (1.6, 0.1))
    setup[layer.id] = [(3, (1.6, 0.1))]
    longer = same_as_fresh(layer, kind, "setup edited")
    assert len(longer) > len(first), (len(longer), len(first))

    # Back as it was: the cuts before.
    layer.set_point(3, LINE[3])
    setup[layer.id] = [(3, LINE[3])]
    assert len(same_as_fresh(layer, kind, "setup put back")) == len(first)

    # A pose on another frame: the same cuts, the split following the points.
    doc.frame_current = 10
    posed = same_as_fresh(layer, kind, "a frame")
    layer.set_point(1, (-0.1, 0.9))
    moved = same_as_fresh(layer, kind, "a key")
    assert len(moved) == len(posed), "a pose doesn't change the pieces"
    assert worst_gap(moved, posed) > 1e-3, "and they follow it"
    doc.frame_current = 0
    assert len(same_as_fresh(layer, kind, "back on frame 0")) == len(first)

    # The settings.
    for settings in ({"spacing": 0.1}, {"split": "COUNT"}, {"pieces": 2}, {"split": "LENGTH", "spacing": 0.2}):
        layer.modifier_set(0, **settings)
        same_as_fresh(layer, kind, str(settings))

# Undo itself, on a drawing that has split on both sides of it.
doc.frame_current = 0
layer = make("Undone", "RESAMPLE")
before = len(layer.evaluated()[0])
ops.mesh.select_none()
layer.select_points([3])
assert ops.mesh.translate(move_x="1.5") == {'FINISHED'}
stretched = len(layer.evaluated()[0])
assert stretched > before, (stretched, before)
ops.ed.undo()
layer = next(l for l in layers.all() if l.name == "Undone")
assert len(layer.evaluated()[0]) == before, "undo brings the cuts back"
ops.ed.redo()
layer = next(l for l in layers.all() if l.name == "Undone")
assert len(layer.evaluated()[0]) == stretched

print("resample self: ok,", twins, "twins")
