"""Smart actions driving smart actions: a dial's action keys other smart bones, so a 360 head turn can be made of a left and a
right turn. They play while the dial's action is edited, and on the mainline each plays after what turns its bone.

Run with:  OpenAnimate --background --python tests/test_smart_nested.py
"""

import math

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers
doc.auto_key = True

FULL = 2.0 * math.pi


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="head", head_x="0.0", head_y="-1.0", tail_x="0.0", tail_y="-0.5")
for name, y in (("left", 1.0), ("right", 2.0), ("dial", 3.0)):
    ops.armature.select_none()
    ops.armature.bone_add(name=name, head_x="5.0", head_y=str(y), tail_x="6.0", tail_y=str(y))
    rig.bone_set(name, strength=0.0)
ops.layer.new()
face = layers.active
face.add_curve([(0.0, 0.0), (1.0, 0.0)])
ops.layer.move_to_group(group=str(rig.id))
layers.active = rig


def where():
    return tuple(face.shown()[0][0])


def turn(name, angle):
    rig.bone_set(name, angle=angle)
    doc.frame_current = doc.frame_current + 1
    doc.frame_current = doc.frame_current - 1


def smart(name, angle, moved):
    """Smart action `name`: its bone turns `angle` by frame 10, taking the face's first point to `moved`."""
    rig.select_bone(name)
    assert ops.anim.action_new(name=name, smart="1") == {'FINISHED'} and doc.action == name
    doc.frame_current = 10
    rig.bone_set(name, angle=angle)
    face.set_point(0, moved)
    doc.frame_current = 11
    doc.frame_current = 0
    doc.action = ""


smart("left", 1.0, (1.0, 0.0))
smart("right", -1.0, (0.0, 1.0))

# --- The dial's action keys the two other bones ---------------------------------------------------------------------
rig.select_bone("dial")
assert ops.anim.action_new(name="dial", smart="1") == {'FINISHED'} and doc.action == "dial"
doc.frame_current = 10
rig.bone_set("dial", angle=-FULL * 0.25)
rig.bone_set("left", angle=1.0)
doc.frame_current = 11
doc.frame_current = 10
assert close(where(), (1.0, 0.0)), ("the left action plays while the dial's is edited", where())
doc.frame_current = 5
assert close(where(), (0.5, 0.0)), ("and between keys", where())

# Nothing of them goes into the dial's own keys, but the bones it keyed.
assert face.keyframes("points[0].pos") == [], face.keyframes("points[0].pos")

# Half way round the left turn hands over to the right one, a Step key apart, for the second half.
doc.frame_current = 20
rig.bone_set("dial", angle=-FULL * 0.5)
rig.bone_set("left", angle=1.0)
rig.bone_set("right", angle=0.0)
doc.frame_current = 21
rig.bone_set("left", angle=0.0)
rig.bone_set("right", angle=-1.0)
doc.frame_current = 22
doc.frame_current = 21
rig.keyframe_set('bones["left"].angle', 21, interp="STEP")
rig.keyframe_set('bones["right"].angle', 21, interp="STEP")
assert close(where(), (0.0, 1.0)), ("right plays, left let go", where())
doc.frame_current = 40
rig.bone_set("dial", angle=-FULL)
rig.bone_set("right", angle=0.0)
doc.frame_current = 41
doc.frame_current = 0
# A dial turns evenly: straight between its keys.
for bone, frames in (("dial", (10, 20, 40)), ("left", (10,)), ("right", (40,))):
    for frame in frames:
        rig.keyframe_set('bones["%s"].angle' % bone, frame, interp="LINEAR")
doc.action = ""
assert [a for a in rig.actions if a["name"] == "dial"][0]["full_turn"], rig.actions

# --- On the mainline, the dial plays them: all the way round, and round again -------------------------------------------
doc.frame_current = 5


def dial(angle):
    turn("dial", angle)
    return where()


assert close(dial(0.0), (0.0, 0.0)), where()
assert close(dial(-FULL * 0.125), (0.5, 0.0), 0.02), where()
assert close(dial(-FULL * 0.25), (1.0, 0.0), 0.02), where()
assert close(dial(-FULL * 0.5), (1.0, 0.0), 0.02), ("still the left turn, up to the Step key", where())
assert close(dial(-FULL * 0.5 - 0.25), (0.0, 1.0), 0.05), ("past it, the right one", where())
assert close(dial(-FULL * 0.75), (0.0, 0.5), 0.05), where()
assert close(dial(-FULL + 0.01), (0.0, 0.0), 0.05), where()
assert close(dial(-FULL * 1.25), (1.0, 0.0), 0.02), ("a 360 goes round again", where())
assert close(dial(FULL * 0.75), (1.0, 0.0), 0.02), ("and the other way", where())

# Between the Step key's two frames it holds, never a blend of the two turns.
dial(0.0)
held = dial(-FULL * 0.5 - (FULL * 0.5) / 20.0 * 0.5)
assert close(held, (1.0, 0.0), 0.02), ("half way across the key's frame", held)

# Turned by hand as well, the two add.
dial(0.0)
turn("left", 0.5)
assert close(where(), (0.5, 0.0), 0.02), where()
assert close(dial(-FULL * 0.125), (1.0, 0.0), 0.03), ("the dial's half on top of the hand's", where())
dial(0.0)
turn("left", 0.0)

# --- Editing a turn itself, the dial doesn't play: nothing in it turns the dial ------------------------------------------
doc.action = "left"
doc.frame_current = 10
assert close(where(), (1.0, 0.0)), where()
doc.frame_current = 0
doc.action = ""

# --- Two smart actions that turn each other don't hang, and each plays once ------------------------------------------
ops.wm.read_homefile(confirmed="1")
doc, layers = oanim.data.document, oanim.data.layers
doc.auto_key = True
ops.layer.new_bone()
rig = layers.active
for name, y in (("a", 1.0), ("b", 2.0)):
    ops.armature.select_none()
    ops.armature.bone_add(name=name, head_x="5.0", head_y=str(y), tail_x="6.0", tail_y=str(y))
for name, other in (("a", "b"), ("b", "a")):
    rig.select_bone(name)
    assert ops.anim.action_new(name=name, smart="1") == {'FINISHED'}
    doc.frame_current = 10
    rig.bone_set(name, angle=1.0)
    rig.bone_set(other, angle=0.5)
    doc.frame_current = 11
    doc.frame_current = 0
    doc.action = ""
doc.frame_current = 5
rig.bone_set("a", angle=0.5)
doc.frame_current = 6
doc.frame_current = 5
assert len(rig.bones) == 2
