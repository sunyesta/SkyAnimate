"""Follow Point's modes, after Harmony's Point Kinematic Output: rotation by one point, Along Line scale, three points.

- One point with Rotation: an Own Place point turns the bone as the bend twists there; a point of the shape turns it as
  the curve's direction does there. New constraints start with Rotation off, and so do old files' one-point ones.
- Two points, Scale Along Line: stretched along the points' line by how far they grow apart, squashed across it by
  (1 / stretch - 1) * Volume + 1, as Harmony's Two-Point Sampling with Scaling.
- Three points: the bone takes the whole affine change they make (turn, both scales and skew), blended by Influence.
  As Harmony's: three points on a line don't move it at all; nearly on a line they skew it a lot.
- The turn, scale and skew go round the first point, as Harmony's go round its first pivot: a head away from it swings.
- Points within Harmony's reach of the first are moved apart before the bend is read, as Harmony does.
- All of it saved with the file.

Run with:  OpenAnimate --background --python tests/test_follow_point_modes.py
"""

import json
import math
import os
import shutil
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
anchor = layers.active


def close(a, b, eps=1e-3):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def new_layer(name):
    layers.active = anchor
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


def new_rig(name):
    layers.active = anchor
    assert ops.layer.new_bone() == {'FINISHED'}
    rig = layers.active
    rig.name = name
    return rig


def box(name, lo=(-1, -1), hi=(1, 1)):
    layer = new_layer(name)
    assert layer.add_primitive("BOX", [lo, hi])
    return layer


def add_deform(layer, shape):
    layers.active = layer
    assert ops.object.modifier_add(type="CAGE_DEFORM") == {'FINISHED'}
    index = len(layer.modifiers) - 1
    layer.modifier_set(index, layer=shape.name, shape=shape.shapes[0][0])
    layer.modifier_set(index, cage_method="MEAN_VALUE", resample_self=False)
    return layer.modifiers[index]["name"]


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def add_bone(rig, name, head, tail):
    at(0)
    layers.active = rig
    assert ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]),
                                 tail_y=str(tail[1])) == {'FINISHED'}
    ops.armature.bone_parent_set(bone=name, parent="")


def follow(rig, bone, layer, deform):
    layers.active = rig
    assert ops.armature.constraint_add(bone=bone, type="FOLLOW_POINT") == {'FINISHED'}
    index = len(rig.bone_constraints(bone)) - 1
    rig.bone_constraint_set(bone, index, follow_deform=(layer.name, deform))
    return index


def constraint(rig, bone):
    return rig.bone_constraints(bone)[0]


def bend_box(cage, fn, frame=10):
    """Every point and handle of the cage put through fn, on that frame (moving a point alone keeps its handles' offsets,
    so its curves wouldn't turn with it)."""
    doc.frame_current = 0
    rest = [p[:2] for p in cage.points]
    handles = cage.curve_handles(0)
    doc.frame_current = frame
    for i, p in enumerate(rest):
        cage.set_point(i, fn(p))
    for k, (_, pre, post) in enumerate(handles):
        cage.place_handle(0, k, "IN", fn(pre), keep_other=True)
        cage.place_handle(0, k, "OUT", fn(post), keep_other=True)
    at(frame)


def tail_way(rig, bone):
    pose = rig.bone_pose(bone)
    return (pose[2] - pose[0], pose[3] - pose[1])


def rig_with(name, bones):
    """A cage, a drawing it bends and a rig of `bones` ({name: (head, tail)}) each following it."""
    cage = box(name + "Cage")
    art = box(name + "Art", (-0.5, -0.5), (0.5, 0.5))
    deform = add_deform(art, cage)
    rig = new_rig(name + "Rig")
    for bone, (head, tail) in bones.items():
        add_bone(rig, bone, head, tail)
        follow(rig, bone, art, deform)
    return cage, art, rig


quarter = lambda p: (-p[1], p[0])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1])


def apply(m, v):
    """m as (a, b, c, d), columns (a, b) and (c, d)."""
    return (m[0] * v[0] + m[2] * v[1], m[1] * v[0] + m[3] * v[1])


def rotated(v, angle):
    co, si = math.cos(angle), math.sin(angle)
    return (v[0] * co - v[1] * si, v[0] * si + v[1] * co)


def affine_of(rests, nows):
    """The linear part taking the rest points' edges to where the deform has them: the bend's own numbers, as the
    constraint samples them (the cage deform isn't itself affine)."""
    v, v2 = sub(rests[1], rests[0]), sub(rests[2], rests[0])
    w, w2 = sub(nows[1], nows[0]), sub(nows[2], nows[0])
    det = v[0] * v2[1] - v[1] * v2[0]
    inv = (v2[1] / det, -v[1] / det, -v2[0] / det, v[0] / det)
    return (w[0] * inv[0] + w2[0] * inv[1], w[1] * inv[0] + w2[1] * inv[1],
            w[0] * inv[2] + w2[0] * inv[3], w[1] * inv[2] + w2[1] * inv[3])


# --- One point -----------------------------------------------------------------------------------------------------
cage, art, rig = rig_with("One", {"still": ((0.25, 0.25), (0.45, 0.25)), "turns": ((0.25, 0.25), (0.45, 0.25))})
assert not constraint(rig, "still")["follow_rotation"], "new constraints start with Rotation off"
assert constraint(rig, "still")["follow_points"] == 1
rig.bone_constraint_set("turns", 0, follow_rotation=True)
bend_box(cage, quarter)
now = constraint(rig, "still")["follow_now"]
assert close(rig.bone_pose("still")[:2], now), (rig.bone_pose("still"), now)
assert close(tail_way(rig, "still"), (0.2, 0.0)), ("one point only moves it", tail_way(rig, "still"))
turn = constraint(rig, "turns")["follow_turn"]
assert abs(turn - math.pi / 2) < 0.1, ("turned about as the cage did", turn)
assert close(rig.bone_pose("turns")[:2], now), rig.bone_pose("turns")
assert close(tail_way(rig, "turns"), rotated((0.2, 0.0), turn)), (tail_way(rig, "turns"), turn)
rig.bone_constraint_set("turns", 0, influence=0.5)
at(10)
assert close(tail_way(rig, "turns"), rotated((0.2, 0.0), turn / 2)), "Influence blends the turn"

# Twisting on one side only: an Own Place point turns as the bend does where it is.
at(0)
cage, art, rig = rig_with("Twist", {"left": ((-0.4, -0.4), (-0.2, -0.4)), "right": ((0.4, 0.4), (0.6, 0.4))})
for bone in ("left", "right"):
    rig.bone_constraint_set(bone, 0, follow_rotation=True)
# One corner lifted: the lines near it turn more than those far from it.
bend_box(cage, lambda p: (p[0], p[1] + 0.8 * (p[0] > 0 and p[1] > 0)))
left, right = constraint(rig, "left")["follow_turn"], constraint(rig, "right")["follow_turn"]
assert left + 0.03 < right, (left, right)

# A point of the shape: the bone turns as the curve's direction does there.
at(0)
cage, art, rig = rig_with("Picked", {"corner": ((1.0, 1.0), (1.2, 1.0))})
corner = next(k for k in range(len(cage.points)) if close(cage.points[k][:2], (1.0, 1.0)))
rig.bone_constraint_set("corner", 0, follow_rotation=True, follow_shape_point=corner)
assert close(constraint(rig, "corner")["follow_point"], (1.0, 1.0)), constraint(rig, "corner")
bend_box(cage, quarter)
c = constraint(rig, "corner")
assert close(rig.bone_pose("corner")[:2], (-1.0, 1.0)), rig.bone_pose("corner")
assert abs(c["follow_turn"] - math.pi / 2) < 1e-4, c["follow_turn"]
assert close(tail_way(rig, "corner"), rotated((0.2, 0.0), c["follow_turn"])), tail_way(rig, "corner")
# Only the corner's handles turned, the point held: the curve's way through it turns by just that.
at(0)
cage, art, rig = rig_with("Handles", {"corner": ((1.0, 1.0), (1.2, 1.0))})
corner = next(k for k in range(len(cage.points)) if close(cage.points[k][:2], (1.0, 1.0)))
rig.bone_constraint_set("corner", 0, follow_rotation=True, follow_shape_point=corner)
_, pre, post = cage.curve_handles(0)[corner]
doc.frame_current = 10
twist = math.radians(30)
for side, handle in (("IN", pre), ("OUT", post)):
    way = rotated(sub(handle, (1.0, 1.0)), twist)
    cage.place_handle(0, corner, side, (1.0 + way[0], 1.0 + way[1]), keep_other=True)
at(10)
c = constraint(rig, "corner")
assert close(rig.bone_pose("corner")[:2], (1.0, 1.0)), rig.bone_pose("corner")
assert abs(c["follow_turn"] - twist) < 1e-4, (c["follow_turn"], twist)

# --- Two points, Scale Along Line ------------------------------------------------------------------------------------
at(0)
volumes = (0.0, 0.5, 1.0, 2.0)
cage, art, rig = rig_with("Line", {f"v{i}": ((0.0, 0.0), (0.0, 0.2)) for i in range(len(volumes))})
for i, volume in enumerate(volumes):
    rig.bone_constraint_set(f"v{i}", 0, follow_points=2, follow_point2=(0.3, 0.0), follow_rotation=True,
                            follow_scale=True, follow_scale_mode="ALONG_LINE", follow_volume=volume)
    assert constraint(rig, f"v{i}")["follow_scale_mode"] == "ALONG_LINE"
# Stretched along x: the bone, across the line, squashes by Volume.
bend_box(cage, lambda p: (p[0] * 1.5, p[1]))
for i, volume in enumerate(volumes):
    c = constraint(rig, f"v{i}")
    w = sub(c["follow_now2"], c["follow_now"])
    along = math.hypot(*w) / 0.3
    assert along > 1.3 and abs(c["follow_stretch"] - along) < 1e-3, (c["follow_stretch"], along)
    across = max((1.0 / along - 1.0) * volume + 1.0, 1e-4)
    d = (w[0] / math.hypot(*w), w[1] / math.hypot(*w))
    k = along - across
    shape = (across + k * d[0] * d[0], k * d[0] * d[1], k * d[0] * d[1], across + k * d[1] * d[1])
    assert close(c["follow_shape"], shape), (volume, c["follow_shape"], shape)
    # Turned, then shaped about the head.
    want = apply(shape, rotated((0.0, 0.2), c["follow_turn"]))
    assert close(tail_way(rig, f"v{i}"), want), (volume, tail_way(rig, f"v{i}"), want)
assert close(tail_way(rig, "v3"), (0.0, 0.2 * max((1.0 / along - 1.0) * 2.0 + 1.0, 1e-4)), 2e-2), tail_way(rig, "v3")
# Uniform, as before: the whole bone grows.
rig.bone_constraint_set("v1", 0, follow_scale_mode="UNIFORM")
at(10)
c = constraint(rig, "v1")
assert close(c["follow_shape"], (1.0, 0.0, 0.0, 1.0))
assert close(tail_way(rig, "v1"), rotated((0.0, 0.2 * c["follow_stretch"]), c["follow_turn"])), tail_way(rig, "v1")
# Along a slanted line: the stretch goes along that line, whatever way the bone points.
at(0)
cage, art, rig = rig_with("Slant", {"d": ((0.0, 0.0), (0.2, 0.0))})
rig.bone_constraint_set("d", 0, follow_points=2, follow_point2=(0.2, 0.2), follow_rotation=True, follow_scale=True,
                        follow_scale_mode="ALONG_LINE", follow_volume=1.0)
bend_box(cage, lambda p: ((p[0] + p[1]) * 0.9 + (p[0] - p[1]) * 0.3, (p[0] + p[1]) * 0.9 - (p[0] - p[1]) * 0.3))
c = constraint(rig, "d")
w = sub(c["follow_now2"], c["follow_now"])
along = math.hypot(*w) / math.hypot(0.2, 0.2)
d = (w[0] / math.hypot(*w), w[1] / math.hypot(*w))
way = rotated((0.2, 0.0), c["follow_turn"])
# Along the line it's stretched by `along`, across it squashed to 1 / along (Volume 1 keeps the area).
along_part = (way[0] * d[0] + way[1] * d[1]) * along
across_part = (-way[0] * d[1] + way[1] * d[0]) / along
want = (along_part * d[0] - across_part * d[1], along_part * d[1] + across_part * d[0])
assert along > 1.2 and close(tail_way(rig, "d"), want), (tail_way(rig, "d"), want, along)

# --- Three points ------------------------------------------------------------------------------------------------
at(0)
cage, art, rig = rig_with("Three", {"x": ((0.1, 0.1), (0.3, 0.1)), "y": ((0.1, 0.1), (0.1, 0.3)),
                                    "half": ((0.1, 0.1), (0.3, 0.1))})
rests = ((0.1, 0.1), (0.4, 0.1), (0.1, 0.4))
for bone in ("x", "y", "half"):
    rig.bone_constraint_set(bone, 0, follow_points=3, follow_point=rests[0], follow_point2=rests[1],
                            follow_point3=rests[2])
c = constraint(rig, "x")
assert c["follow_points"] == 3 and c["follow_second"] and close(c["follow_point3"], (0.1, 0.4)), c
rig.bone_constraint_set("half", 0, influence=0.5)
# Turned, sheared and scaled, and moved.
bend_box(cage, lambda p: (1.2 * p[0] - 0.4 * p[1] + 0.1, 0.3 * p[0] + 0.8 * p[1] - 0.05))
c = constraint(rig, "x")
nows = (c["follow_now"], c["follow_now2"], c["follow_now3"])
A = affine_of(rests, nows)
assert abs(A[1]) > 0.1 and abs(A[2]) > 0.1, ("it's skewed", A)
for bone, way in (("x", (0.2, 0.0)), ("y", (0.0, 0.2))):
    assert close(rig.bone_pose(bone)[:2], nows[0]), (bone, rig.bone_pose(bone), nows[0])
    assert close(tail_way(rig, bone), apply(A, way)), (bone, tail_way(rig, bone), apply(A, way))
turn = math.atan2(A[1] - A[2], A[0] + A[3])
assert abs(c["follow_turn"] - turn) < 1e-3, (c["follow_turn"], turn)
# The shape change is what's left after the turn.
co, si = math.cos(-turn), math.sin(-turn)
K = (A[0] * co + A[2] * si, A[1] * co + A[3] * si, -A[0] * si + A[2] * co, -A[1] * si + A[3] * co)
assert close(c["follow_shape"], K), (c["follow_shape"], K)
# Half Influence: half the turn, and halfway from no change of shape to the whole.
half_shape = tuple((k + i) / 2 for k, i in zip(K, (1.0, 0.0, 0.0, 1.0)))
want = apply(half_shape, rotated((0.2, 0.0), turn / 2))
assert close(tail_way(rig, "half"), want), (tail_way(rig, "half"), want)
rig.bone_constraint_set("half", 0, influence=0.0)
at(10)
assert close(rig.bone_pose("half")[:2], (0.1, 0.1)) and close(tail_way(rig, "half"), (0.2, 0.0)), rig.bone_pose("half")
# Rotation and Scale off change nothing with three points: they always count.
rig.bone_constraint_set("x", 0, follow_rotation=False, follow_scale=False)
at(10)
assert close(tail_way(rig, "x"), apply(A, (0.2, 0.0))), tail_way(rig, "x")
# On frame 0 nothing moves.
at(0)
assert close(tail_way(rig, "y"), (0.0, 0.2)) and close(constraint(rig, "y")["follow_shape"], (1.0, 0.0, 0.0, 1.0))

# Three points on a line give Harmony no change at all: the bone stays as its parent has it, not even moved.
skew = lambda p: (1.2 * p[0] - 0.4 * p[1] + 0.1, 0.3 * p[0] + 0.8 * p[1] - 0.05)
cage, art, rig = rig_with("Line3", {"b": ((0.0, 0.0), (0.2, 0.0)), "near": ((0.0, 0.0), (0.2, 0.0))})
rig.bone_constraint_set("b", 0, follow_points=3, follow_point=(0.0, 0.0), follow_point2=(0.2, 0.0),
                        follow_point3=(0.4, 0.0))
# Nearly on a line: the whole change, however big the skew it makes.
near = ((0.0, 0.0), (0.4, 0.0), (0.2, 0.004))
rig.bone_constraint_set("near", 0, follow_points=3, follow_point=near[0], follow_point2=near[1], follow_point3=near[2])
bend_box(cage, lambda p: (p[0] + 0.3 * p[1] * p[1], p[1] + 0.2 * p[0] * p[0] + 0.1))
c = constraint(rig, "b")
assert close(c["follow_shape"], (1.0, 0.0, 0.0, 1.0)) and c["follow_turn"] == 0.0, c
assert close(rig.bone_pose("b"), (0.0, 0.0, 0.2, 0.0)), rig.bone_pose("b")
c = constraint(rig, "near")
A = affine_of(near, (c["follow_now"], c["follow_now2"], c["follow_now3"]))
assert abs(A[1] + A[2]) > 0.5, ("skewed, not just turned", A)
assert close(tail_way(rig, "near"), apply(A, (0.2, 0.0)), 1e-3), (tail_way(rig, "near"), apply(A, (0.2, 0.0)))

line_rig = rig
# Fewer points again: the third is kept for later but doesn't count.
line_rig.bone_constraint_set("b", 0, follow_points=1)
c = constraint(line_rig, "b")
assert c["follow_points"] == 1 and not c["follow_second"], c
try:
    line_rig.bone_constraint_set("b", 0, follow_points=4)
except ValueError:
    pass
else:
    raise AssertionError("four points")

# --- Round the first point ------------------------------------------------------------------------------------------
# A head away from the first point swings round it, as a drawing under Harmony's output swings round its first pivot.
at(0)
cage, art, rig = rig_with("Pivot", {"two": ((0.3, -0.2), (0.5, -0.2)), "three": ((0.3, -0.2), (0.5, -0.2)),
                                    "line": ((0.3, -0.2), (0.5, -0.2))})
rig.bone_constraint_set("two", 0, follow_points=2, follow_point=(0.0, 0.0), follow_point2=(0.3, 0.0),
                        follow_rotation=True)
rig.bone_constraint_set("three", 0, follow_points=3, follow_point=(0.0, 0.0), follow_point2=(0.3, 0.0),
                        follow_point3=(0.0, 0.3))
rig.bone_constraint_set("line", 0, follow_points=2, follow_point=(0.0, 0.0), follow_point2=(0.3, 0.0),
                        follow_rotation=True, follow_scale=True, follow_scale_mode="ALONG_LINE", follow_volume=1.0)
bend_box(cage, skew)
head = (0.3, -0.2)
c = constraint(rig, "two")
want = rotated(head, c["follow_turn"])
want = (c["follow_now"][0] + want[0], c["follow_now"][1] + want[1])
assert close(rig.bone_pose("two")[:2], want), (rig.bone_pose("two"), want)
c = constraint(rig, "three")
A = affine_of(((0.0, 0.0), (0.3, 0.0), (0.0, 0.3)), (c["follow_now"], c["follow_now2"], c["follow_now3"]))
want = apply(A, head)
want = (c["follow_now"][0] + want[0], c["follow_now"][1] + want[1])
assert close(rig.bone_pose("three")[:2], want), (rig.bone_pose("three"), want)
assert close(tail_way(rig, "three"), apply(A, (0.2, 0.0))), tail_way(rig, "three")
c = constraint(rig, "line")
want = apply(c["follow_shape"], rotated(head, c["follow_turn"]))
want = (c["follow_now"][0] + want[0], c["follow_now"][1] + want[1])
assert close(rig.bone_pose("line")[:2], want), (rig.bone_pose("line"), want)

# --- Points close together ------------------------------------------------------------------------------------------
# Within Harmony's reach (1e-5 squared) of the first, the second is read 1e-4 further along x, as Harmony does.
at(0)
cage, art, rig = rig_with("Close", {"pair": ((0.0, 0.0), (0.2, 0.0)), "probe": ((0.0011, 0.0), (0.2, 0.0))})
rig.bone_constraint_set("pair", 0, follow_points=2, follow_point=(0.0, 0.0), follow_point2=(0.001, 0.0),
                        follow_rotation=True)
bend_box(cage, lambda p: (p[0] + 0.3 * p[1] * p[1], p[1] + 0.2 * p[0] * p[0] + 0.5 * p[0]))
c = constraint(rig, "pair")
assert close(c["follow_now2"], constraint(rig, "probe")["follow_now"], 1e-6), (c["follow_now2"],
                                                                           constraint(rig, "probe")["follow_now"])
w = sub(c["follow_now2"], c["follow_now"])
assert abs(c["follow_stretch"] - math.hypot(*w) / 0.0011) < 1e-2, (c["follow_stretch"], math.hypot(*w) / 0.0011)

# --- Saved with the file -------------------------------------------------------------------------------------------
at(0)
cage, art, rig = rig_with("Old", {"one": ((0.2, 0.2), (0.4, 0.2))})
rig.bone_constraint_set("one", 0, follow_rotation=True)
folder = tempfile.mkdtemp(prefix="oanim_follow_modes_")
try:
    path = os.path.join(folder, "modes.oanim")
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
    find = lambda name: next(l for l in layers.all() if l.name == name)
    c = constraint(find("ThreeRig"), "x")
    assert c["follow_points"] == 3 and close(c["follow_point3"], (0.1, 0.4)), c
    c = constraint(find("LineRig"), "v3")
    assert c["follow_scale_mode"] == "ALONG_LINE" and abs(c["follow_volume"] - 2.0) < 1e-6, c
    assert constraint(find("OldRig"), "one")["follow_rotation"], "a one-point Rotation made now is kept"
    at(10)
    assert close(tail_way(find("ThreeRig"), "x"), apply(A, (0.2, 0.0))), tail_way(find("ThreeRig"), "x")

    # A file from before: its one-point constraints never turned, whatever Rotation said.
    old = os.path.join(folder, "old.oanim")
    with zipfile.ZipFile(path) as src, zipfile.ZipFile(old, "w", zipfile.ZIP_DEFLATED) as dst:
        for item in src.infolist():
            data = src.read(item.filename)
            if item.filename.endswith(".json"):
                text = json.loads(data)

                def strip(node):
                    if isinstance(node, dict):
                        for key in ("follow_scale_mode", "follow_volume", "follow_three", "follow_point3",
                                    "follow_rest3", "follow_shift3", "follow_point3_id", "follow_shift3_keys"):
                            node.pop(key, None)
                        for value in node.values():
                            strip(value)
                    elif isinstance(node, list):
                        for value in node:
                            strip(value)

                strip(text)
                data = json.dumps(text).encode()
            dst.writestr(item, data)
    assert ops.wm.open_mainfile(filepath=old, confirmed="1") == {'FINISHED'}
    c = constraint(find("OldRig"), "one")
    assert not c["follow_rotation"] and c["follow_points"] == 1, c
    c = constraint(find("LineRig"), "v3")
    assert c["follow_rotation"] and c["follow_scale_mode"] == "UNIFORM" and c["follow_points"] == 2, c
finally:
    shutil.rmtree(folder, ignore_errors=True)

print("follow point modes: ok")
