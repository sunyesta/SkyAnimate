"""2 Axis Controllers: a bone converted into a box with a knob, whose four sides play actions. The knob's position is the
action frame. An ordinary action is played by the controller; an action bone action stays its bone's, the bone and the
knob linked, one set of keys. The knob has one place on an axis, so the other side's bone is held at rest.

Run with:  OpenAnimate --background --python tests/test_controller.py
"""

import math
import os
import tempfile

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers
doc.auto_key = True

QUARTER = math.pi / 2.0


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="head", head_x="0.0", head_y="-1.0", tail_x="0.0", tail_y="-0.5")
for name, y in (("turn_r", 1.0), ("turn_l", 2.0), ("dial", 3.0)):
    ops.armature.select_none()
    ops.armature.bone_add(name=name, head_x="5.0", head_y=str(y), tail_x="6.0", tail_y=str(y))
    rig.bone_set(name, strength=0.0)
ops.armature.select_none()
ops.armature.bone_add(name="stick", head_x="3.0", head_y="3.0", tail_x="3.5", tail_y="3.0")
ops.layer.new()
face = layers.active
face.add_curve([(0.0, 0.0), (1.0, 0.0)])
ops.layer.move_to_group(group=str(rig.id))
layers.active = rig


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


def settle():
    doc.frame_current = doc.frame_current + 1
    doc.frame_current = doc.frame_current - 1


def where():
    return tuple(face.shown()[0][0])


def mainline():
    doc.action = ""
    doc.frame_current = 0


def ordinary(name, frames, moved):
    """Ordinary action `name`: the face's first point goes to `moved` by `frames`."""
    assert ops.anim.action_new(name=name) == {'FINISHED'} and doc.action == name
    doc.frame_current = frames
    face.set_point(0, moved)
    doc.frame_current = frames + 1
    mainline()


def action_bone(name, frames, angle, moved):
    """Action bone action `name`: its bone turns `angle` by `frames`, taking the face's first point to `moved`."""
    rig.select_bone(name)
    assert ops.anim.action_new(name=name, action_bone="1") == {'FINISHED'} and doc.action == name
    doc.frame_current = frames
    rig.bone_set(name, angle=bone(name)["setup_angle"] + angle)
    face.set_point(0, moved)
    doc.frame_current = frames + 1
    mainline()


def side(side_id, action, keep=None):
    args = {"side": side_id, "action": action}
    if keep:
        args["keep"] = keep
    return ops.armature.controller_side(**args)


def turn(name, angle):
    """Turns a bone by hand to `angle` from its rest."""
    rig.select_bone(name)
    rig.bone_set(name, angle=bone(name)["setup_angle"] + angle)
    settle()


# --- Convert To ---------------------------------------------------------------------------------------------------
assert bone("stick")["type"] == "REGULAR"
rig.select_bone("stick")
doc.frame_current = 5
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'CANCELLED'}, "the setup frame only, as other rig changes"
assert bone("stick")["type"] == "REGULAR"
doc.frame_current = 0
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert bone("stick")["type"] == "CONTROLLER_2D"
assert bone("stick")["controller_box"] == ((0.0, 0.0), (0.0, 0.0)), "no actions: no sides"
ops.ed.undo()
assert bone("stick")["type"] == "REGULAR", "one undo step"
ops.ed.redo()
assert bone("stick")["type"] == "CONTROLLER_2D"

# --- Ordinary actions, sized by their frames ------------------------------------------------------------------------
ordinary("Look Right", 40, (2.0, 0.0))
ordinary("Look Left", 30, (-1.0, 0.0))
ordinary("Look Up", 20, (0.0, 1.0))
rig.select_bone("stick")
assert side("RIGHT", "Look Right") == {'FINISHED'}
assert side("LEFT", "Look Left") == {'FINISHED'}
assert bone("stick")["controller_box"] == ((-30.0, 0.0), (40.0, 0.0)), ("frames make the box", bone("stick"))
assert side("UP", "Look Up") == {'FINISHED'}
assert bone("stick")["controller_box"] == ((-30.0, 0.0), (40.0, 20.0)), "a side with no action isn't there"
assert side("DOWN", "Look Up") == {'CANCELLED'}, "an action has one side"
assert side("RIGHT", "nothing here") == {'CANCELLED'}

# A wider action, a wider box, and the frame size is what says how far a frame is.
size = bone("stick")["controller_frame_size"]
assert close(size, 0.5 / 40.0, 1e-5), ("fitted to the longest side", size)
rig.bone_set("stick", controller_frame_size=0.01)
assert close(bone("stick")["controller_frame_size"], 0.01, 1e-6)

doc.frame_current = 10
rig.select_bone("stick")
rig.bone_set("stick", knob=(20.0, 0.0))
settle()
assert close(bone("stick")["knob"], (20.0, 0.0))
assert close(where(), (1.0, 0.0)), ("frame 20 of the action, not a share of the box", where())
rig.bone_set("stick", knob=(-15.0, 0.0))
settle()
assert close(where(), (-0.5, 0.0)), where()
rig.bone_set("stick", knob=(20.0, 10.0))
settle()
assert close(where(), (1.0, 0.5)), ("the sides add together, as action bone actions do", where())
rig.bone_set("stick", knob=(500.0, 0.0))
assert close(bone("stick")["knob"], (40.0, 0.0)), "it stays in the box"
rig.bone_set("stick", knob=(0.0, -5.0))
assert close(bone("stick")["knob"], (0.0, 0.0)), "no Down action, no way down"
settle()

# Keys hold frames: a longer action leaves a key on its pose, a shorter one shows the last frame.
rig.bone_set("stick", knob=(20.0, 0.0))
doc.frame_current = 11
doc.frame_current = 10
assert any(k["frame"] == 10 for k in rig.keyframes('bones["stick"].position')), "the knob keys as any bone does"
assert close(rig.keyframes('bones["stick"].position')[-1]["value"], (20.0, 0.0))
doc.frame_current = 0
doc.action = "Look Right"
doc.frame_current = 60
face.set_point(0, (9.0, 0.0))
doc.frame_current = 20
frame_20 = where()
doc.frame_current = 21
mainline()
doc.frame_current = 10
assert bone("stick")["controller_box"] == ((-30.0, 0.0), (60.0, 20.0)), "the box follows the action"
assert close(bone("stick")["knob"], (20.0, 0.0)), "a key holds a frame, not a share of the box"
assert close(where(), frame_20), ("a longer action keeps a key on its pose", where(), frame_20)
doc.frame_current = 0
doc.action = "Look Right"
doc.frame_current = 60
face.keyframe_delete("points[0].pos", 60)
face.keyframe_delete("points[0].pos", 40)
doc.frame_current = 15
face.set_point(0, (1.5, 0.0))
doc.frame_current = 16
mainline()
doc.frame_current = 10
assert bone("stick")["controller_box"][1][0] == 15.0, bone("stick")
assert close(where(), (1.5, 0.0)), ("past its end it shows the last frame", where())
assert close(bone("stick")["knob"], (20.0, 0.0)), "and the key stays"
playing = {name: (t, share) for name, t, share in rig.action_bone_actions_playing}
assert close(playing["Look Right"], (15.0, 1.0)), ("and it counts as its last frame", playing)

# Inside its own action the knob shows the frame, and isn't moved.
doc.frame_current = 0
doc.action = "Look Right"
doc.frame_current = 6
assert close(bone("stick")["knob_shown"], (6.0, 0.0)), bone("stick")
mainline()

# --- It bends nothing --------------------------------------------------------------------------------------------
assert face.bone_weights("stick") == [0.0, 0.0], "automatic weights leave a controller out"
ops.layer.new()
beside = layers.active
beside.add_curve([(3.0, 3.0), (3.4, 3.0)])
ops.layer.move_to_group(group=str(rig.id))
assert ops.weight.automatic() == {'FINISHED'}
assert beside.bone_weights("stick") == [0.0, 0.0], ("even a drawing right on it", beside.bone_weights("stick"))
assert all(w > 0.9 for w in beside.bone_weights("head")), beside.bone_weights("head")
layers.active = rig
doc.frame_current = 10
head = bone("stick")["head"]
rig.bone_set("stick", knob=(10.0, 5.0))
settle()
assert close(bone("stick")["head"], head), "posed, the bone stays where it rests: only the knob moves"
rig.select_bone("stick")
assert ops.armature.rotate(angle="30") == {'FINISHED'} and close(bone("stick")["head"], head)
assert ops.armature.resize(scale="2") == {'FINISHED'}
assert close(bone("stick")["scale"], (1.0, 1.0)), "R and S have nothing to turn or scale"
assert not rig.keyframes('bones["stick"].angle') and not rig.keyframes('bones["stick"].scale')
settle()
rig.bone_set("stick", knob=(0.0, 0.0))
settle()
doc.frame_current = 0

# Moving the knob by dragging, as the viewport does: bone layer units through the frame size.
doc.frame_current = 12
rig.select_bone("stick")
assert rig.drag_bone_ends([(0.05, 0.0), (0.1, 0.1)])
assert close(bone("stick")["knob"], (10.0, 10.0)), ("0.1 at 0.01 a frame, both ways", bone("stick")["knob"])
assert close(bone("stick")["head"], head)
doc.frame_current = 0

# The box rides on a parent.
rig.select_bone("stick")
rig.select_bone("head", extend=True)
assert ops.armature.parent_set(type="OFFSET") == {'FINISHED'}
doc.frame_current = 12
rest = bone("stick")["head"]
rig.bone_set("head", position=(1.0, 0.0))
settle()
moved = bone("stick")["head"]
assert close((moved[0] - rest[0], moved[1] - rest[1]), (1.0, 1.0)), ("it follows its parent", rest, moved)
doc.frame_current = 0
rig.keyframe_delete('bones["head"].position', 12)
rig.keyframe_delete('bones["head"].position', 0)
rig.select_bone("stick")
assert ops.armature.parent_clear(type="CLEAR") == {'FINISHED'}

for k in list(rig.keyframes('bones["stick"].position')):
    if k["frame"]:
        rig.keyframe_delete('bones["stick"].position', k["frame"])

# --- A bone's action in a side: linked ---------------------------------------------------------------------------
action_bone("turn_r", 90, QUARTER, (1.0, 1.0))
action_bone("turn_l", 60, -QUARTER, (-1.0, 1.0))
ops.armature.select_none()
ops.armature.bone_add(name="joy", head_x="-3.0", head_y="3.0", tail_x="-2.5", tail_y="3.0")
rig.select_bone("joy")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert side("RIGHT", "turn_r") == {'FINISHED'}
assert side("LEFT", "turn_l") == {'FINISHED'}
assert bone("joy")["controller_box"] == ((-60.0, 0.0), (90.0, 0.0)), bone("joy")
assert bone("turn_r")["controller"] == "joy" and bone("turn_l")["controller"] == "joy"
kinds = {a["name"]: a["kind"] for a in rig.actions}
assert kinds["turn_r"] == "ACTION_BONE" and kinds["Look Right"] == "ACTION_BONE", kinds

# An action that holds a linked bone at 0, as every action does the bones it doesn't move, doesn't pull it back.
ops.armature.select_none()
ops.armature.bone_add(name="nodder", head_x="5.0", head_y="-2.0", tail_x="6.0", tail_y="-2.0")
rig.bone_set("nodder", strength=0.0)
action_bone("nodder", 90, QUARTER, (0.0, 0.0))
doc.action = "nodder"
rig.keyframe_insert('bones["turn_r"].angle', 90)
assert [k["frame"] for k in rig.keyframes('bones["turn_r"].angle') if k["frame"]] == [90], "held at 0 in nodder"
mainline()
rig.select_bone("joy")
assert side("UP", "nodder") == {'FINISHED'}
doc.frame_current = 10
rig.select_bone("joy")
rig.bone_set("joy", knob=(45.0, 45.0))
settle()
assert close(bone("turn_r")["angle"] - bone("turn_r")["setup_angle"], QUARTER / 2.0), bone("turn_r")
assert close(bone("joy")["knob_playing"], (45.0, 45.0), 0.05), bone("joy")
rig.bone_set("joy", knob=(0.0, 0.0))
settle()
doc.frame_current = 0
assert side("UP", "") == {'FINISHED'}
for path in ('bones["joy"].position', 'bones["nodder"].angle'):
    for k in list(rig.keyframes(path)):
        rig.keyframe_delete(path, k["frame"])

# Knob to bone: it turns, and it is the bone that plays the action.
doc.frame_current = 10
rig.select_bone("joy")
rig.bone_set("joy", knob=(45.0, 0.0))
settle()
assert close(bone("turn_r")["angle"] - bone("turn_r")["setup_angle"], QUARTER / 2.0), bone("turn_r")
assert close(bone("turn_l")["angle"] - bone("turn_l")["setup_angle"], 0.0)
assert close(where(), (0.5, 0.5)), where()
doc.frame_current = 11
doc.frame_current = 10
keys = rig.keyframes('bones["joy"].position')
assert [k["frame"] for k in keys] == [0, 10] and close(keys[-1]["value"], (45.0, 0.0)), keys
assert not rig.keyframes('bones["turn_r"].angle'), "one set of keys, the knob's"

# Bone to knob: either can be moved, and the key goes in the same place.
doc.frame_current = 20
turn("turn_r", QUARTER * 2.0 / 3.0)
assert close(bone("joy")["knob"], (60.0, 0.0), 0.05), bone("joy")
assert close(where(), (2.0 / 3.0, 2.0 / 3.0), 0.01), where()
doc.frame_current = 21
doc.frame_current = 20
assert [k["frame"] for k in rig.keyframes('bones["joy"].position')] == [0, 10, 20]
assert not rig.keyframes('bones["turn_r"].angle')

# The timeline: the bone's row is the knob's keys, so the same keys answer either.
rig.select_bone("turn_r")
rows = [r for r in oanim.wm.timeline_rows() if r[2]]
assert any("joy" in r[1] for r in rows), ("selecting the bone shows the knob's keys", rows)
rig.select_bone("joy")
assert any("joy" in r[1] for r in oanim.wm.timeline_rows() if r[2])

# I on a linked bone keys the knob, never the bone.
rig.keyframe_insert('bones["turn_r"].angle', 25)
settle()
assert not rig.keyframes('bones["turn_r"].angle'), "a key on the bone goes to the knob"
assert any(k["frame"] == 25 for k in rig.keyframes('bones["joy"].position'))

# Crossing the middle: the knob passes through rest, both bones at rest there, never both turning.
doc.frame_current = 0
for k in list(rig.keyframes('bones["joy"].position')):
    if k["frame"]:
        rig.keyframe_delete('bones["joy"].position', k["frame"])
doc.frame_current = 10
rig.select_bone("joy")
rig.bone_set("joy", knob=(-60.0, 0.0))
doc.frame_current = 30
rig.bone_set("joy", knob=(60.0, 0.0))
doc.frame_current = 31
for frame in (0, 10, 30):
    rig.keyframe_set('bones["joy"].position', frame, interp="LINEAR")
doc.frame_current = 20
settle()
assert close(bone("joy")["knob"], (0.0, 0.0)), bone("joy")
assert close(bone("turn_r")["angle"], bone("turn_r")["setup_angle"]) and close(bone("turn_l")["angle"], bone("turn_l")["setup_angle"])
assert close(where(), (0.0, 0.0)), ("through rest, no mix of the two", where())

# --- The block ------------------------------------------------------------------------------------------------------
doc.frame_current = 10
settle()
assert close(bone("joy")["knob"], (-60.0, 0.0)), bone("joy")
assert bone("turn_r")["held"] and not bone("turn_l")["held"], "the knob is on the left: the right bone is held"
turn("turn_r", 0.5)
assert close(bone("turn_r")["angle"] - bone("turn_r")["setup_angle"], 0.0), "it won't leave rest"
assert close(bone("joy")["knob"], (-60.0, 0.0)), "and the knob stays"
said = oanim.wm.banner_report()
assert "held" in said[3] or "held" in said[2], ("and it says why", said)
# The same the other way: on the right, the left bone is held.
rig.select_bone("joy")
rig.bone_set("joy", knob=(30.0, 0.0))
settle()
assert bone("turn_l")["held"] and not bone("turn_r")["held"]
turn("turn_l", -0.5)
assert close(bone("turn_l")["angle"] - bone("turn_l")["setup_angle"], 0.0), "it won't leave rest"
assert close(bone("joy")["knob"], (30.0, 0.0)), bone("joy")

# Picking both and turning them: neither moves.
rig.bone_set("joy", knob=(0.0, 0.0))
settle()
rig.select_bone("turn_r")
rig.select_bone("turn_l", extend=True)
assert ops.armature.rotate(angle="20") == {'FINISHED'}
settle()
assert close(bone("joy")["knob"], (0.0, 0.0)), bone("joy")
assert close(bone("turn_r")["angle"], bone("turn_r")["setup_angle"])
assert close(bone("turn_l")["angle"], bone("turn_l")["setup_angle"])

# Alt R on a linked bone sends the knob home.
turn("turn_l", -QUARTER / 2.0)
assert close(bone("joy")["knob"], (-30.0, 0.0)), bone("joy")
rig.select_bone("turn_l")
assert oanim.wm.event_simulate("R", alt=True, area="Viewport")
settle()
assert close(bone("joy")["knob"], (0.0, 0.0)), bone("joy")
assert close(bone("turn_l")["angle"], bone("turn_l")["setup_angle"]), bone("turn_l")

# Moving back toward rest is never blocked, and the other side is then free.
turn("turn_l", -QUARTER / 2.0)
assert close(bone("joy")["knob"], (-30.0, 0.0)), bone("joy")
turn("turn_l", 0.0)
assert close(bone("joy")["knob"], (0.0, 0.0)), bone("joy")
assert not bone("turn_r")["held"]
turn("turn_r", 0.5)
assert bone("joy")["knob"][0] > 1.0, bone("joy")

# Bones stop at their action's ends, and don't turn the wrong way.
turn("turn_r", QUARTER * 3.0)
assert close(bone("joy")["knob"], (90.0, 0.0)) and close(bone("turn_r")["angle"] - bone("turn_r")["setup_angle"], QUARTER), bone("turn_r")
turn("turn_r", -0.4)
assert close(bone("joy")["knob"], (0.0, 0.0)) and close(bone("turn_r")["angle"] - bone("turn_r")["setup_angle"], 0.0), bone("turn_r")

# The link is the mainline's: inside an action the bone is keyed as ever.
doc.frame_current = 0
doc.action = "turn_r"
doc.frame_current = 45
rig.bone_set("turn_r", angle=bone("turn_r")["setup_angle"] + 1.0)
doc.frame_current = 46
doc.frame_current = 45
assert close(bone("turn_r")["angle"] - bone("turn_r")["setup_angle"], 1.0), "no limit, no block in there"
rig.keyframe_delete('bones["turn_r"].angle', 45)
mainline()

# Frame 0 is the rig at rest.
assert close(bone("turn_r")["angle"], bone("turn_r")["setup_angle"]) and close(bone("joy")["knob"], (0.0, 0.0))
setup = bone("joy")["setup_position"]
doc.frame_current = 1
doc.frame_current = 0
assert close(bone("joy")["setup_position"], setup), "no pose ever becomes its setup"

# --- A dial that drives the linked bones goes on working --------------------------------------------------------------
for k in list(rig.keyframes('bones["joy"].position')):
    if k["frame"]:
        rig.keyframe_delete('bones["joy"].position', k["frame"])
rig.select_bone("dial")
assert ops.anim.action_new(name="dial", action_bone="1") == {'FINISHED'}
doc.frame_current = 90
rig.bone_set("dial", angle=bone("dial")["setup_angle"] + 1.0)
rig.bone_set("turn_r", angle=bone("turn_r")["setup_angle"] + QUARTER / 2.0)
doc.frame_current = 91
mainline()
doc.frame_current = 40
turn("dial", 1.0)
assert close(where(), (0.5, 0.5)), ("the dial turns the bone, and it plays, as before", where())
assert close(bone("joy")["knob"], (0.0, 0.0)), "that isn't the knob's"
assert close(bone("joy")["knob_playing"], (45.0, 0.0), 0.5), ("the box shows where it really is", bone("joy"))
turn("dial", 0.0)
doc.frame_current = 0

# --- Fixes ----------------------------------------------------------------------------------------------------------
for k in list(rig.keyframes('bones["joy"].position')):
    if k["frame"]:
        rig.keyframe_delete('bones["joy"].position', k["frame"])
for k in list(rig.keyframes('bones["dial"].angle')):
    rig.keyframe_delete('bones["dial"].angle', k["frame"])
ops.armature.select_none()
ops.armature.bone_add(name="corner", head_x="-3.0", head_y="-3.0", tail_x="-2.5", tail_y="-3.0")
rig.select_bone("corner")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
ordinary("C Right", 20, (2.0, 2.0))
ordinary("C Up", 20, (-2.0, 2.0))
rig.select_bone("corner")
assert side("RIGHT", "C Right") == {'FINISHED'} and side("UP", "C Up") == {'FINISHED'}
doc.frame_current = 50
rig.bone_set("corner", knob=(20.0, 20.0))
settle()
assert close(where(), (0.0, 4.0)), where()
assert ops.anim.fix_action_new() == {'FINISHED'}, "Fix This Pose works from a knob as from two bones"
fix = doc.action
assert fix and bone("corner")["controller_fix_poses"] == [(20.0, 20.0)], (fix, bone("corner"))
assert close(bone("corner")["knob_shown"], (20.0, 20.0)), bone("corner")
face.set_point(0, (0.5, 3.0))
doc.frame_current = doc.frame_current + 1
doc.frame_current = doc.frame_current - 1
mainline()
doc.frame_current = 50
settle()
assert close(where(), (0.5, 3.0)), ("the fix plays at the corner", where())
rig.bone_set("corner", knob=(20.0, 0.0))
settle()
assert close(where(), (2.0, 2.0)), ("and not with one side alone", where())
doc.frame_current = 0

for k in list(rig.keyframes('bones["corner"].position')):
    if k["frame"]:
        rig.keyframe_delete('bones["corner"].position', k["frame"])

# --- Letting go -----------------------------------------------------------------------------------------------------
# A side emptied: the bone gets keys of its own, moving as it did.
doc.frame_current = 30
rig.select_bone("joy")
rig.bone_set("joy", knob=(45.0, 0.0))
doc.frame_current = 31
doc.frame_current = 0
assert side("RIGHT", "") == {'FINISHED'}
assert "controller" not in bone("turn_r")
assert [k["frame"] for k in rig.keyframes('bones["turn_r"].angle')] == [0, 30]
doc.frame_current = 30
settle()
assert close(bone("turn_r")["angle"] - bone("turn_r")["setup_angle"], QUARTER / 2.0), bone("turn_r")
doc.frame_current = 0

# Picking it again: its keys move onto the knob, and the same pose shows.
rig.select_bone("joy")
assert side("RIGHT", "turn_r") == {'FINISHED'}
assert not rig.keyframes('bones["turn_r"].angle')
assert any(k["frame"] == 30 and close(k["value"], (45.0, 0.0)) for k in rig.keyframes('bones["joy"].position'))

# Keys on both is one too many: it says so and keep says whose.
assert side("RIGHT", "") == {'FINISHED'}
rig.keyframe_insert('bones["joy"].position', 30)
doc.frame_current = 30
rig.select_bone("joy")
rig.bone_set("joy", knob=(-20.0, 0.0))
doc.frame_current = 31
doc.frame_current = 0
assert side("RIGHT", "turn_r") == {'CANCELLED'}, "both have keys"
assert side("RIGHT", "turn_r", keep="THIS") == {'FINISHED'}
assert any(k["frame"] == 30 and close(k["value"], (45.0, 0.0)) for k in rig.keyframes('bones["joy"].position'))

# The controller deleted: its bones keep moving.
doc.frame_current = 0
rig.select_bone("joy")
assert ops.armature.bone_delete() == {'FINISHED'}
assert not any(b["name"] == "joy" for b in rig.bones)
assert "controller" not in bone("turn_r") and "controller" not in bone("turn_l")
doc.frame_current = 30
settle()
assert close(bone("turn_r")["angle"] - bone("turn_r")["setup_angle"], QUARTER / 2.0), bone("turn_r")
doc.frame_current = 0
for name in ("turn_r", "turn_l"):
    for k in list(rig.keyframes(f'bones["{name}"].angle')):
        rig.keyframe_delete(f'bones["{name}"].angle', k["frame"])

# --- A dial converted: its actions go in Right and Left, its keys onto the knob, and back -----------------------------
doc.frame_current = 0
ops.armature.select_none()
ops.armature.bone_add(name="nod", head_x="7.0", head_y="1.0", tail_x="8.0", tail_y="1.0")
rig.bone_set("nod", strength=0.0)
action_bone("nod", 30, 1.0, (0.0, -1.0))
assert ops.anim.action_new(name="nod 2") == {'FINISHED'}
doc.frame_current = 30
rig.bone_set("nod", angle=bone("nod")["setup_angle"] - 1.0)
face.set_point(0, (0.0, 1.0))
doc.frame_current = 31
mainline()
doc.frame_current = 15
turn("nod", 0.5)
doc.frame_current = 16
doc.frame_current = 15
assert close(where(), (0.0, -0.5)), where()
doc.frame_current = 0
rig.select_bone("nod")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert bone("nod")["controller_actions"]["RIGHT"] == "nod" and bone("nod")["controller_actions"]["LEFT"] == "nod 2"
assert not rig.keyframes('bones["nod"].angle')
doc.frame_current = 15
settle()
assert close(bone("nod")["knob"], (15.0, 0.0)), bone("nod")
assert close(where(), (0.0, -0.5)), ("nothing stops playing", where())
doc.frame_current = 0
assert ops.armature.bone_convert(type="REGULAR") == {'FINISHED'}
assert bone("nod")["type"] == "REGULAR"
assert [k["frame"] for k in rig.keyframes('bones["nod"].angle')] == [0, 15]
doc.frame_current = 15
settle()
assert close(bone("nod")["angle"] - bone("nod")["setup_angle"], 0.5) and close(where(), (0.0, -0.5)), where()
doc.frame_current = 0

# --- Make Left from Right -------------------------------------------------------------------------------------------
ops.armature.select_none()
ops.armature.bone_add(name="mirrored", head_x="-5.0", head_y="-3.0", tail_x="-4.5", tail_y="-3.0")
rig.select_bone("mirrored")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
ordinary("M Right", 25, (1.0, 0.25))
rig.select_bone("mirrored")
assert side("RIGHT", "M Right") == {'FINISHED'}
assert ops.armature.controller_mirror(side="LEFT") == {'FINISHED'}
left = bone("mirrored")["controller_actions"]["LEFT"]
assert left == "M Left", ("named as the other side", left)
assert bone("mirrored")["controller_box"] == ((-25.0, 0.0), (25.0, 0.0)), bone("mirrored")

# New action for a side: named for it, opened, a side to pull on before it has keys.
assert ops.armature.controller_action_new(side="UP") == {'FINISHED'}
assert doc.action == "mirrored Up"
assert bone("mirrored")["controller_actions"]["UP"] == "mirrored Up"
assert bone("mirrored")["controller_box"][1][1] == 24.0, bone("mirrored")
doc.frame_current = 12
face.set_point(0, (0.0, 0.7))
doc.frame_current = 13
doc.frame_current = 12
assert bone("mirrored")["controller_box"][1][1] == 12.0, ("its keys say while it's edited", bone("mirrored"))
mainline()

# Renames follow, and a deleted action leaves its side.
assert ops.anim.action_rename(name="M Right", to="Right Turn") == {'FINISHED'}
assert bone("mirrored")["controller_actions"]["RIGHT"] == "Right Turn"
assert ops.anim.action_delete(name="Right Turn") == {'FINISHED'}
assert bone("mirrored")["controller_actions"]["RIGHT"] == ""
assert bone("mirrored")["controller_box"][1][0] == 0.0, "its side has gone"

# Moved with Auto Keying off, a knob with no keys still goes home on the setup frame, never moving its box.
ops.armature.select_none()
ops.armature.bone_add(name="loose", head_x="1.0", head_y="-3.0", tail_x="1.5", tail_y="-3.0")
rig.select_bone("loose")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_action_new(side="RIGHT") == {'FINISHED'}
mainline()
doc.auto_key = False
doc.frame_current = 20
rig.select_bone("loose")
rig.bone_set("loose", knob=(10.0, 0.0))
settle()
doc.frame_current = 0
doc.frame_current = 5
doc.frame_current = 0
assert close(bone("loose")["knob"], (0.0, 0.0)), bone("loose")
assert close(bone("loose")["setup_position"], (1.0, -3.0)), ("the box stays where it was put", bone("loose"))
doc.auto_key = True

# --- Copies ---------------------------------------------------------------------------------------------------------
ops.armature.select_none()
ops.armature.bone_add(name="eye_L", head_x="2.0", head_y="-3.0", tail_x="2.5", tail_y="-3.0")
rig.select_bone("eye_L")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
ordinary("look_L", 20, (0.1, 0.0))
ordinary("look_R", 20, (-0.1, 0.0))
rig.select_bone("eye_L")
assert side("RIGHT", "look_L") == {'FINISHED'}
assert ops.armature.symmetrize() == {'FINISHED'}
assert bone("eye_R")["type"] == "CONTROLLER_2D", "the other side is a controller too"
assert bone("eye_R")["controller_actions"]["LEFT"] == "look_R", ("across, the other side's action", bone("eye_R"))
assert bone("eye_R")["controller_actions"]["RIGHT"] == ""
rig.select_bone("eye_L")
assert ops.armature.duplicate_move() == {'FINISHED'}
copy = next(b for b in rig.bones if b["name"].startswith("eye_L."))
assert copy["type"] == "CONTROLLER_2D" and not any(copy["controller_actions"].values()), "an action has one controller"

# --- Files ----------------------------------------------------------------------------------------------------------
doc.frame_current = 22
rig.select_bone("stick")
rig.bone_set("stick", knob=(10.0, 0.0))
doc.frame_current = 23
doc.frame_current = 0
path = os.path.join(tempfile.mkdtemp(), "controller.oanim")
assert ops.wm.save_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
rig = next(l for l in oanim.data.layers if l.bones)
assert bone("stick")["type"] == "CONTROLLER_2D"
assert bone("stick")["controller_actions"]["RIGHT"] == "Look Right"
assert close(bone("stick")["controller_frame_size"], 0.01, 1e-6)
assert bone("stick")["controller_box"] == ((-30.0, 0.0), (15.0, 20.0)), bone("stick")
assert any(k["frame"] == 22 and close(k["value"], (10.0, 0.0)) for k in rig.keyframes('bones["stick"].position'))

print("controller ok")
