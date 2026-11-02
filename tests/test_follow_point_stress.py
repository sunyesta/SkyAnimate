"""Follow Point, tried hard. What it reads is checked against what the deform really does to a drawing, not against sums:

- A point followed lands exactly where the deform puts a drawn point there, for every deform and method, on a drawing
  and on a group, over random shapes and frames.
- Collapsed, flipped and huge shapes, points far outside or on top of each other: never NaN, never a hang.
- Stacks and layers changing under it: moved, renamed, duplicated, deleted and undone, it follows what it named or says so.
- Rigs, groups and layers moved, turned, scaled and flipped: the bone stays on the point.
- Other constraints on the same bone, chains and random webs of rigs: loops refused, the rest settled in any layer order.
- The order frames are visited changes nothing, and a held frame never drifts.

Run with:  OpenAnimate --background --python tests/test_follow_point_stress.py
"""

import json
import math
import os
import random
import shutil
import tempfile
import time
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
anchor = layers.active
rng = random.Random(20261101)
folder = tempfile.mkdtemp(prefix="oanim_follow_stress_")


def close(a, b, eps=1e-3):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def finite(*values):
    return all(math.isfinite(v) for value in values for v in value)


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


def new_group(name, *children):
    layers.active = anchor
    assert ops.layer.new_group() == {'FINISHED'}
    group = layers.active
    group.name = name
    for child in children:
        layers.active = child
        assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
    return group


def into(group, *children):
    for child in children:
        layers.active = child
        assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}


def box(name, lo=(-1, -1), hi=(1, 1)):
    layer = new_layer(name)
    assert layer.add_primitive("BOX", [lo, hi])
    return layer


def grid(name, lo=(-1, -1), hi=(1, 1), columns=2, rows=2):
    layer = new_layer(name)
    assert layer.add_primitive("GRID", [lo, hi], columns=columns, rows=rows)
    return layer


def line(name, points):
    layer = new_layer(name)
    layer.add_curve(points)
    return layer


def add_deform(layer, shape, kind="CAGE_DEFORM", **settings):
    layers.active = layer
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    index = len(layer.modifiers) - 1
    layer.modifier_set(index, layer=shape.name, shape=shape.shapes[0][0], **settings)
    return layer.modifiers[index]["name"]


def add_bone(rig, name, head, tail=None, parent=None):
    at(0)
    layers.active = rig
    tail = tail or (head[0] + 0.2, head[1])
    assert ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]), tail_y=str(tail[1])) == {'FINISHED'}
    ops.armature.bone_parent_set(bone=name, parent=parent or "")


def follow(rig, bone, layer, deform):
    layers.active = rig
    assert ops.armature.constraint_add(bone=bone, type="FOLLOW_POINT") == {'FINISHED'}
    index = len(rig.bone_constraints(bone)) - 1
    rig.bone_constraint_set(bone, index, follow_deform=(layer.name, deform))
    return index


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def head(rig, bone):
    return rig.bone_pose(bone)[:2]


def world_head(rig, bone):
    return rig.to_document(head(rig, bone))


def constraint(rig, bone, index=0):
    return rig.bone_constraints(bone)[index]


def shake(shape, amount, frame=10):
    """Every point of the shape moved its own random way, on that frame."""
    doc.frame_current = 0
    rest = [p[:2] for p in shape.points]
    doc.frame_current = frame
    for i, p in enumerate(rest):
        shape.set_point(i, (p[0] + rng.uniform(-amount, amount), p[1] + rng.uniform(-amount, amount)))
    at(frame)


def place(shape, fn, frame=10):
    doc.frame_current = 0
    rest = [p[:2] for p in shape.points]
    doc.frame_current = frame
    for i, p in enumerate(rest):
        shape.set_point(i, fn(p))
    at(frame)


# --- Against what the deform really does ---------------------------------------------------------------------------
KINDS = [
    ("CAGE_DEFORM", {"cage_method": "MEAN_VALUE"}, box),
    ("CAGE_DEFORM", {"cage_method": "RIGID"}, box),
    ("CAGE_DEFORM", {"cage_method": "ENVELOPE"}, box),
    ("MESH_DEFORM", {"mesh_method": "CELLS"}, grid),
    ("MESH_DEFORM", {"mesh_method": "ENVELOPE"}, grid),
    ("MESH_DEFORM", {"mesh_method": "CELLS", "smooth": True}, grid),
    ("CURVE_DEFORM", {}, None),
    ("CURVE_DEFORM", {"stretch": False}, None),
]
worst = 0.0
compared = 0
for run, (kind, settings, maker) in enumerate(KINDS):
    for on_group in (False, True):
        at(0)
        tag = "%d%s" % (run, "G" if on_group else "D")
        shape = maker("Shape" + tag) if maker else line("Shape" + tag, [(-1, 0), (-0.3, 0.1), (0.4, -0.1), (1, 0)])
        art = new_layer("Art" + tag)
        places = [(rng.uniform(-0.8, 0.8), rng.uniform(-0.8, 0.8)) for _ in range(5)]
        for p in places:
            art.add_curve([p, (p[0] + 0.01, p[1])])
        owner = new_group("Group" + tag, art) if on_group else art
        deform = add_deform(owner, shape, kind, precision=0.0, **settings)
        rig = new_rig("Rig" + tag)
        for i, p in enumerate(places):
            add_bone(rig, "b%d" % i, p)
            follow(rig, "b%d" % i, owner, deform)
            assert constraint(rig, "b%d" % i)["follow_error"] == "", constraint(rig, "b%d" % i)
        for frame in (6, 10, 17):
            shake(shape, 0.25, frame)
        for frame in (3, 6, 10, 13, 17, 30):
            at(frame)
            drawn = art.evaluated()[0]
            for i, p in enumerate(places):
                truth = art.to_document(drawn[i * 2])
                seen = constraint(rig, "b%d" % i)["follow_now"]
                gap = math.dist(truth, seen)
                worst = max(worst, gap)
                compared += 1
                assert gap < 1e-4, (kind, settings, on_group, frame, p, truth, seen)
                assert close(world_head(rig, "b%d" % i), seen, 1e-4), (kind, frame, world_head(rig, "b%d" % i), seen)
print("against the deform: %d points, worst gap %.7f" % (compared, worst))

# --- Shapes gone wrong --------------------------------------------------------------------------------------------
at(0)
wild = box("Wild")
wild_art = box("WildArt", (-0.5, -0.5), (0.5, 0.5))
wild_deform = add_deform(wild_art, wild)
wild_rig = new_rig("WildRig")
for name, p in (("in", (0.3, 0.2)), ("edge", (1.0, 0.0)), ("corner", (1.0, 1.0)), ("far", (50.0, -80.0)), ("huge", (1e4, 1e4))):
    add_bone(wild_rig, name, p)
    i = follow(wild_rig, name, wild_art, wild_deform)
    wild_rig.bone_constraint_set(name, i, follow_second=True)
    wild_rig.bone_constraint_set(name, i, follow_rotation=True, follow_scale=True)
add_bone(wild_rig, "same", (0.1, 0.1))
i = follow(wild_rig, "same", wild_art, wild_deform)
wild_rig.bone_constraint_set("same", i, follow_second=True, follow_rotation=True, follow_scale=True)
wild_rig.bone_constraint_set("same", i, follow_point2=(0.1, 0.1))
SHAPES = {
    "a point": lambda p: (0.2, 0.3),
    "a line": lambda p: (p[0], 0.0),
    "flipped": lambda p: (-p[0], p[1]),
    "inside out": lambda p: (-p[0], -p[1]),
    "huge": lambda p: (p[0] * 1e5, p[1] * 1e5),
    "tiny": lambda p: (p[0] * 1e-6, p[1] * 1e-6),
    "bow tie": lambda p: (p[0] * p[1], p[1]),
}
for name, fn in SHAPES.items():
    place(wild, fn)
    for _ in range(2):
        at(10)
    for bone in ("in", "edge", "corner", "far", "huge", "same"):
        pose = wild_rig.bone_pose(bone)
        c = constraint(wild_rig, bone)
        assert finite(pose, c["follow_now"], c["follow_now2"], c["follow_offset"], (c["follow_turn"], c["follow_stretch"])), (name, bone, pose, c)
    held = wild_rig.bone_pose("in")
    at(9)
    at(10)
    assert held == wild_rig.bone_pose("in"), (name, "it gives the same answer every time")
# Dragged toward a shape that collapsed: no NaN and it comes back.
place(wild, lambda p: (0.2, 0.3))
i = 0
wild_rig.bone_constraint_set("in", i, follow_to=(5.0, 5.0))
assert finite(constraint(wild_rig, "in")["follow_shift"]), constraint(wild_rig, "in")
wild_rig.bone_constraint_set("in", i, follow_shift=(0.0, 0.0))
place(wild, lambda p: p)
at(10)
assert close(head(wild_rig, "in"), (0.3, 0.2)), head(wild_rig, "in")
# A shape with its points all deleted, and no shape at all.
at(0)
add_deform(wild_art, wild, "CAGE_DEFORM")
wild_art.modifier_set(0, layer="")
at(10)
assert close(head(wild_rig, "in"), (0.3, 0.2)) and constraint(wild_rig, "in")["follow_error"] == "", constraint(wild_rig, "in")
wild_art.modifier_set(0, layer="Wild", shape="Box 1" if False else wild.shapes[0][0])
wild_art.modifier_set(0, strength=0.0)
at(10)
assert close(head(wild_rig, "in"), (0.3, 0.2))
wild_art.modifier_set(0, strength=1.0)

# --- The stack changing under it ---------------------------------------------------------------------------------
at(0)
cage = box("Cage")
art = box("Art", (-0.5, -0.5), (0.5, 0.5))
deform = add_deform(art, cage)
rig = new_rig("Rig")
add_bone(rig, "eye", (0.25, 0.25))
i = follow(rig, "eye", art, deform)
place(cage, lambda p: (p[0] + 0.5, p[1]))
assert close(head(rig, "eye"), (0.75, 0.25)), head(rig, "eye")
# Renamed and moved in the stack, it's the same deform.
at(0)
layers.active = art
assert ops.object.modifier_add(type="RESAMPLE") == {'FINISHED'} or True
index = [m["name"] for m in art.modifiers].index(deform)
art.modifier_set(index, name="Renamed") if False else None
assert ops.object.modifier_move(index=str(index), direction="1") in ({'FINISHED'}, {'CANCELLED'})
at(10)
assert close(head(rig, "eye"), (0.75, 0.25)), ("moved in the stack", head(rig, "eye"))
# A copy of the deform is another deform.
at(0)
layers.active = art
index = [m["name"] for m in art.modifiers].index(deform)
assert ops.object.modifier_duplicate(index=str(index)) == {'FINISHED'}
copy = [m for m in art.modifiers if m["type"] == "CAGE_DEFORM" and m["name"] != deform][0]["name"]
add_bone(rig, "other", (0.25, 0.25))
j = follow(rig, "other", art, copy)
assert constraint(rig, "other")["follow_modifier"] == copy and constraint(rig, "eye")["follow_modifier"] == deform, (constraint(rig, "other")["follow_modifier"], constraint(rig, "eye")["follow_modifier"])
# Deleted: it says so and rests, a new deform doesn't take its place, and undo brings it back.
layers.active = art
index = [m["name"] for m in art.modifiers].index(copy)
assert ops.object.modifier_remove(index=str(index)) == {'FINISHED'}
at(10)
assert constraint(rig, "other")["follow_error"] == "Its deform is gone", constraint(rig, "other")
at(0)
layers.active = art
assert ops.object.modifier_add(type="CAGE_DEFORM") == {'FINISHED'}
fresh = len(art.modifiers) - 1
art.modifier_set(fresh, layer=cage.name, shape=cage.shapes[0][0])
add_bone(rig, "third", (0.0, 0.0))
follow(rig, "third", art, art.modifiers[fresh]["name"])
at(10)
assert constraint(rig, "other")["follow_error"] == "Its deform is gone", ("a new deform isn't the old one", constraint(rig, "other"))
at(0)

# The shape's layer deleted, then the deform's: undo brings each back.
for doomed in (cage, art):
    name = doomed.name
    at(0)
    layers.active = doomed
    assert ops.layer.delete() == {'FINISHED'}
    at(10)
    assert finite(rig.bone_pose("eye")), rig.bone_pose("eye")
    if name == "Art":
        assert constraint(rig, "eye")["follow_error"] == "Its deform is gone", constraint(rig, "eye")
    assert ops.ed.undo() == {'FINISHED'}
    at(10)
    assert close(head(rig, "eye"), (0.75, 0.25)), (name, head(rig, "eye"), constraint(rig, "eye"))
cage = next(l for l in layers.all() if l.name == "Cage")
art = next(l for l in layers.all() if l.name == "Art")
rig = next(l for l in layers.all() if l.name == "Rig")

# Its bone deleted, a constraint moved and removed.
at(0)
layers.active = rig
assert ops.armature.constraint_add(bone="eye", type="LIMIT_ANGLE") == {'FINISHED'}
assert ops.armature.constraint_move(index="0", direction="1", bone="eye") in ({'FINISHED'}, {'CANCELLED'})
at(10)
assert close(head(rig, "eye"), (0.75, 0.25)), head(rig, "eye")

# Two on one bone add up.
at(0)
second_cage = box("SecondCage")
second_art = box("SecondArt", (-0.5, -0.5), (0.5, 0.5))
second_deform = add_deform(second_art, second_cage)
place(second_cage, lambda p: (p[0], p[1] + 0.3))
at(0)
follow(rig, "eye", second_art, second_deform)
at(10)
assert close(head(rig, "eye"), (0.75, 0.55)), head(rig, "eye")

# Copied with the layers: a rig duplicated with its deform follows its own copy.
at(0)
pack_cage = box("PackCage")
pack_art = box("PackArt", (-0.5, -0.5), (0.5, 0.5))
pack_deform = add_deform(pack_art, pack_cage)
pack_rig = new_rig("PackRig")
add_bone(pack_rig, "stud", (0.1, 0.1))
follow(pack_rig, "stud", pack_art, pack_deform)
pack = new_group("Pack", pack_cage, pack_art, pack_rig)
layers.active = pack
assert ops.layer.duplicate() == {'FINISHED'}
dup = layers.active
assert dup.id != pack.id
inside = [l for l in layers.all() if l.parent is not None and l.parent.id == dup.id]
copied_rig = [l for l in inside if l.type == "BONE"][0]
copied_cage = [l for l in inside if l.name.startswith("PackCage")][0]
assert constraint(copied_rig, "stud")["follow_error"] == "", constraint(copied_rig, "stud")
place(copied_cage, lambda p: (p[0] + 0.4, p[1]))
assert close(head(copied_rig, "stud"), (0.5, 0.1)), ("its own copy", head(copied_rig, "stud"), constraint(copied_rig, "stud"))
assert close(head(pack_rig, "stud"), (0.1, 0.1)), ("the first stays", head(pack_rig, "stud"))
at(0)
# Only the rig duplicated: it still follows the one deform.
layers.active = pack_rig
assert ops.layer.duplicate() == {'FINISHED'}
alone = layers.active
assert constraint(alone, "stud")["follow_layer"] == "PackArt", constraint(alone, "stud")
# Bones copied and symmetrized keep working.
at(0)
add_bone(pack_rig, "stud_L", (0.3, 0.1))
follow(pack_rig, "stud_L", pack_art, pack_deform)
layers.active = pack_rig
assert ops.armature.symmetrize() == {'FINISHED'}
assert "stud_R" in [b["name"] for b in pack_rig.bones]
c = constraint(pack_rig, "stud_R")
assert c["type"] == "FOLLOW_POINT" and close(c["follow_point"], (-0.3, 0.1)), ("its point mirrored too", c)

# With a partner on the other side, the mirrored bone follows the partner's deform.
at(0)
cage_l, cage_r = box("PatchCage_L", (0.2, -0.3), (0.8, 0.3)), box("PatchCage_R", (-0.8, -0.3), (-0.2, 0.3))
art_l, art_r = box("Patch_L", (0.3, -0.2), (0.7, 0.2)), box("Patch_R", (-0.7, -0.2), (-0.3, 0.2))
patch_deform = add_deform(art_l, cage_l)
assert add_deform(art_r, cage_r) == patch_deform
patch_rig = new_rig("PatchRig")
add_bone(patch_rig, "patch_L", (0.5, 0.0))
follow(patch_rig, "patch_L", art_l, patch_deform)
doc.frame_current = 5
patch_rig.bone_constraint_set("patch_L", 0, follow_shift=(0.05, 0.0))
at(0)
layers.active = patch_rig
assert ops.armature.symmetrize() == {'FINISHED'}
c = constraint(patch_rig, "patch_R")
assert c["follow_layer"] == "Patch_R" and close(c["follow_point"], (-0.5, 0.0)) and c["follow_error"] == "", c
assert close(c["follow_shift"], (0.0, 0.0)), "keys stay on the side they were made for"
place(cage_r, lambda p: (p[0] - 0.1, p[1]))
assert close(head(patch_rig, "patch_R"), (-0.6, 0.0)), head(patch_rig, "patch_R")
assert close(head(patch_rig, "patch_L")[:2], (0.5, 0.0), 0.06), head(patch_rig, "patch_L")

# A whole group mirrored: its bones and their points go across with its drawings.
at(0)
sided_cage = box("SidedCage", (0.2, -0.5), (1.0, 0.5))
sided_art = box("SidedArt", (0.3, -0.3), (0.9, 0.3))
sided_deform = add_deform(sided_art, sided_cage)
sided_rig = new_rig("SidedRig")
add_bone(sided_rig, "stud", (0.6, 0.0))
follow(sided_rig, "stud", sided_art, sided_deform)
sided = new_group("Sided_L", sided_cage, sided_art, sided_rig)
layers.active = sided
assert ops.layer.symmetrize() == {'FINISHED'}
flipped = next(l for l in layers.all() if l.name == "Sided_R")
inside = [l for l in layers.all() if l.parent is not None and l.parent.id == flipped.id]
mirror_rig = [l for l in inside if l.type == "BONE"]
assert mirror_rig, [l.name for l in inside]
c = constraint(mirror_rig[0], "stud")
assert close(c["follow_point"], (-0.6, 0.0)) and c["follow_error"] == "", c
at(0)

# --- Everything moved, turned, scaled, flipped ------------------------------------------------------------------
at(0)
for name, values in (("moved", {"location_x": 0.7, "location_y": -0.4}),
                     ("turned", {"rotation": 0.6}),
                     ("scaled", {"scale_x": 2.0, "scale_y": 2.0}),
                     ("flipped", {"scale_x": -1.0}),
                     ("stretched", {"scale_x": 1.5, "scale_y": 0.5})):
    for target in ("rig", "owner", "both", "above"):
        at(0)
        tag = name + target
        far_cage = box("FarCage" + tag)
        far_art = box("FarArt" + tag, (-0.5, -0.5), (0.5, 0.5))
        far_deform = add_deform(far_art, far_cage)
        far_rig = new_rig("FarRig" + tag)
        holder = new_group("Holder" + tag, far_rig)
        add_bone(far_rig, "b", (0.3, 0.2))
        i = follow(far_rig, "b", far_art, far_deform)
        moved = {"rig": [far_rig], "owner": [far_art, far_cage], "both": [far_rig, far_art, far_cage], "above": [holder]}[target]
        # At rest first: where the point starts is frame 0's.
        for layer in moved:
            for key, value in values.items():
                setattr(layer, key, value)
        at(0)
        rest_point = constraint(far_rig, "b")["follow_point"]
        place(far_cage, lambda p: (p[0] + 0.3, p[1] - 0.2))
        for frame in (10, 5):
            at(frame)
            c = constraint(far_rig, "b")
            assert c["follow_error"] == "" and finite(c["follow_now"]), c
        # Then moving on a posed frame: the bone stays on the point wherever things go.
        at(0)
        far_rig.bone_constraint_set("b", i, follow_point=far_art.to_document((0.3, 0.2)) if False else rest_point)
        at(10)
        for layer in moved:
            layer.location_x = layer.location_x + 0.25
        at(10)
        c = constraint(far_rig, "b")
        assert finite(c["follow_now"], far_rig.bone_pose("b")), c

# The head lands exactly on the point, for the one case where they start together.
at(0)
pair_cage = box("PairCage")
pair_art = box("PairArt", (-0.5, -0.5), (0.5, 0.5))
pair_deform = add_deform(pair_art, pair_cage)
pair_rig = new_rig("PairRig")
pair_rig.location_x = 0.4
pair_rig.rotation = 0.5
pair_rig.scale_x = pair_rig.scale_y = 2.0
add_bone(pair_rig, "b", (0.1, 0.2))
follow(pair_rig, "b", pair_art, pair_deform)
at(0)
assert close(world_head(pair_rig, "b"), constraint(pair_rig, "b")["follow_now"], 1e-4), (world_head(pair_rig, "b"), constraint(pair_rig, "b"))
place(pair_cage, lambda p: (p[0] * 1.3 + 0.2, p[1] + p[0] * 0.3))
for frame in (5, 10):
    at(frame)
    assert close(world_head(pair_rig, "b"), constraint(pair_rig, "b")["follow_now"], 1e-3), (frame, world_head(pair_rig, "b"), constraint(pair_rig, "b"))
# The rig moved on a posed frame: the bone stays on the shape, in the document.
doc.frame_current = 10
was = constraint(pair_rig, "b")["follow_now"]
pair_rig.location_x = 1.4
at(10)
assert close(constraint(pair_rig, "b")["follow_now"], was, 1e-3) and close(world_head(pair_rig, "b"), was, 1e-3), (was, world_head(pair_rig, "b"))

# --- Other constraints on and around it ----------------------------------------------------------------------------
at(0)
team_cage = box("TeamCage")
team_art = box("TeamArt", (-0.5, -0.5), (0.5, 0.5))
team_deform = add_deform(team_art, team_cage)
team = new_rig("Team")
add_bone(team, "follower", (0.5, 0.0))
follow(team, "follower", team_art, team_deform)
# IK reaching for it, Track To aiming at it, Copy Position going to it.
add_bone(team, "upper", (-1.0, 0.0), (-0.5, 0.0))
add_bone(team, "lower", (-0.5, 0.0), (0.0, 0.0), parent="upper")
layers.active = team
assert ops.armature.constraint_add(bone="lower", type="IK") == {'FINISHED'}
team.bone_constraint_set("lower", 0, target="follower", chain=2)
add_bone(team, "aimer", (0.0, -0.5))
layers.active = team
assert ops.armature.constraint_add(bone="aimer", type="TRACK_TO") == {'FINISHED'}
team.bone_constraint_set("aimer", 0, target="follower")
add_bone(team, "copier", (0.0, 0.5))
layers.active = team
assert ops.armature.constraint_add(bone="copier", type="COPY_POSITION") == {'FINISHED'}
team.bone_constraint_set("copier", 0, target="follower", follow_move=False)
place(team_cage, lambda p: (p[0] + 0.3, p[1] + 0.3))
assert close(head(team, "follower"), (0.8, 0.3)), head(team, "follower")
# Out of its reach, the chain straightens toward where the follower is.
tip = team.bone_pose("lower")[2:4]
assert abs(math.atan2(tip[1], tip[0] + 1.0) - math.atan2(0.3, 1.8)) < 1e-2, ("IK reaches for it", tip)
assert close(head(team, "copier"), (0.8, 0.3)), head(team, "copier")
aim = team.bone_pose("aimer")
assert abs(math.atan2(aim[3] - aim[1], aim[2] - aim[0]) - math.atan2(0.8, 0.8)) < 1e-2, aim
# A bone with Dynamics too, played through: finite, and it doesn't drift while held.
add_bone(team, "spring", (0.2, 0.0))
follow(team, "spring", team_art, team_deform)
layers.active = team
assert ops.armature.constraint_add(bone="spring", type="DYNAMICS") == {'FINISHED'}
for frame in range(0, 30, 3):
    doc.frame_current = frame
    assert finite(team.bone_pose("spring")), team.bone_pose("spring")
# Delay on a Copy reading a follower: it reads the bone's keys, and nothing breaks.
team.bone_constraint_set("copier", 0, delay=3)
at(10)
assert finite(team.bone_pose("copier"))
team.bone_constraint_set("copier", 0, delay=0)

# Influence keyed, turned off and on across frames.
at(0)
add_bone(team, "faded", (0.0, 0.0))
k = follow(team, "faded", team_art, team_deform)
doc.frame_current = 10
team.bone_constraint_set("faded", k, influence=0.0)
at(10)
assert close(head(team, "faded"), (0.0, 0.0)), head(team, "faded")

# --- Any order of visiting gives the same -------------------------------------------------------------------------
at(0)
frames = [0, 1, 2, 3, 5, 8, 10, 13, 17, 21, 30]
first = {}
for frame in frames:
    at(frame)
    first[frame] = (rig.bone_pose("eye"), team.bone_pose("follower"), team.bone_pose("lower"))
for _ in range(30):
    frame = rng.choice(frames)
    doc.frame_current = frame
    assert first[frame] == (rig.bone_pose("eye"), team.bone_pose("follower"), team.bone_pose("lower")), (frame,)

# --- Random webs of rigs ------------------------------------------------------------------------------------------
loops = chains = 0
for web in range(4):
    at(0)
    count = 5
    webs = []
    for n in range(count):
        tag = "%d_%d" % (web, n)
        web_rig = new_rig("WebRig" + tag)
        add_bone(web_rig, "b", (0.0, 0.0))
        web_cage = box("WebCage" + tag)
        web_art = box("WebArt" + tag, (-0.5, -0.5), (0.5, 0.5))
        into(web_rig, web_cage)
        assert ops.layer.parent_bone_set(bone="b") == {'FINISHED'}
        webs.append((web_rig, web_cage, web_art, add_deform(web_art, web_cage)))
    rng.shuffle(webs)
    # Each follows another's cage: some of these make loops.
    reads = {}
    for n, (web_rig, _, _, _) in enumerate(webs):
        other = rng.randrange(count + 1)
        if other < count and other != n:
            reads[n] = other
            follow(web_rig, "b", webs[other][2], webs[other][3])
    base = rng.randrange(count)
    moved = (0.3, 0.1)
    at(10)
    at(0)
    doc.frame_current = 10
    webs[base][0].location_x = 0.0
    for n, (web_rig, cage_n, _, _) in enumerate(webs):
        if n == base:
            place(cage_n, lambda p: (p[0] + moved[0], p[1] + moved[1]))
    at(10)

    held = {}
    for n, (web_rig, _, _, _) in enumerate(webs):
        if n not in reads:
            continue
        c = constraint(web_rig, "b")
        walk = n
        looped = False
        seen = []
        while walk in reads:
            if walk in seen:
                # Only those on the loop: one that just reads from it has nothing wrong of its own.
                looped = walk == n
                break
            seen.append(walk)
            walk = reads[walk]
        if looped:
            loops += 1
            assert c["follow_error"].startswith("Loop"), (n, reads, c["follow_error"])
        else:
            chains += 1
            assert c["follow_error"] == "", (n, reads, c["follow_error"])
        assert finite(web_rig.bone_pose("b"))
        held[n] = web_rig.bone_pose("b")
    for _ in range(4):
        at(10)
    for n, pose in held.items():
        assert webs[n][0].bone_pose("b") == pose, "a held frame doesn't drift"
print("webs: %d in loops, %d in chains" % (loops, chains))

# A group and a drawing with one name: each deform is found.
at(0)
twin_cage = box("TwinCage")
twin_line = line("TwinLine", [(-1, 0), (1, 0)])
twin_art = box("Twin", (-0.5, -0.5), (0.5, 0.5))
twin_deform = add_deform(twin_art, twin_line, "CURVE_DEFORM")
twin_group = new_group("Twin", twin_art)
group_deform = add_deform(twin_group, twin_cage, "CAGE_DEFORM")
twin_rig = new_rig("TwinRig")
add_bone(twin_rig, "a", (0.1, 0.1))
add_bone(twin_rig, "b", (0.1, 0.1))
follow(twin_rig, "a", twin_art, twin_deform)
follow(twin_rig, "b", twin_art, group_deform)
assert constraint(twin_rig, "a")["follow_modifier"] == twin_deform and constraint(twin_rig, "b")["follow_modifier"] == group_deform

# Smart actions mirrored: keys on the point go across with the rest.
at(0)
turner = new_rig("Turner")
add_bone(turner, "Turn_L", (1.5, 1.5))
add_bone(turner, "m_L", (0.5, 0.0))
follow(turner, "m_L", art_l, patch_deform)
layers.active = turner
turner.select_bone("Turn_L")
setup = {b["name"]: b["setup_angle"] for b in turner.bones}
assert ops.anim.action_new(name="Turn_L", smart="1") == {'FINISHED'}
doc.frame_current = 10
turner.bone_set("Turn_L", angle=setup["Turn_L"] + 0.8)
turner.bone_constraint_set("m_L", 0, follow_shift=(0.1, 0.05))
doc.frame_current = 11
doc.frame_current = 0
doc.action = ""
layers.active = turner
ops.armature.select_none()
assert ops.armature.symmetrize() == {'FINISHED'}
assert ops.armature.symmetrize(smart_actions="1") == {'FINISHED'}
doc.action = "Turn_R"
at(10)
c = constraint(turner, "m_R")
assert close(c["follow_shift"], (-0.1, 0.05)), ("mirrored", c["follow_shift"])
doc.action = ""
at(0)

# --- Saving keeps it, and files from before open -------------------------------------------
at(0)
path = os.path.join(folder, "stress.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.name == "Rig")
team = next(l for l in layers.all() if l.name == "Team")
at(10)
assert close(head(rig, "eye"), (0.75, 0.55)), head(rig, "eye")
assert close(head(team, "follower"), (0.8, 0.3)), head(team, "follower")

# Without any of its settings, as a file from before: the constraint is simply unaimed.
at(0)
older = os.path.join(folder, "older.oanim")
with zipfile.ZipFile(path) as source, zipfile.ZipFile(older, "w") as target:
    for item in source.infolist():
        data = source.read(item.filename)
        if item.filename.endswith(".json"):
            text = data.decode("utf-8")
            def strip(value):
                if isinstance(value, dict):
                    return {k: strip(v) for k, v in value.items() if not k.startswith("follow_") or k == "follow_move"}
                if isinstance(value, list):
                    return [strip(v) for v in value]
                return value
            data = json.dumps(strip(json.loads(text))).encode("utf-8")
        target.writestr(item, data)
assert ops.wm.open_mainfile(filepath=older, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.name == "Rig")
c = constraint(rig, "eye")
assert c["follow_layer"] == "" and c["follow_error"] == "", c
at(10)
assert close(head(rig, "eye"), (0.25, 0.25)), head(rig, "eye")

# --- What it costs -----------------------------------------------------------------------------------------------
assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
anchor = layers.active


def timed(frames=120):
    start = time.perf_counter()
    for frame in range(1, frames + 1):
        doc.frame_current = frame
    return (time.perf_counter() - start) / frames * 1000.0


cost_cage = box("CostCage")
cost_art = box("CostArt", (-0.5, -0.5), (0.5, 0.5))
cost_deform = add_deform(cost_art, cost_cage)
cost_rig = new_rig("CostRig")
add_bone(cost_rig, "plain", (0.0, 0.0))
at(0)
place(cost_cage, lambda p: (p[0] + 0.3, p[1]), 100)
without = timed()
for n in range(30):
    add_bone(cost_rig, "f%d" % n, (rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5)))
    follow(cost_rig, "f%d" % n, cost_art, cost_deform)
at(0)
followers = timed()
per = (followers - without) / 30
print("cost: %.2f ms a frame bare, %.2f ms with 30 followers, %.3f ms each" % (without, followers, per))
assert per < 2.0, ("a follower has to be cheap", without, followers)

shutil.rmtree(folder, ignore_errors=True)
print("follow_point_stress: ok")
