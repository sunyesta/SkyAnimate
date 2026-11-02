"""Follow Point: a bone stays on a bending shape without bending, by a point of its own that a deform carries.

- Its point starts at the bone's head, and only the point's change reaches the bone: nothing jumps, and keys still work.
- It goes where the deform puts that point, on a drawing's deform and on a group's, by Strength and Influence.
- Position only: the bone doesn't turn or scale.
- The point moves separately from the bone, and two bones on one cage keep their own places.
- A bone above it moving the shape counts once: a face feature on a head its own bones move.
- A bone that moves its own shape, however it does, is refused and says why; nothing drifts while a frame is held.
- Chains go in order, whatever order the layers are in. Saved with the file; a deleted deform says so.
- A second point turns and scales the bone, each switchable, counting a bone above it once.
- Its points can be keyed, as their change from setup: a follower slides along the shape, through the bend.
- Or a point can be one the shape already has: the bone takes exactly that point's change.

Run with:  OpenAnimate --background --python tests/test_follow_point.py
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
anchor = layers.active


def close(a, b, eps=1e-3):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


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


def box(name, lo=(-1, -1), hi=(1, 1)):
    layer = new_layer(name)
    assert layer.add_primitive("BOX", [lo, hi])
    return layer


def add_deform(layer, shape, kind="CAGE_DEFORM"):
    layers.active = layer
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    index = len(layer.modifiers) - 1
    layer.modifier_set(index, layer=shape.name, shape=shape.shapes[0][0])
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


def move_box(cage, offset, frame=10):
    """Every point of the cage moved, on that frame."""
    doc.frame_current = 0
    rest = [p[:2] for p in cage.points]
    doc.frame_current = frame
    for i, p in enumerate(rest):
        cage.set_point(i, (p[0] + offset[0], p[1] + offset[1]))
    at(frame)


def constraint(rig, bone, index=0):
    return rig.bone_constraints(bone)[index]


# --- A drawing's deform ---------------------------------------------------------------------------------------
cage = box("Cage")
art = box("Art", (-0.5, -0.5), (0.5, 0.5))
deform = add_deform(art, cage)
rig = new_rig("Rig")
add_bone(rig, "eye", (0.25, 0.25))
assert rig.bone_constraints("eye") == []
i = follow(rig, "eye", art, deform)
c = constraint(rig, "eye")
assert c["type"] == "FOLLOW_POINT" and c["name"] == "Follow Point"
assert c["follow_layer"] == "Art" and c["follow_modifier"] == deform and c["follow_error"] == "", c
assert close(c["follow_point"], (0.25, 0.25)), ("starts at the bone's head", c)
assert not c["show_follow_point"]

at(0)
assert close(head(rig, "eye"), (0.25, 0.25))
move_box(cage, (0.5, 0.0))
assert close(head(rig, "eye"), (0.75, 0.25)), head(rig, "eye")
assert close(constraint(rig, "eye")["follow_now"], (0.75, 0.25)), "where it is shown"
at(5)
assert close(head(rig, "eye"), (0.5, 0.25)), ("between the keys", head(rig, "eye"))
at(0)
assert close(head(rig, "eye"), (0.25, 0.25)), "frame 0 is the rest"

# Only position reaches it.
doc.frame_current = 10
for index, p in enumerate(cage.points):
    if p[0] > 0:
        cage.set_point(index, (p[0], p[1] + p[0]))
at(10)
pose = rig.bone_pose("eye")
assert abs(pose[3] - pose[1]) < 1e-3, ("no turn from a slanted cage", pose)
assert abs(pose[2] - pose[0] - 0.2) < 1e-3, ("no scale", pose)

# Strength and Influence take part of it, and keys on the bone stay on top.
at(0)
move_box(cage, (0.5, 0.0))
at(10)
assert close(head(rig, "eye"), (0.75, 0.25))
rig.bone_constraint_set("eye", i, influence=0.5)
at(10)
assert close(head(rig, "eye"), (0.5, 0.25)), head(rig, "eye")
rig.bone_constraint_set("eye", i, influence=1.0)
art.modifier_set(0, strength=0.5)
at(10)
assert close(head(rig, "eye"), (0.5, 0.25)), head(rig, "eye")
art.modifier_set(0, strength=1.0)
rig.bone_constraint_set("eye", i, enabled=False)
at(10)
assert close(head(rig, "eye"), (0.25, 0.25)), "off, it stays"
rig.bone_constraint_set("eye", i, enabled=True)
doc.frame_current = 10
rig.bone_set("eye", position=(0.25, 0.35))
at(10)
assert close(head(rig, "eye"), (0.75, 0.35)), ("its own keys on top", head(rig, "eye"))
doc.frame_current = 10
rig.bone_set("eye", position=(0.25, 0.25))
at(10)

# The point moves apart from the bone, and hiding it changes nothing.
at(0)
rig.bone_constraint_set("eye", i, follow_point=(-0.25, 0.0), show_follow_point=True)
assert close(head(rig, "eye"), (0.25, 0.25)), "the bone stays when its point moves at rest"
at(10)
assert close(head(rig, "eye"), (0.75, 0.25)), "a cage moved whole moves it the same"
assert close(constraint(rig, "eye")["follow_now"], (0.25, 0.0))
rig.bone_constraint_set("eye", i, show_follow_point=False)
at(10)
assert close(head(rig, "eye"), (0.75, 0.25)), "hidden, it still follows"

# Two bones on one cage keep their own places: a corner pulled moves the near one more.
add_bone(rig, "far", (-0.75, -0.75))
far = follow(rig, "far", art, deform)
assert close(constraint(rig, "far")["follow_point"], (-0.75, -0.75))
at(10)
assert close(head(rig, "far"), (-0.25, -0.75)), head(rig, "far")
rig.bone_constraint_set("eye", i, follow_point=(0.25, 0.25))

# --- A group's deform, as the mouth cage is -----------------------------------------------------------------------
at(0)
group_cage = box("GroupCage")
inside = box("Inside", (-0.5, -0.5), (0.5, 0.5))
group = new_group("Group", inside)
group_deform = add_deform(group, group_cage)
group_rig = new_rig("GroupRig")
add_bone(group_rig, "stud", (0.25, 0.25))
g = follow(group_rig, "stud", group, group_deform)
assert constraint(group_rig, "stud")["follow_error"] == "", constraint(group_rig, "stud")
move_box(group_cage, (0.4, 0.0))
assert close(head(group_rig, "stud"), (0.65, 0.25)), head(group_rig, "stud")
# What moves the group stays on top of the deform.
doc.frame_current = 10
group.location_y = 0.3
at(10)
assert close(head(group_rig, "stud"), (0.65, 0.55)), head(group_rig, "stud")
doc.frame_current = 10
group.location_y = 0.0
at(10)

# --- A face feature on a head its own bones move -----------------------------------------------------------------
at(0)
face = new_rig("Face")
add_bone(face, "head", (0.0, 0.0), (0.0, 1.0))
add_bone(face, "stud", (0.5, 0.5), parent="head")
head_cage = box("HeadCage")
head_art = box("HeadArt", (-0.8, -0.8), (0.8, 0.8))
for layer in (head_cage, head_art):
    layers.active = layer
    assert ops.layer.move_to_group(group=str(face.id)) == {'FINISHED'}
layers.active = head_cage
assert ops.layer.parent_bone_set(bone="head") == {'FINISHED'}
head_deform = add_deform(head_art, head_cage)
f = follow(face, "stud", head_art, head_deform)
c = constraint(face, "stud")
assert c["follow_error"] == "", ("the head moving the cage is no loop for a bone on it", c)
# The head turning carries the cage and the stud: that counts once.
doc.frame_current = 10
face.bone_set("head", angle=face.bones[0]["setup_angle"] + math.pi / 2)
at(10)
turned = (-0.5, 0.5)
assert close(head(face, "stud"), turned, 1e-2), ("carried once", head(face, "stud"), constraint(face, "stud"))
assert close(constraint(face, "stud")["follow_offset"], (0, 0), 1e-2), constraint(face, "stud")

# A group the same bone moves: the turn reaches the bone through the group, and counts once.
at(0)
carrier = new_rig("Carrier")
add_bone(carrier, "turn", (0.0, 0.0), (0.0, 1.0))
add_bone(carrier, "free", (0.5, 0.5))
carried_cage = box("CarriedCage")
carried_art = box("CarriedArt", (-0.8, -0.8), (0.8, 0.8))
carried_group = new_group("CarriedGroup", carried_art)
for layer in (carried_cage, carried_group):
    layers.active = layer
    assert ops.layer.move_to_group(group=str(carrier.id)) == {'FINISHED'}
    assert ops.layer.parent_bone_set(bone="turn") == {'FINISHED'}
carried_deform = add_deform(carried_group, carried_cage)
follow(carrier, "free", carried_group, carried_deform)
assert constraint(carrier, "free")["follow_error"] == "", constraint(carrier, "free")
doc.frame_current = 10
carrier.bone_set("turn", angle=carrier.bones[0]["setup_angle"] + math.pi / 2)
at(10)
assert close(head(carrier, "free"), (-0.5, 0.5), 1e-2), ("once, not twice", head(carrier, "free"), constraint(carrier, "free"))

# --- Loops are refused and say so -------------------------------------------------------------------------------
at(0)
loop_rig = new_rig("LoopRig")
add_bone(loop_rig, "root", (0.0, 0.0))
add_bone(loop_rig, "chaser", (0.5, 0.0))
loop_cage = box("LoopCage")
loop_art = box("LoopArt", (-0.5, -0.5), (0.5, 0.5))
for layer in (loop_cage,):
    layers.active = layer
    assert ops.layer.move_to_group(group=str(loop_rig.id)) == {'FINISHED'}
    assert ops.layer.parent_bone_set(bone="chaser") == {'FINISHED'}
loop_deform = add_deform(loop_art, loop_cage)
n = follow(loop_rig, "chaser", loop_art, loop_deform)
at(10)
error = constraint(loop_rig, "chaser")["follow_error"]
assert error.startswith("Loop"), error
assert constraint(loop_rig, "chaser")["follow_offset"] == (0.0, 0.0)
# Through a target too: root copies chaser, the cage follows root.
at(0)
layers.active = loop_cage
assert ops.layer.parent_bone_set(bone="root") == {'FINISHED'}
at(10)
assert constraint(loop_rig, "chaser")["follow_error"] == "", "the bone it follows no longer moves it"
layers.active = loop_rig
assert ops.armature.constraint_add(bone="root", type="COPY_POSITION") == {'FINISHED'}
loop_rig.bone_constraint_set("root", 0, target="chaser", follow_move=True)
at(10)
assert constraint(loop_rig, "chaser")["follow_error"].startswith("Loop"), constraint(loop_rig, "chaser")
loop_rig.bone_constraint_set("root", 0, target="")
at(10)
assert constraint(loop_rig, "chaser")["follow_error"] == ""

# Two rigs following each other.
at(0)
rig_a, rig_b = new_rig("RigA"), new_rig("RigB")
add_bone(rig_a, "a", (0.0, 0.0))
add_bone(rig_b, "b", (0.0, 0.0))
cage_a, cage_b = box("CageA"), box("CageB")
art_a, art_b = box("ArtA", (-0.5, -0.5), (0.5, 0.5)), box("ArtB", (-0.5, -0.5), (0.5, 0.5))
for cage_layer, r, bone in ((cage_a, rig_a, "a"), (cage_b, rig_b, "b")):
    layers.active = cage_layer
    assert ops.layer.move_to_group(group=str(r.id)) == {'FINISHED'}
    assert ops.layer.parent_bone_set(bone=bone) == {'FINISHED'}
deform_a, deform_b = add_deform(art_a, cage_a), add_deform(art_b, cage_b)
follow(rig_a, "a", art_b, deform_b)
follow(rig_b, "b", art_a, deform_a)
at(10)
assert constraint(rig_a, "a")["follow_error"].startswith("Loop"), constraint(rig_a, "a")
assert constraint(rig_b, "b")["follow_error"].startswith("Loop"), constraint(rig_b, "b")
held = [head(rig_a, "a"), head(rig_b, "b")]
for _ in range(5):
    at(10)
assert held == [head(rig_a, "a"), head(rig_b, "b")], "nothing drifts while a frame is held"

# --- A chain: first rig follows a cage the second rig's follower moves, whichever comes first in the layers ----------
at(0)
for later_first in (False, True):
    first_name, second_name = ("Second", "First") if later_first else ("First", "Second")
    rig_1 = new_rig("Chain" + first_name)
    rig_2 = new_rig("Chain" + second_name)
    add_bone(rig_1, "one", (0.0, 0.0))
    add_bone(rig_2, "two", (0.0, 0.0))
    base = box("Base" + first_name)
    base_art = box("BaseArt" + first_name, (-0.5, -0.5), (0.5, 0.5))
    middle = box("Middle" + first_name)
    middle_art = box("MiddleArt" + first_name, (-0.5, -0.5), (0.5, 0.5))
    layers.active = middle
    assert ops.layer.move_to_group(group=str(rig_2.id)) == {'FINISHED'}
    assert ops.layer.parent_bone_set(bone="two") == {'FINISHED'}
    follow(rig_2, "two", base_art, add_deform(base_art, base))
    follow(rig_1, "one", middle_art, add_deform(middle_art, middle))
    move_box(base, (0.3, 0.0))
    assert close(head(rig_2, "two"), (0.3, 0.0)), head(rig_2, "two")
    assert close(head(rig_1, "one"), (0.3, 0.0)), ("in order", later_first, head(rig_1, "one"))
    at(0)

# --- A second point turns and scales it ---------------------------------------------------------------------------
at(0)
gem_cage = box("GemCage")
gem_art = box("GemArt", (-0.5, -0.5), (0.5, 0.5))
gem_deform = add_deform(gem_art, gem_cage)
gem_rig = new_rig("GemRig")
add_bone(gem_rig, "gem", (0.0, 0.0), (0.2, 0.0))
g = follow(gem_rig, "gem", gem_art, gem_deform)
gem_rig.bone_constraint_set("gem", g, follow_second=True)
c = constraint(gem_rig, "gem")
assert c["follow_second"] and close(c["follow_point2"], (0.2, 0.0)), ("along the bone", c)
gem_rig.bone_constraint_set("gem", g, follow_point2=(0.5, 0.0))


def transform_box(cage, fn, frame=10):
    doc.frame_current = 0
    rest = [p[:2] for p in cage.points]
    doc.frame_current = frame
    for i, p in enumerate(rest):
        cage.set_point(i, fn(p))
    at(frame)


# The cage turned a quarter turn: the bone turns with it, its head on its point.
transform_box(gem_cage, lambda p: (-p[1], p[0]))
pose = gem_rig.bone_pose("gem")
assert close(pose[:2], (0.0, 0.0)) and close(pose[2:4], (0.0, 0.2), 1e-2), ("turned", pose, constraint(gem_rig, "gem"))
assert abs(constraint(gem_rig, "gem")["follow_turn"] - math.pi / 2) < 0.05, constraint(gem_rig, "gem")["follow_turn"]
gem_rig.bone_constraint_set("gem", g, influence=0.5)
at(10)
pose = gem_rig.bone_pose("gem")
assert abs(math.atan2(pose[3] - pose[1], pose[2] - pose[0]) - math.pi / 4) < 0.05, ("Influence takes part of it", pose)
gem_rig.bone_constraint_set("gem", g, influence=1.0, follow_rotation=False)
at(10)
pose = gem_rig.bone_pose("gem")
assert close(pose[2:4], (0.2, 0.0), 1e-2), ("Rotation off", pose)
gem_rig.bone_constraint_set("gem", g, follow_rotation=True)

# The cage twice as big: scale is off until asked for, and goes both ways.
transform_box(gem_cage, lambda p: (p[0] * 2, p[1] * 2))
pose = gem_rig.bone_pose("gem")
assert close(pose[2:4], (0.2, 0.0), 1e-2), ("no scale yet", pose)
gem_rig.bone_constraint_set("gem", g, follow_scale=True)
at(10)
pose = gem_rig.bone_pose("gem")
assert abs(constraint(gem_rig, "gem")["follow_stretch"] - 2.0) < 0.06 and close(pose[2:4], (0.4, 0.0), 2e-2), ("scaled", pose)
at(0)
assert close(gem_rig.bone_pose("gem")[2:4], (0.2, 0.0)), "frame 0 is the rest"
# Off, the second point changes nothing, and it's kept.
gem_rig.bone_constraint_set("gem", g, follow_second=False)
at(10)
assert close(gem_rig.bone_pose("gem")[2:4], (0.2, 0.0), 1e-2)
gem_rig.bone_constraint_set("gem", g, follow_second=True)
assert close(constraint(gem_rig, "gem")["follow_point2"], (0.5, 0.0)), "kept"
at(10)
assert close(gem_rig.bone_pose("gem")[2:4], (0.4, 0.0), 2e-2)

# A bone above it turning the shape counts once here too: no turn of its own.
at(0)
add_bone(face, "stud2", (0.5, 0.5), parent="head")
s = follow(face, "stud2", head_art, head_deform)
face.bone_constraint_set("stud2", s, follow_second=True, follow_point2=(0.5, 0.0))
doc.frame_current = 10
at(10)
c = constraint(face, "stud2")
assert c["follow_error"] == "" and abs(c["follow_turn"]) < 1e-2, c
pose = face.bone_pose("stud2")
assert close(pose[:2], (-0.5, 0.5), 1e-2), pose

# --- The point keyed: a follower sliding along the shape ---------------------------------------------------------------
at(0)
slide_cage = box("SlideCage")
slide_art = box("SlideArt", (-0.5, -0.5), (0.5, 0.5))
slide_deform = add_deform(slide_art, slide_cage)
slide_rig = new_rig("SlideRig")
add_bone(slide_rig, "slider", (0.0, 0.0))
k = follow(slide_rig, "slider", slide_art, slide_deform)
# Keys hold the change from setup: it slides, and frame 0 stays the rest.
doc.frame_current = 10
slide_rig.bone_constraint_set("slider", k, follow_shift=(0.4, 0.0))
at(10)
assert close(constraint(slide_rig, "slider")["follow_shift"], (0.4, 0.0)), constraint(slide_rig, "slider")
assert close(head(slide_rig, "slider"), (0.4, 0.0)), head(slide_rig, "slider")
at(0)
assert close(constraint(slide_rig, "slider")["follow_shift"], (0.0, 0.0)) and close(head(slide_rig, "slider"), (0.0, 0.0))
at(5)
assert 0.0 < head(slide_rig, "slider")[0] < 0.4, ("between the keys", head(slide_rig, "slider"))
# With the cage moved too, the two add up, through the bend.
move_box(slide_cage, (0.0, 0.3))
assert close(head(slide_rig, "slider"), (0.4, 0.3)), head(slide_rig, "slider")
# Moving the point on frame 0 moves it on every frame: the keys are changes from there.
at(0)
slide_rig.bone_constraint_set("slider", k, follow_point=(-0.2, 0.0))
assert close(head(slide_rig, "slider"), (0.0, 0.0)), "the bone stays at rest"
at(10)
assert close(constraint(slide_rig, "slider")["follow_now"], (0.2, 0.3)), constraint(slide_rig, "slider")
assert close(head(slide_rig, "slider"), (0.4, 0.3)), ("the same move from where the bone is", head(slide_rig, "slider"))

# As a drag does: the point lands under the mouse through the bend, here a cage pulled unevenly.
at(0)
pull_cage = box("PullCage")
pull_art = box("PullArt", (-0.5, -0.5), (0.5, 0.5))
pull_deform = add_deform(pull_art, pull_cage)
add_bone(slide_rig, "puller", (0.1, 0.1))
q = follow(slide_rig, "puller", pull_art, pull_deform)
doc.frame_current = 0
corner = max(range(len(pull_cage.points)), key=lambda i: pull_cage.points[i][0] + pull_cage.points[i][1])
start = pull_cage.points[corner][:2]
doc.frame_current = 10
pull_cage.set_point(corner, (start[0] + 0.7, start[1] + 0.5))
at(10)
for target in ((0.5, 0.3), (-0.3, 0.2), (0.1, -0.4)):
    doc.frame_current = 10
    slide_rig.bone_constraint_set("puller", q, follow_to=target)
    at(10)
    c = constraint(slide_rig, "puller")
    assert close(c["follow_now"], target, 2e-3), ("under the mouse", target, c["follow_now"], c["follow_shift"])
    assert close(head(slide_rig, "puller"), target, 2e-3), (head(slide_rig, "puller"), target)

# A bone above it moving the shape: the keyed slide stays, on top of what the bone carries.
at(0)
face_key = new_rig("FaceKey")
add_bone(face_key, "skull", (0.0, 0.0), (0.0, 1.0))
add_bone(face_key, "sticker", (0.5, 0.5), parent="skull")
key_cage = box("KeyCage")
key_art = box("KeyArt", (-0.8, -0.8), (0.8, 0.8))
for layer in (key_cage, key_art):
    layers.active = layer
    assert ops.layer.move_to_group(group=str(face_key.id)) == {'FINISHED'}
layers.active = key_cage
assert ops.layer.parent_bone_set(bone="skull") == {'FINISHED'}
f = follow(face_key, "sticker", key_art, add_deform(key_art, key_cage))
doc.frame_current = 10
face_key.bone_constraint_set("sticker", f, follow_shift=(0.0, 0.2))
face_key.bone_set("skull", angle=face_key.bones[0]["setup_angle"] + math.pi / 2)
at(10)
assert close(head(face_key, "sticker"), (-0.7, 0.5), 2e-2), ("a quarter turn and its slide", head(face_key, "sticker"), constraint(face_key, "sticker"))

# The second point keyed turns the bone.
at(0)
add_bone(slide_rig, "turner", (0.0, 0.0))
u = follow(slide_rig, "turner", slide_art, slide_deform)
slide_rig.bone_constraint_set("turner", u, follow_second=True)
slide_rig.bone_constraint_set("turner", u, follow_point2=(0.3, 0.0))
doc.frame_current = 10
slide_rig.bone_constraint_set("turner", u, follow_shift2=(-0.3, 0.3))
at(10)
pose = slide_rig.bone_pose("turner")
assert abs(math.atan2(pose[3] - pose[1], pose[2] - pose[0]) - math.pi / 2) < 0.05, ("turned", pose)

# --- A point the shape already has --------------------------------------------------------------------------------
at(0)
pick_cage = box("PickCage")
pick_skin = box("PickSkin", (-0.5, -0.5), (0.5, 0.5))
pick_deform = add_deform(pick_skin, pick_cage)
pick_rig = new_rig("PickRig")
add_bone(pick_rig, "on_point", (0.3, 0.3))
index = follow(pick_rig, "on_point", pick_skin, pick_deform)
corner = next(k for k in range(len(pick_cage.points)) if close(pick_cage.points[k], (1, 1)))
pick_rig.bone_constraint_set("on_point", index, follow_shape_point=corner)
c = constraint(pick_rig, "on_point", index)
assert c["follow_shape_point"] == corner and c["follow_shape_point2"] is None, c
assert close(c["follow_point"], (1, 1), 1e-6), ("it rests where the shape's point does", c["follow_point"])
rest_head = head(pick_rig, "on_point")
assert close(rest_head, (0.3, 0.3)), "nothing jumps"
# Exactly the point's change, where a place of its own there would stop short on a method that strays: and it never shifts.
doc.frame_current = 10
pick_cage.set_point(corner, (1.4, 1.2))
at(10)
assert close(head(pick_rig, "on_point"), (0.7, 0.5), 1e-5), head(pick_rig, "on_point")
assert close(constraint(pick_rig, "on_point", index)["follow_now"], (1.4, 1.2), 1e-5)
pick_rig.bone_constraint_set("on_point", index, follow_shift=(0.2, 0.2))
at(10)
assert close(head(pick_rig, "on_point"), (0.7, 0.5), 1e-5), "a picked point has no shift"
pick_rig.bone_constraint_set("on_point", index, follow_shift=(0.0, 0.0))
# A deform's Strength and the constraint's Influence as for any place.
pick_skin.modifier_set(0, strength=0.5)
at(10)
assert close(head(pick_rig, "on_point"), (0.5, 0.4), 1e-5), head(pick_rig, "on_point")
pick_skin.modifier_set(0, strength=1.0)
# Moved on frame 0 the point rests there, and only the change counts.
at(0)
pick_cage.set_point(corner, (1.1, 1.0))
at(0)
assert close(constraint(pick_rig, "on_point", index)["follow_point"], (1.1, 1.0), 1e-6)
assert close(head(pick_rig, "on_point"), (0.3, 0.3)), "frame 0 is the rest"
at(10)
now = pick_cage.evaluated()[0][corner]
assert close(head(pick_rig, "on_point"), (0.3 + now[0] - 1.1, 0.3 + now[1] - 1.0), 1e-5), (head(pick_rig, "on_point"), now)
# Back to its own place, from where the point rests.
at(0)
pick_rig.bone_constraint_set("on_point", index, follow_shape_point=None)
assert constraint(pick_rig, "on_point", index)["follow_shape_point"] is None
assert close(constraint(pick_rig, "on_point", index)["follow_point"], (1.1, 1.0), 1e-6)
# Only the shape's own points, and a new deform lets go of it.
for bad in (999, -1):
    try:
        pick_rig.bone_constraint_set("on_point", index, follow_shape_point=bad)
    except ValueError:
        continue
    raise AssertionError(("a point off the shape", bad))
pick_rig.bone_constraint_set("on_point", index, follow_shape_point=corner)
pick_rig.bone_constraint_set("on_point", index, follow_deform=("Art", deform))
assert constraint(pick_rig, "on_point", index)["follow_shape_point"] is None, "its id means nothing on another shape"
pick_rig.bone_constraint_set("on_point", index, follow_deform=None)
try:
    pick_rig.bone_constraint_set("on_point", index, follow_shape_point=corner)
except ValueError:
    pass
else:
    raise AssertionError("no deform")

# Two of the shape's points turn it.
at(0)
turn_cage = box("TurnCage")
turn_skin = box("TurnSkin", (-0.5, -0.5), (0.5, 0.5))
turn_deform = add_deform(turn_skin, turn_cage)
add_bone(pick_rig, "turned", (0.0, 0.0))
index = follow(pick_rig, "turned", turn_skin, turn_deform)
points = [p[:2] for p in turn_cage.points]
pick_rig.bone_constraint_set("turned", index, follow_second=True, follow_shape_point=0, follow_shape_point2=1)
doc.frame_current = 10
for k, p in enumerate(points):
    turn_cage.set_point(k, (-p[1], p[0]))
at(10)
c = constraint(pick_rig, "turned", index)
assert c["follow_shape_point"] == 0 and c["follow_shape_point2"] == 1
assert abs(math.remainder(c["follow_turn"] - math.pi / 2, math.tau)) < 1e-5, c["follow_turn"]
pose = pick_rig.bone_pose("turned")
assert close((pose[2] - pose[0], pose[3] - pose[1]), (0.0, 0.2), 1e-4), ("the bone turned by it", pose)

# On a group's deform, as the mouth cage is: the same.
at(0)
group_cage = box("PickGroupCage")
inside = box("PickInside", (-0.5, -0.5), (0.5, 0.5))
outer = new_group("PickGroup", inside)
group_deform = add_deform(outer, group_cage)
add_bone(pick_rig, "grouped", (0.2, 0.2))
index = follow(pick_rig, "grouped", outer, group_deform)
corner = next(k for k in range(len(group_cage.points)) if close(group_cage.points[k], (1, 1)))
pick_rig.bone_constraint_set("grouped", index, follow_shape_point=corner)
doc.frame_current = 10
group_cage.set_point(corner, (1.25, 1.0))
at(10)
assert close(head(pick_rig, "grouped"), (0.45, 0.2), 1e-5), head(pick_rig, "grouped")
# What moves the group stays on top of it.
doc.frame_current = 10
outer.location_y = 0.3
at(10)
assert close(head(pick_rig, "grouped"), (0.45, 0.5), 1e-5), head(pick_rig, "grouped")
doc.frame_current = 10
outer.location_y = 0.0
at(10)

# A bone above it moving the shape counts once.
at(0)
carry_rig = new_rig("CarryRig")
add_bone(carry_rig, "carrier", (0.0, 0.0))
add_bone(carry_rig, "rider", (0.3, 0.3), parent="carrier")
carry_cage = box("CarryCage")
carry_skin = box("CarrySkin", (-0.5, -0.5), (0.5, 0.5))
for layer in (carry_cage, carry_skin):
    layers.active = layer
    assert ops.layer.move_to_group(group=str(carry_rig.id)) == {'FINISHED'}
    assert ops.layer.parent_bone_set(bone="carrier") == {'FINISHED'}
carry_deform = add_deform(carry_skin, carry_cage)
index = follow(carry_rig, "rider", carry_skin, carry_deform)
corner = next(k for k in range(len(carry_cage.points)) if close(carry_cage.points[k], (1, 1)))
carry_rig.bone_constraint_set("rider", index, follow_shape_point=corner)
doc.frame_current = 10
carry_rig.bone_set("carrier", position=(carry_rig.bones[0]["position"][0] + 0.5, carry_rig.bones[0]["position"][1]))
at(10)
assert close(head(carry_rig, "rider"), (0.8, 0.3), 1e-5), ("once", head(carry_rig, "rider"))
carry_cage.set_point(corner, (1.2, 1.1))
at(10)
assert close(head(carry_rig, "rider"), (1.0, 0.4), 1e-5), ("its change of shape on top", head(carry_rig, "rider"))

# Symmetrized, the copy's point is across from the first, its own place.
at(0)
add_bone(pick_rig, "side_L", (0.4, 0.0))
index = follow(pick_rig, "side_L", pick_skin, pick_deform)
corner = next(k for k in range(len(pick_cage.points)) if close(pick_cage.points[k], (1.1, 1.0)))
pick_rig.bone_constraint_set("side_L", index, follow_shape_point=corner)
layers.active = pick_rig
assert ops.armature.select_none() == {'FINISHED'}
pick_rig.select_bone("side_L")
assert ops.armature.symmetrize() == {'FINISHED'}
mirrored = constraint(pick_rig, "side_R", index)
assert mirrored["follow_shape_point"] is None and close(mirrored["follow_point"], (-1.1, 1.0), 1e-5), mirrored
at(0)

# --- Saved, and a deleted deform says so -------------------------------------------------------------------------
at(0)
rig.bone_constraint_set("eye", i, show_follow_point=True)
folder = tempfile.mkdtemp(prefix="oanim_follow_point_")
try:
    path = os.path.join(folder, "follow.oanim")
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
    rig = next(l for l in layers.all() if l.name == "Rig")
    art = next(l for l in layers.all() if l.name == "Art")
    c = constraint(rig, "eye")
    assert c["follow_layer"] == "Art" and c["follow_modifier"] == deform and c["show_follow_point"], c
    assert close(c["follow_point"], (0.25, 0.25)), c
    at(10)
    assert close(head(rig, "eye"), (0.75, 0.25)), head(rig, "eye")
    gem_rig = next(l for l in layers.all() if l.name == "GemRig")
    c = constraint(gem_rig, "gem")
    assert c["follow_second"] and c["follow_scale"] and c["follow_rotation"] and close(c["follow_point2"], (0.5, 0.0)), c
    assert close(gem_rig.bone_pose("gem")[2:4], (0.4, 0.0), 2e-2), gem_rig.bone_pose("gem")
    slide_rig = next(l for l in layers.all() if l.name == "SlideRig")
    at(10)
    assert close(constraint(slide_rig, "slider")["follow_shift"], (0.4, 0.0)), "the keys are saved"
    at(0)
    assert close(constraint(slide_rig, "slider")["follow_shift"], (0.0, 0.0))
    carry_rig = next(l for l in layers.all() if l.name == "CarryRig")
    carry_cage = next(l for l in layers.all() if l.name == "CarryCage")
    corner = next(k for k in range(len(carry_cage.points)) if close(carry_cage.points[k], (1, 1)))
    assert constraint(carry_rig, "rider")["follow_shape_point"] == corner
    at(10)
    assert close(head(carry_rig, "rider"), (1.0, 0.4), 1e-5), head(carry_rig, "rider")
    # Its point deleted from the shape: the bone just stays where its parent has it.
    at(0)
    layers.active = carry_cage
    carry_cage.select_points(range(len(carry_cage.points)), False)
    carry_cage.select_points([corner])
    assert ops.mesh.delete() == {'FINISHED'}
    at(10)
    assert constraint(carry_rig, "rider")["follow_shape_point"] is None
    assert close(head(carry_rig, "rider"), (0.8, 0.3), 1e-5), head(carry_rig, "rider")
    ops.ed.undo()
    at(10)
    layers.active = art
    assert ops.object.modifier_remove(index="0") == {'FINISHED'}
    at(10)
    assert constraint(rig, "eye")["follow_error"] == "Its deform is gone", constraint(rig, "eye")
    assert close(head(rig, "eye"), (0.25, 0.25)), head(rig, "eye")
    ops.ed.undo()
    at(10)
    assert close(head(rig, "eye"), (0.75, 0.25)), "undo brings it back"
finally:
    shutil.rmtree(folder, ignore_errors=True)

print("follow_point: ok")
