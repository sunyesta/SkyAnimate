"""Riding points: a point added to a curve that moves changes no frame, and needs no keys until it is moved itself.

Each case is measured against a twin drawing that never had the point.

Run with:  OpenAnimate --background --python tests/test_riding_points.py
"""

import math
import pathlib
import tempfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
FRAMES = (0, 5, 10, 15, 20)


def off(a, b):
    """How far the furthest of closed outline `a` is from closed outline `b`."""

    def to_piece(p, q, r):
        dx, dy = r[0] - q[0], r[1] - q[1]
        t = max(0.0, min(1.0, ((p[0] - q[0]) * dx + (p[1] - q[1]) * dy) / max(dx * dx + dy * dy, 1e-12)))
        return math.hypot(p[0] - q[0] - dx * t, p[1] - q[1] - dy * t)

    return max(min(to_piece(p, q, r) for q, r in zip(b, b[1:] + b[:1])) for p in a)


def ring(layer, frame, shown=False):
    doc.frame_current = frame
    outline = layer.evaluated_outline(0, 48) if shown else layer.shape_outline(0, 48)
    return [tuple(p[:2]) for p in outline[0]]


def rings(layer, shown=False):
    made = {f: ring(layer, f, shown) for f in FRAMES}
    doc.frame_current = 0
    return made


def worst(a, b):
    return max(off(a[f], b[f]) for f in FRAMES)


def posed():
    """A circle keyed on frames 10 and 20."""
    ops.layer.new()
    layer = layers.active
    ops.mesh.primitive_circle_add(x=0.0, y=0.0, width=1.0, height=1.0, fill="1")
    for frame, path in ((10, "0.5,0.0;0.8,0.0"), (20, "0.0,0.5;0.0,0.9")):
        doc.frame_current = frame
        ops.sculpt.stroke(brush="GRAB", path=path, radius=0.3, strength=1.0, falloff="CONSTANT")
    doc.frame_current = 0
    return layer


def move(layer, points, by, frame):
    layers.active = layer
    doc.frame_current = frame
    layer.select_points(list(range(layer.point_count)), False)
    layer.select_points(points, True)
    layer.move_points(by)
    doc.frame_current = 0


def own_keys(layer, point):
    return [k["frame"] for k in layer.keyframes("points[%d].pos" % point)]


# --- Adding one changes no frame, and touches no key ------------------------------------------------------------------
twin, layer = posed(), posed()
before = rings(twin)
layers.active = layer
keys_before = [own_keys(layer, i) for i in range(4)]
new = layer.split_segment(0, 0, 0.5)
assert layer.point_count == 5
assert worst(before, rings(layer)) < 1e-4, "like it was never added"
assert own_keys(layer, new) == [], "no keys of its own"
assert [own_keys(layer, i) for i in range(4)] == keys_before, "its neighbours' keys are as they were"

# It looks like any other point: shown at level 0, and not counted as detail.
detail = layer.detail
assert detail["rides"][new] and not detail["hidden"][new]
assert detail["shape_max_levels"][0] == 0, detail["shape_max_levels"]
assert new in detail["wires"][0], "on the wire with the others"

# Poses made later: a neighbour moved on a keyed frame, or a new key: it comes along.
move(twin, [1], (0.15, 0.1), 10)
move(layer, [1], (0.15, 0.1), 10)
assert worst(rings(twin), rings(layer)) < 1e-3
assert own_keys(layer, new) == [], "still no keys"
move(twin, [0], (-0.1, 0.2), 30)
move(layer, [0], (-0.1, 0.2), 30)
assert off(ring(twin, 30), ring(layer, 30)) < 1e-3
doc.frame_current = 0

# Moved itself on a posed frame: it keys there, and rides everywhere else.
was = rings(layer)
move(layer, [new], (0.05, 0.05), 10)
assert own_keys(layer, new) == [0, 10], own_keys(layer, new)
doc.frame_current = 10
assert layer.detail["rides"][new]
now = rings(layer)
assert off(was[10], now[10]) > 0.02, "it moved"
assert off(was[0], now[0]) < 1e-4, "the setup is as it was"

# Deleting an unused one gives back the old line, on every frame.
twin, layer = posed(), posed()
before = rings(twin)
layers.active = layer
new = layer.split_segment(0, 0, 0.5)
layer.select_points(list(range(layer.point_count)), False)
layer.select_points([new], True)
assert ops.mesh.delete(type="POINTS") == {"FINISHED"}
assert layer.point_count == 4, layer.point_count
assert worst(before, rings(layer)) < 1e-3

# Moved on the setup frame it reshapes the setup, and a shape keeps its motion.
twin, layer = posed(), posed()
layers.active = layer
new = layer.split_segment(0, 0, 0.5)
at = layer.points[new]
move(layer, [new], (0.1, 0.1), 0)
assert abs(layer.points[new][0] - at[0] - 0.1) < 1e-3
doc.frame_current = 10
assert layer.points[new][0] > at[0] + 0.1, "the pose carries it on from there"
doc.frame_current = 0

# Subdivide adds them too, however many.
twin, layer = posed(), posed()
before = rings(twin)
layers.active = layer
layer.select_points(list(range(layer.point_count)), False)
layer.select_points([0, 1], True)
assert ops.mesh.subdivide(number_cuts="3") == {"FINISHED"}
assert layer.point_count == 7, layer.point_count
assert all(layer.detail["rides"][4:])
assert worst(before, rings(layer)) < 1e-3, worst(before, rings(layer))

# Saved and opened: it still rides, on no keys.
twin, layer = posed(), posed()
before = rings(twin)
layers.active = layer
new = layer.split_segment(0, 0, 0.5)
name = layer.name
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_riding_")) / "riding.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {"FINISHED"}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {"FINISHED"}
layer = next(l for l in layers.all() if l.name == name)
assert layer.detail["rides"][new]
assert worst(before, rings(layer)) < 1e-3

# --- Only where the curve moves ---------------------------------------------------------------------------------------
ops.layer.new()
still = layers.active
ops.mesh.primitive_circle_add(x=0.0, y=0.0, width=1.0, height=1.0, fill="1")
new = still.split_segment(0, 0, 0.5)
assert not still.detail["rides"][new] and still.detail["levels"][new] == 0, "an ordinary point"

# A shape's own detail still works as it did: a shown detail point doesn't ride.
twin = posed()
twin.set_detail_level(1, [0])
layers.active = twin
shown = twin.split_segment(0, 0, 0.5)
assert not twin.detail["rides"][shown] and twin.detail["levels"][shown] == 1

# --- Width -----------------------------------------------------------------------------------------------------------
twin, layer = posed(), posed()
layers.active = layer
new = layer.split_segment(0, 0, 0.5)
for x in (twin, layer):
    layers.active = x
    doc.frame_current = 10
    x.select_points(list(range(x.point_count)), False)
    x.select_points([0], True)
    x.mean_radius = 0.2
    doc.frame_current = 0
doc.frame_current = 10
assert layer.radii[new] < 0.8, ("the line's width there, narrowing with its neighbour", layer.radii)
doc.frame_current = 0

# --- Make Independent ---------------------------------------------------------------------------------------------
twin, layer = posed(), posed()
before = rings(twin)
layers.active = layer
new = layer.split_segment(0, 0, 0.5)
layer.select_points(list(range(layer.point_count)), False)
layer.select_points([new], True)
assert ops.mesh.make_independent() == {"FINISHED"}
detail = layer.detail
assert not detail["rides"][new] and detail["levels"][new] == 0
assert own_keys(layer, new), "keys to hold each pose"
after = rings(layer)
assert max(off(before[f], after[f]) for f in (0, 10, 20)) < 1e-3, "every keyed pose as it was"
assert max(off(before[f], after[f]) for f in (5, 15)) < 0.02, "and near it between keys, as near as keys can hold"
assert ops.mesh.make_independent() == {"CANCELLED"}, "nothing riding any more"

# --- Under bones: placed on the bent curve -------------------------------------------------------------------------


def rig():
    ops.layer.new()
    art = layers.active
    ops.mesh.primitive_circle_add(x=0.0, y=0.0, width=1.0, height=0.6, fill="1")
    ops.layer.new_bone()
    bones = layers.active
    layers.active = art
    ops.layer.move_to_group(group=str(bones.id))
    layers.active = bones
    ops.armature.bone_add(name="a", head_x="-0.5", head_y="0", tail_x="0", tail_y="0")
    ops.armature.bone_add(name="b", head_x="0", head_y="0", tail_x="0.5", tail_y="0")
    layers.active = art
    ops.weight.automatic()
    layers.active = bones
    doc.frame_current = 10
    bones.bone_set("b", angle=1.0)
    doc.frame_current = 11
    doc.frame_current = 0
    layers.active = art
    return art


twin, layer = rig(), rig()
before = rings(twin, shown=True)
layers.active = layer
assert off(before[0], before[10]) > 0.1, "the bone bends it"
new = layer.split_segment(0, 0, 0.5)
assert layer.detail["rides"][new], "a curve on bones moves"
assert worst(before, rings(layer, shown=True)) < 1e-3, worst(before, rings(layer, shown=True))

# --- Cutting a keyed line: its new ends can't ride, so they are keyed to hold each pose -----------------------------
def keyed_line():
    ops.layer.new()
    line = layers.active
    ops.wm.tool_draw(mode="FREEHAND")
    oanim.wm.set_draw(use_pressure=False)
    rows = ";".join("%f,%f,1,%f" % (-0.6 + 1.2 * i / 79.0, 0.3 * math.sin(i * 0.08), i * 0.01) for i in range(80))
    ops.draw.stroke(samples=rows, pixel=2.0 / doc.height)
    doc.frame_current = 10
    ops.sculpt.stroke(brush="GRAB", path="0.3,0.0;0.3,0.4", radius=0.5, strength=1.0, falloff="CONSTANT")
    doc.frame_current = 0
    return line


def to_line(p, path):
    best = 1e9
    for q, r in zip(path, path[1:]):
        dx, dy = r[0] - q[0], r[1] - q[1]
        t = max(0.0, min(1.0, ((p[0] - q[0]) * dx + (p[1] - q[1]) * dy) / max(dx * dx + dy * dy, 1e-12)))
        best = min(best, math.hypot(p[0] - q[0] - dx * t, p[1] - q[1] - dy * t))
    return best


def shown_line(line, frame):
    doc.frame_current = frame
    return [tuple(p[:2]) for s in range(line.shape_count) for p in line.evaluated_outline(s, 48)[0]]


for where, inside in (("a plain line", still), ("a line on bones", layer)):
    layers.active = inside
    twin, line = keyed_line(), keyed_line()
    layers.active = line
    assert any(own_keys(line, i) for i in range(line.point_count)), "keyed"
    was = {f: shown_line(twin, f) for f in (0, 10)}
    doc.frame_current = 0
    ops.wm.tool_draw(mode="STROKE_ERASER")
    oanim.wm.set_draw(fill_size=0.04)
    rows = ";".join("%f,%f,1,%f" % (0.05, -0.8 + 1.6 * i / 59.0, i * 0.01) for i in range(60))
    assert ops.draw.stroke(samples=rows, pixel=2.0 / doc.height) == {"FINISHED"}
    assert line.curve_count == 2, line.curve_count
    assert not any(line.detail["rides"]), "ends are ordinary points"
    for f in (0, 10):
        worst_off = max(to_line(p, was[f]) for p in shown_line(line, f))
        assert worst_off < 2e-3, (where, "what is left of it stays where the line was on frame", f, worst_off)
    doc.frame_current = 0

ops.wm.tool_draw(mode="FREEHAND")
print("riding points: ok")
