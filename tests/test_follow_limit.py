"""Follow Deform's Limit Method: None moves the whole drawing; Shape moves one of its shapes and Point Group the points
of a point group, each by its weight; Invert all but those. Whatever isn't taken in stays as the stack left it. Its point
starts at the middle of what it moves, unless it was put somewhere. A shape or point group that isn't there moves
nothing. Cut points take their share from their segment's ends. It goes with the layer into copies, Symmetrize and the file.

Run with:  OpenAnimate --background --python tests/test_follow_limit.py
"""
import math
import os
import tempfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0

MOVE = (0.3, 0.2)


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


def box(layer, lo, hi):
    """Four points, closed and filled: a shape of its own, named as the layer's next."""
    layer.add_curve([(lo, lo), (hi, lo), (hi, hi), (lo, hi)], closed=True, fill=True)


def face(name):
    """Its first four points the nose (shape 0), its next four the mouth (shape 1)."""
    layer = new_layer(name)
    box(layer, 0.1, 0.3)
    box(layer, -0.5, -0.3)
    return layer


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def follow(layer, shape, **settings):
    layers.active = layer
    assert ops.object.modifier_add(type="FOLLOW_DEFORM") == {'FINISHED'}
    index = len(layer.modifiers) - 1
    layer.modifier_set(index, layer=shape.name, shape=shape.shapes[0][0])
    layer.modifier_set(index, **settings)
    return index


def moved(layer, rest):
    """How far each point has gone from `rest`."""
    return [(p[0] - q[0], p[1] - q[1]) for p, q in zip(layer.evaluated()[0], rest)]


def close(a, b, eps=1e-4):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def drawn(layer):
    doc.frame_current = 0
    return [tuple(p[:2]) for p in layer.evaluated()[0]]


cage = new_layer("Cage")
cage.add_curve([(-1, -1), (1, -1), (1, 1), (-1, 1)], closed=True, fill=True)
rest_cage = [tuple(p[:2]) for p in cage.points]


def carry_cage(frame=10):
    """Moves the whole cage, so whatever follows it goes by MOVE."""
    doc.frame_current = frame
    for i, p in enumerate(rest_cage):
        cage.set_point(i, (p[0] + MOVE[0], p[1] + MOVE[1]))
    at(frame)


# --- None, as it always was ------------------------------------------------------------------------------------------
art = face("Art")
n = follow(art, cage)
assert art.modifiers[n]["limit"] == "NONE" and art.modifiers[n]["limit_invert"] is False
assert close(art.modifiers[n]["point"], (-0.1, -0.1)), art.modifiers[n]["point"]
rest = drawn(art)
carry_cage()
assert all(close(d, MOVE) for d in moved(art, rest)), "the whole drawing"

# --- Shape -----------------------------------------------------------------------------------------------------------
nose_name, mouth_name = art.shapes[0][0], art.shapes[1][0]
art.modifier_set(n, limit="SHAPE", limit_shape=nose_name)
at(10)
d = moved(art, rest)
assert all(close(q, MOVE) for q in d[:4]), ("the nose follows", d)
assert all(close(q, (0.0, 0.0), 1e-6) for q in d[4:]), ("the mouth stays exactly", d)

# Its point went to the middle of what it moves.
assert close(art.modifiers[n]["point"], (0.2, 0.2)), art.modifiers[n]["point"]

# Invert: all but the nose.
art.modifier_set(n, limit_invert=True)
at(10)
d = moved(art, rest)
assert all(close(q, (0.0, 0.0), 1e-6) for q in d[:4]) and all(close(q, MOVE) for q in d[4:]), d
assert close(art.modifiers[n]["point"], (-0.4, -0.4)), art.modifiers[n]["point"]
art.modifier_set(n, limit_invert=False)

# A point put somewhere stays there.
art.modifier_set(n, point=(0.25, 0.25))
art.modifier_set(n, limit_shape=mouth_name)
assert close(art.modifiers[n]["point"], (0.25, 0.25)), art.modifiers[n]["point"]
# Through a limit with nothing picked yet, too.
art.modifier_set(n, limit="GROUP", limit_group="")
art.modifier_set(n, limit_group="Nobody")
assert close(art.modifiers[n]["point"], (0.25, 0.25)), art.modifiers[n]["point"]
art.modifier_set(n, limit="SHAPE", limit_shape=nose_name, point=(0.2, 0.2))

# Frame 0: as drawn.
assert drawn(art) == rest

# --- Point Group: by weight ------------------------------------------------------------------------------------------
layers.active = art
ops.mesh.select_none()
art.select_points([0, 1])
ops.mesh.point_group_add(name="Soft")
assert ops.mesh.point_group_assign(weight="1") == {'FINISHED'}
ops.mesh.select_none()
art.select_points([2])
assert ops.mesh.point_group_assign(weight="0.5") == {'FINISHED'}
ops.mesh.select_none()
art.modifier_set(n, point=(0.25, 0.25), limit="GROUP", limit_group="")
art.modifier_set(n, limit_group="Soft")
assert close(art.modifiers[n]["point"], (0.25, 0.25)), ("put somewhere: stays", art.modifiers[n]["point"])
at(10)
d = moved(art, rest)
assert close(d[0], MOVE) and close(d[1], MOVE), d
assert close(d[2], (MOVE[0] * 0.5, MOVE[1] * 0.5)), d
assert all(close(q, (0.0, 0.0), 1e-6) for q in d[3:]), ("not in the group: stays", d)

# Invert by weight.
art.modifier_set(n, limit_invert=True)
at(10)
d = moved(art, rest)
assert close(d[0], (0.0, 0.0), 1e-6) and close(d[2], (MOVE[0] * 0.5, MOVE[1] * 0.5)) and close(d[3], MOVE), d
art.modifier_set(n, limit_invert=False)

# --- Nothing there, nothing moves ------------------------------------------------------------------------------------
art.modifier_set(n, limit_group="Nobody")
at(10)
assert all(close(q, (0.0, 0.0), 1e-6) for q in moved(art, rest)), "no such group"
art.modifier_set(n, limit_invert=True)
at(10)
assert all(close(q, (0.0, 0.0), 1e-6) for q in moved(art, rest)), "not even inverted"
art.modifier_set(n, limit_invert=False, limit="SHAPE", limit_shape="")
at(10)
assert all(close(q, (0.0, 0.0), 1e-6) for q in moved(art, rest)), "no shape picked"
art.modifier_set(n, limit_shape=nose_name)
try:
    art.modifier_set(n, limit_shape="No such shape")
    raise AssertionError("a name that isn't there")
except ValueError:
    pass
try:
    art.modifier_set(n, limit="SIDEWAYS")
    raise AssertionError("not a Limit Method")
except ValueError:
    pass

# --- Two on one layer, each its own part -----------------------------------------------------------------------------
doc.frame_current = 0
for k in range(len(art.modifiers) - 1, -1, -1):
    if art.modifiers[k]["type"] == "FOLLOW_DEFORM":
        layers.active = art
        assert ops.object.modifier_remove(index=str(k)) == {'FINISHED'}
other = new_layer("Other")
other.add_curve([(-2, -2), (2, -2), (2, 2), (-2, 2)], closed=True, fill=True)
rest_other = [tuple(p[:2]) for p in other.points]
a = follow(art, cage, limit="SHAPE", limit_shape=nose_name)
b = follow(art, other, limit="SHAPE", limit_shape=mouth_name)
doc.frame_current = 10
for i, p in enumerate(rest_other):
    other.set_point(i, (p[0] - 0.1, p[1] + 0.4))
at(10)
d = moved(art, rest)
assert all(close(q, MOVE) for q in d[:4]) and all(close(q, (-0.1, 0.4)) for q in d[4:]), d
doc.frame_current = 0
layers.active = art
while any(m["type"] == "FOLLOW_DEFORM" for m in art.modifiers):
    assert ops.object.modifier_remove(index=str(len(art.modifiers) - 1)) == {'FINISHED'}

# --- A turn: the limited part's handles turn with it, the rest's stay ----------------------------------------------------
doc.frame_current = 0
for k in range(4):
    p = art.points[k]
    art.move_handle(0, k, "OUT", (p[0] + 0.05, p[1] + 0.02))
at(0)
n = follow(art, cage, limit="SHAPE", limit_shape=nose_name, second=True, point2=(0.2, 0.5))
assert close(art.modifiers[n]["point"], (0.2, 0.2)), art.modifiers[n]
was_points = [tuple(p[:2]) for p in art.evaluated()[0]]
was = art.evaluated_handles(0)
mouth_was = art.evaluated_handles(1)
doc.frame_current = 10
for i, p in enumerate(rest_cage):
    cage.set_point(i, (-p[1], p[0]))
at(10)
now_points = [tuple(p[:2]) for p in art.evaluated()[0]]
now = art.evaluated_handles(0)
for g, h in zip(art.evaluated_handles(1), mouth_was):
    assert close(g[1], h[1], 1e-5) and close(g[2], h[2], 1e-5), ("the mouth's handles stay", g, h)
for k in range(4):
    out_was = (was[k][2][0] - was_points[k][0], was[k][2][1] - was_points[k][1])
    out_now = (now[k][2][0] - now_points[k][0], now[k][2][1] - now_points[k][1])
    assert close(out_now, (-out_was[1], out_was[0]), 2e-2), ("the nose's handles turn with it", k, out_was, out_now)
doc.frame_current = 0
for i, p in enumerate(rest_cage):
    cage.set_point(i, p)
layers.active = art
assert ops.object.modifier_remove(index=str(n)) == {'FINISHED'}

# --- What the stack cut into a shape goes with it ------------------------------------------------------------------------
cut = face("Cut")
layers.active = cut
assert ops.object.modifier_add(type="RESAMPLE") == {'FINISHED'}
cut.modifier_set(0, split="COUNT", pieces=4)
n = follow(cut, cage, limit="SHAPE", limit_shape=cut.shapes[0][0])
rest_cut = drawn(cut)
assert len(rest_cut) == 32
carry_cage()
d = moved(cut, rest_cut)
assert all(close(q, MOVE) for q in d[:4]) and all(close(q, (0.0, 0.0), 1e-6) for q in d[4:8]), d
nose_cuts = d[8:20]
mouth_cuts = d[20:32]
assert all(close(q, MOVE) for q in nose_cuts), ("the nose's cuts follow", nose_cuts)
assert all(close(q, (0.0, 0.0), 1e-6) for q in mouth_cuts), ("the mouth's cuts stay", mouth_cuts)

# A limit taking in all of a drawing is no limit, through bones too: those moving the shape as well count once.
doc.frame_current = 0
for i, p in enumerate(rest_cage):
    cage.set_point(i, p)
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
rig.name = "Rig"
assert ops.armature.bone_add(name="head", head_x="0", head_y="0", tail_x="0", tail_y="1") == {'FINISHED'}
ops.armature.bone_parent_set(bone="head", parent="")
head_cage = new_layer("HeadCage")
head_cage.add_curve([(-1, -1), (1, -1), (1, 1), (-1, 1)], closed=True, fill=True)
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
assert ops.layer.parent_bone_set(bone="head") == {'FINISHED'}
rings = []
for limited in (False, True):
    ring = new_layer("Ring%d" % len(rings))
    box(ring, 0.3, 0.5)
    assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
    assert ops.layer.parent_bone_set(bone="head") == {'FINISHED'}
    ring = next(l for l in layers.all() if l.name == "Ring%d" % len(rings))
    head_cage = next(l for l in layers.all() if l.name == "HeadCage")
    follow(ring, head_cage, **({"limit": "SHAPE", "limit_shape": ring.shapes[0][0]} if limited else {}))
    rings.append(ring)
rig = next(l for l in layers.all() if l.name == "Rig")
doc.frame_current = 10
rig.bone_set("head", angle=rig.bones[0]["setup_angle"] + math.pi / 2)
at(10)
whole, limited = [[r.to_document(p) for p in r.evaluated()[0]] for r in rings]
assert all(close(p, q, 1e-5) for p, q in zip(whole, limited)), (whole, limited)
assert close(whole[0], (-0.3, 0.3), 2e-2), ("a quarter turn once", whole[0])
doc.frame_current = 0

# What only the rest of the drawing's bones do has no say in where the limited part reads the shape.
assert ops.layer.new_bone() == {'FINISHED'}
rig2 = layers.active
rig2.name = "Rig2"
assert ops.armature.bone_add(name="near", head_x="0", head_y="0", tail_x="0", tail_y="1") == {'FINISHED'}
ops.armature.bone_parent_set(bone="near", parent="")
assert ops.armature.bone_add(name="far", head_x="2", head_y="0", tail_x="2", tail_y="1") == {'FINISHED'}
ops.armature.bone_parent_set(bone="far", parent="")
bent = new_layer("BentCage")
bent.add_curve([(-1, -1), (1, -1), (1, 1), (-1, 1)], closed=True, fill=True)
assert ops.layer.move_to_group(group=str(rig2.id)) == {'FINISHED'}
assert ops.layer.parent_bone_set(bone="near") == {'FINISHED'}
parts = {}
for name, boxes in (("Alone", ((0.1, 0.3),)), ("Parts", ((0.1, 0.3), (0.4, 0.6)))):
    part = new_layer(name)
    for lo, hi in boxes:
        box(part, lo, hi)
    assert ops.layer.move_to_group(group=str(rig2.id)) == {'FINISHED'}
    part = next(l for l in layers.all() if l.name == name)
    for points, bone in (([0, 1, 2, 3], "near"), ([4, 5, 6, 7], "far")):
        if points[0] < part.point_count:
            layers.active = part
            ops.mesh.select_none()
            part.select_points(points)
            rig2.select_bone(bone)
            layers.active = part
            assert ops.weight.assign(weight="1") == {'FINISHED'}
    ops.mesh.select_none()
    parts[name] = part
    follow(part, bent, cage_method="MEAN_VALUE", limit="SHAPE", limit_shape=part.shapes[0][0])
doc.frame_current = 10
rig2.bone_set("near", position=(rig2.bones[0]["position"][0] + 0.1, rig2.bones[0]["position"][1]))
rig2.bone_set("far", position=(rig2.bones[1]["position"][0] + 0.7, rig2.bones[1]["position"][1]))
bent = next(l for l in layers.all() if l.name == "BentCage")
at(10)
for i, p in enumerate(bent.points):
    bent.set_point(i, (p[0] * (1.0 + 0.3 * p[1]), p[1] + 0.2 * p[0] * p[0]))
at(10)
alone, both = parts["Alone"].evaluated()[0], parts["Parts"].evaluated()[0]
assert all(close(p, q, 1e-4) for p, q in zip(alone[:4], both[:4])), (alone[:4], both[:4])
doc.frame_current = 0

# --- It comes along --------------------------------------------------------------------------------------------------
carry_cage()
doc.frame_current = 0
kept = new_layer("Kept_L")
box(kept, 0.1, 0.3)
box(kept, 0.4, 0.6)
n = follow(kept, cage, limit="SHAPE", limit_shape=kept.shapes[0][0])
rest = drawn(kept)
at(10)
d = moved(kept, rest)
assert all(close(q, MOVE) for q in d[:4]) and all(close(q, (0.0, 0.0), 1e-6) for q in d[4:]), d

# A copy.
doc.frame_current = 0
layers.active = kept
assert ops.layer.duplicate() == {'FINISHED'}
copy = layers.active
assert copy.id != kept.id and copy.modifiers[n]["limit_shape"] == kept.modifiers[n]["limit_shape"], copy.modifiers[n]
at(10)
d = moved(copy, rest)
assert all(close(q, MOVE) for q in d[:4]) and all(close(q, (0.0, 0.0), 1e-6) for q in d[4:]), ("a copy", d)
doc.frame_current = 0
layers.active = copy
assert ops.layer.delete() == {'FINISHED'}

# Symmetrize: the copy's own shape, across.
kept = next(l for l in layers.all() if l.name == "Kept_L")
layers.active = kept
assert ops.layer.symmetrize() == {'FINISHED'}
right = next(l for l in layers.all() if l.name == "Kept_R")
m = right.modifiers[n]
assert m["limit"] == "SHAPE" and m["limit_shape"] == kept.modifiers[n]["limit_shape"], m
assert close(m["point"], (-0.2, 0.2)), m["point"]
rest_right = drawn(right)
at(10)
d = moved(right, rest_right)
assert all(close(q, MOVE) for q in d[:4]) and all(close(q, (0.0, 0.0), 1e-6) for q in d[4:]), ("symmetrized", d)

# The file.
doc.frame_current = 0
kept.modifier_set(n, limit="GROUP", limit_group="Soft", limit_invert=True)
folder = tempfile.mkdtemp()
path = os.path.join(folder, "follow_limit.oanim")
try:
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
    kept = next(l for l in layers.all() if l.name == "Kept_L")
    m = kept.modifiers[n]
    assert m["limit"] == "GROUP" and m["limit_group"] == "Soft" and m["limit_invert"] is True, m
    right = next(l for l in layers.all() if l.name == "Kept_R")
    assert right.modifiers[n]["limit"] == "SHAPE" and right.modifiers[n]["limit_shape"], right.modifiers[n]
finally:
    if os.path.exists(path):
        os.remove(path)
    os.rmdir(folder)

print("follow limit: ok")
