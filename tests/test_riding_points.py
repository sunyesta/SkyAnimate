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

# Past detail that has moved it goes a level up, an exact split all the same, and shows and deletes as ever.
twin, layer = posed(), posed()
for x in (twin, layer):
    layers.active = x
    x.set_detail_level(1, [0])
    shown = x.split_segment(0, 0, 0.5)
    move(x, [shown], (0.06, 0.06), 0)
    x.set_detail_level(0, [0])
before = rings(twin)
layers.active = layer
new = layer.split_segment(0, 0, 0.25)
detail = layer.detail
assert detail["rides"][new] and detail["levels"][new] == 2 and not detail["hidden"][new], detail
assert worst(before, rings(layer)) < 1e-3, worst(before, rings(layer))
layer.select_points(list(range(layer.point_count)), False)
layer.select_points([new], True)
assert ops.mesh.delete(type="POINTS") == {"FINISHED"}
assert worst(before, rings(layer)) < 1e-3

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
assert own_keys(layer, new) == [] and layer.keyframes("points[%d].radius" % new) == [], "following the line keys nothing"


def set_width(layer, points, width, frame):
    layers.active = layer
    doc.frame_current = frame
    layer.select_points(list(range(layer.point_count)), False)
    layer.select_points(points, True)
    layer.mean_radius = width
    doc.frame_current = frame + 1
    doc.frame_current = frame


# A line whose only keys are its width moves too.
ops.layer.new()
line = layers.active
line.add_curve([(-0.5, 0.0), (0.0, 0.3), (0.5, 0.0)])
set_width(line, [0], 0.2, 10)
doc.frame_current = 0
new = line.split_segment(0, 0, 0.5)
assert line.detail["rides"][new]
doc.frame_current = 10
assert line.radii[new] < 0.8, line.radii

# Its own Width key adds to the line's, so it goes on following it.
set_width(line, [new], 0.5, 10)
assert abs(line.radii[new] - 0.5) < 1e-3 and line.keyframes("points[%d].radius" % new), line.radii
set_width(line, [1], 0.6, 10)
assert line.radii[new] < 0.5 - 0.1, line.radii
doc.frame_current = 0
assert abs(line.radii[new] - 1.0) < 1e-3, line.radii
shown = line.radii[new]
layers.active = line
line.select_points(list(range(line.point_count)), False)
line.select_points([new], True)
doc.frame_current = 10
want = line.radii[new]
doc.frame_current = 0
assert ops.mesh.make_independent() == {"FINISHED"}
doc.frame_current = 10
assert abs(line.radii[new] - want) < 2e-3, "independent, its width is what it was"
doc.frame_current = 0

# --- Other edits leave the frames alone -------------------------------------------------------------------------------
def line_of(points, *keys):
    ops.layer.new()
    line = layers.active
    line.add_curve(points)
    for point, by, frame in keys:
        move(line, [point], by, frame)
    return line


def shown_rows(line, frames=(0, 10, 20)):
    rows = {f: shown_line(line, f) for f in frames}
    doc.frame_current = 0
    return rows


def shown_line(line, frame):
    doc.frame_current = frame
    return [tuple(p[:2]) for s in range(line.shape_count) for p in line.evaluated_outline(s, 48)[0]]


def to_line(p, path):
    best = 1e9
    for q, r in zip(path, path[1:]):
        dx, dy = r[0] - q[0], r[1] - q[1]
        t = max(0.0, min(1.0, ((p[0] - q[0]) * dx + (p[1] - q[1]) * dy) / max(dx * dx + dy * dy, 1e-12)))
        best = min(best, math.hypot(p[0] - q[0] - dx * t, p[1] - q[1] - dy * t))
    return best


def stayed(line, rows):
    worst_off = max(to_line(p, rows[f]) for f in rows for p in shown_rows(line, (f,))[f])
    assert worst_off < 2.5e-3, worst_off


WAVE = [(-0.6 + 0.24 * i, 0.2 * math.sin(i)) for i in range(6)]
KEYS = ((1, (0.15, -0.2), 10), (3, (0.1, 0.25), 10), (3, (-0.1, -0.2), 20))

# What would leave one a line's end makes it independent first: an end has nothing to ride.
for how in ("point", "edge", "edges menu", "separate"):
    line = line_of(WAVE, *KEYS)
    layers.active = line
    new = line.split_segment(0, 0, 0.5) if how == "point" else line.split_segment(0, 2, 0.5)
    rows = shown_rows(line)
    points = line.curve_points(0)
    at = points.index(new)
    picked = {"point": [0], "edge": [points[at - 1], new], "edges menu": [points[at - 1], new],
              "separate": [new, points[at + 1], points[at + 2]]}[how]
    line.select_points(list(range(line.point_count)), False)
    line.select_points(picked, True)
    if how == "point":
        assert ops.mesh.delete(type="POINTS") == {"FINISHED"}
    elif how == "edge":
        ops.mesh.select_mode_edge()
        assert ops.mesh.delete(type="POINTS") == {"FINISHED"}
        ops.mesh.select_mode_point()
    elif how == "edges menu":
        assert ops.mesh.delete_edges() == {"FINISHED"}
    else:
        assert ops.mesh.separate() == {"FINISHED"}
    assert not any(line.detail["rides"]), how
    stayed(line, rows)

# On a posed frame too, and it stays on that frame.
line = line_of(WAVE, *KEYS)
layers.active = line
line.split_segment(0, 0, 0.5)
doc.frame_current = 7
line.select_points(list(range(line.point_count)), False)
line.select_points([0], True)
assert ops.mesh.delete_points() == {"FINISHED"}
assert doc.frame_current == 7
doc.frame_current = 0

# The X menu's Delete Points gives the curve back too.
twin, layer = posed(), posed()
before = rings(twin)
layers.active = layer
new = layer.split_segment(0, 0, 0.5)
layer.select_points(list(range(layer.point_count)), False)
layer.select_points([new], True)
assert ops.mesh.delete_points() == {"FINISHED"} and layer.point_count == 4
assert worst(before, rings(layer)) < 1e-3

# A line welded to one takes it for its end and goes with it, by any of the ways of welding.
for how in ("segment", "merge"):
    twin, layer = line_of(WAVE, *KEYS), line_of(WAVE, *KEYS)
    before = rings_of = {f: shown_line(twin, f) for f in (0, 10, 20)}
    layers.active = layer
    new = layer.split_segment(0, 2, 0.5)
    at = layer.points[new]
    layer.add_curve([(1.5, 1.5), (at[0], at[1] + (0.0 if how == "merge" else 0.2))])
    n = layer.point_count
    layer.select_points(list(range(n)), False)
    layer.select_points([new, n - 2 if how == "segment" else n - 1], True)
    assert (ops.mesh.make_segment() if how == "segment" else ops.mesh.merge()) == {"FINISHED"}
    assert layer.detail["rides"][new], how
    for f in (0, 10, 20):
        doc.frame_current = f
        outline = [tuple(p[:2]) for p in layer.evaluated_outline(0, 48)[0]]
        assert max(to_line(p, outline) for p in before[f]) < 1e-3, (how, f)
    doc.frame_current = 0

# Detail levels: riding points aren't detail, so Delete Higher leaves them, and Rebuild Levels leaves their curves.
twin, layer = posed(), posed()
before = rings(twin)
layers.active = layer
new = layer.split_segment(0, 0, 0.5)
layer.select_points(list(range(layer.point_count)), True)
ops.mesh.detail_delete_higher()
assert layer.point_count == 5 and layer.detail["rides"][new]
ops.mesh.detail_rebuild(tolerance="0.01")
assert layer.detail["rides"][new]
assert worst(before, rings(layer)) < 1e-3

# But one on detail that goes, goes with it.
layer = posed()
layers.active = layer
layer.set_detail_level(1, [0])
shown = layer.split_segment(0, 0, 0.5)
move(layer, [shown], (0.06, 0.06), 0)
layer.set_detail_level(0, [0])
layer.split_segment(0, 0, 0.25)
layer.select_points(list(range(layer.point_count)), True)
assert ops.mesh.detail_delete_higher() == {"FINISHED"} and layer.point_count == 4

# What makes one the curve's own holds each pose as Make Independent does.
for name in ("detail_apply", "detail_lower"):
    twin, layer = posed(), posed()
    before = rings(twin)
    layers.active = layer
    new = layer.split_segment(0, 0, 0.5)
    layer.select_points(list(range(layer.point_count)), False)
    layer.select_points([new, 0], True)
    assert getattr(ops.mesh, name)() == {"FINISHED"}
    assert not layer.detail["rides"][new] and layer.detail["levels"][new] == 0 and own_keys(layer, new)
    after = rings(layer)
    assert max(off(before[f], after[f]) for f in (0, 10, 20)) < 1e-3, name

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

# One of two on a segment: the other stays riding, and the line where it was, its handles as they show.
twin, layer = posed(), posed()
before = rings(twin)
layers.active = layer
a = layer.split_segment(0, 0, 0.5)
b = layer.split_segment(0, 1, 0.5)
layer.select_points(list(range(layer.point_count)), False)
layer.select_points([a], True)
assert ops.mesh.make_independent() == {"FINISHED"}
assert layer.detail["rides"][b] and own_keys(layer, b) == []
after = rings(layer)
assert max(off(before[f], after[f]) for f in (0, 10, 20)) < 1e-3

# Beside one moved off the line, the line holds there too.
layer = posed()
layers.active = layer
a = layer.split_segment(0, 0, 0.4)
b = layer.split_segment(0, 1, 0.5)
move(layer, [a], (0.05, 0.03), 10)
move(layer, [b], (-0.03, 0.04), 20)
before = rings(layer)
layer.select_points(list(range(layer.point_count)), False)
layer.select_points([a], True)
assert ops.mesh.make_independent() == {"FINISHED"}
after = rings(layer)
assert max(off(before[f], after[f]) for f in (0, 10, 20)) < 1.5e-3, max(off(before[f], after[f]) for f in (0, 10, 20))

# Past moved detail, all the way down.
layer = posed()
layers.active = layer
layer.set_detail_level(1, [0])
shown = layer.split_segment(0, 0, 0.5)
move(layer, [shown], (0.06, 0.06), 0)
layer.set_detail_level(0, [0])
before = rings(layer)
new = layer.split_segment(0, 0, 0.25)
assert layer.detail["levels"][new] == 2
layer.select_points(list(range(layer.point_count)), False)
layer.select_points([new], True)
assert ops.mesh.make_independent() == {"FINISHED"}
assert layer.detail["levels"][new] == 0 and not layer.detail["rides"][new]
after = rings(layer)
assert max(off(before[f], after[f]) for f in (0, 10, 20)) < 2e-3, max(off(before[f], after[f]) for f in (0, 10, 20))

# The action open stays open, even one of another rig.
ops.layer.new_bone()
other = layers.active
ops.armature.bone_add(name="other", head_x="5", head_y="0", tail_x="6", tail_y="0", parent="")
assert ops.anim.action_new(name="other rig", smart="0") == {"FINISHED"}
layer = posed()
layers.active = layer
new = layer.split_segment(0, 0, 0.5)
layer.select_points(list(range(layer.point_count)), False)
layer.select_points([new], True)
assert ops.mesh.make_independent() == {"FINISHED"}
assert doc.action == "other rig", doc.action
assert ops.anim.action_set(name="") == {"FINISHED"}

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

# It is drawn and picked where it shows, on the bent line, not where its own weights would put it.
for f in FRAMES:
    outline = ring(layer, f, shown=True)
    drawn = layer.shown()[0][new]
    assert to_line(drawn, outline + outline[:1]) < 2e-3, (f, drawn)

# Put somewhere on a posed frame, it goes there.
doc.frame_current = 10
at = layer.shown()[0][new]
goal = (at[0] + 0.1, at[1] + 0.05)
layer.select_points(list(range(layer.point_count)), False)
layer.select_points([new], True)
layer.place_point(new, goal)
doc.frame_current = 11
doc.frame_current = 10
drawn = layer.shown()[0][new]
assert math.hypot(drawn[0] - goal[0], drawn[1] - goal[1]) < 2e-3, (goal, drawn)
outline = ring(layer, 10, shown=True)
assert to_line(goal, outline + outline[:1]) < 4e-3, "the line goes there with it"
doc.frame_current = 0

# Resample before the bones takes it for the line's: it bends as the line does, by the weights there.
twin, layer = rig(), rig()
for x in (twin, layer):
    layers.active = x
    assert ops.object.modifier_add(type="RESAMPLE") == {"FINISHED"}
before = rings(twin, shown=True)
layers.active = layer
layer.split_segment(0, 1, 0.5)
assert worst(before, rings(layer, shown=True)) < 0.04, worst(before, rings(layer, shown=True))

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

# --- Smart actions: Make Independent keys the smart action and what the mainline adds, not the same move twice ---------
ops.wm.read_homefile(confirmed="1")
ops.layer.new_bone()
dial = layers.active
ops.armature.bone_add(name="dial", head_x="3", head_y="0", tail_x="4", tail_y="0", parent="")


def on_dial():
    ops.layer.new()
    shape = layers.active
    ops.layer.move_to_group(group=str(dial.id))
    ops.mesh.primitive_circle_add(x=0.0, y=0.0, width=1.0, height=1.0, fill="1")
    return shape


twin, layer = on_dial(), on_dial()
layers.active = dial
dial.select_bone("dial")
assert ops.anim.action_new(name="dial", smart="1") == {"FINISHED"}
doc.frame_current = 10
dial.bone_set("dial", angle=1.0)
for x in (twin, layer):
    layers.active = x
    x.select_points(list(range(x.point_count)), False)
    x.select_points([1], True)
    x.move_points((0.3, 0.3))
doc.frame_current = 0
assert ops.anim.action_set(name="") == {"FINISHED"}
layers.active = dial
doc.frame_current = 10
dial.bone_set("dial", angle=0.7)
doc.frame_current = 11
doc.frame_current = 0
before = rings(twin, shown=True)
layers.active = layer
new = layer.split_segment(0, 0, 0.5)
assert layer.detail["rides"][new]
assert worst(before, rings(layer, shown=True)) < 1e-3, worst(before, rings(layer, shown=True))
layer.select_points(list(range(layer.point_count)), False)
layer.select_points([new], True)
assert ops.mesh.make_independent() == {"FINISHED"}
after = rings(layer, shown=True)
assert max(off(before[f], after[f]) for f in (0, 10)) < 2e-3, max(off(before[f], after[f]) for f in (0, 10))

print("riding points: ok")
