"""Smart actions, as Moho's smart bones: an action named after a bone. Its keyed angle maps the
bone's angle anywhere else to a frame of the action, whose changes from frame 0 are added on top.
A " 2" action covers turning the other way. The added amounts never become data: saving, undo
and keying keep the plain values.

Run with:  SkyAnimate --background --python tests/test_smart_actions.py
"""

import pathlib
import tempfile

import sky

doc = sky.data.document
layers = sky.data.layers
doc.frame_current = 0

sky.ops.layer.new_bone()
rig = layers.active
sky.ops.armature.bone_add(name="dial", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
sky.ops.layer.new()
face = layers.active
sky.ops.layer.move_to_group(group=str(rig.id))
layers.active = rig
rig.select_bone("dial")


def dial(angle):
    rig.bone_set("dial", angle=angle)
    doc.frame_current = doc.frame_current + 1  # record and evaluate
    doc.frame_current = doc.frame_current - 1


# --- Make the smart action: the dial turns to 1.0 by frame 10, moving the face to x = 2 ----------
assert sky.ops.anim.action_new(name="dial", smart="1") == {'FINISHED'} and doc.action == "dial"
doc.frame_current = 10
rig.bone_set("dial", angle=1.0)
face.location_x = 2.0
doc.frame_current = 0
assert sky.ops.anim.action_set(name="") == {'FINISHED'}

# --- Turning the dial anywhere plays the action that far -----------------------------------------
# Even before the dial has any key on the mainline: it works at once, while turning.
doc.frame_current = 20
assert rig.keyframes('bones["dial"].angle') == []
rig.bone_set("dial", angle=0.5)
doc.frame_current = doc.frame_current  # a sync, as the app does every frame
assert abs(face.location_x - 1.0) < 1e-3, ("unkeyed bone", face.location_x)
sky.ops.armature.select_none()  # an undo step records (keys) the turn; still applied
assert abs(face.location_x - 1.0) < 1e-3, ("after recording", face.location_x)
sky.ops.armature.select_all()
dial(0.5)
assert abs(face.location_x - 1.0) < 1e-3, face.location_x
dial(1.0)
assert abs(face.location_x - 2.0) < 1e-3, face.location_x
dial(3.0)
assert abs(face.location_x - 2.0) < 1e-3, "past the end it holds the last frame"
dial(-0.5)
assert abs(face.location_x) < 1e-3, "the other way: nothing (that's the \" 2\" action's job)"

# Turning the bone without changing frame (as dragging it does) applies at once.
rig.bone_set("dial", angle=0.25)
doc.frame_current = doc.frame_current  # a sync, as the app does every frame
assert abs(face.location_x - 0.5) < 1e-3, face.location_x
rig.bone_set("dial", angle=0.75)
doc.frame_current = doc.frame_current
assert abs(face.location_x - 1.5) < 1e-3, face.location_x

# The added amount isn't data: the face's own position is still 0.
dial(0.5)
assert face.keyframes("position") == [], "no keys from smart actions"
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_smart_")) / "smart.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
doc = sky.data.document
rig = next(l for l in sky.data.layers if l.type == "BONE")
face = rig.children[0]
assert abs(face.location_x - 1.0) < 1e-3, "worked out again on opening, not saved in"
doc.frame_current = 0
assert abs(face.location_x) < 1e-6, "the setup pose is untouched"
doc.frame_current = 20

# Undo doesn't add it twice.
sky.data.layers.active = rig
sky.ops.anim.keys_select_all()
sky.ops.ed.undo()
assert abs(face.location_x - 1.0) < 1e-3, face.location_x

# Keying the face while the dial is turned keys its own value, not the smart part.
sky.data.layers.active = face
face.location_y = 0.5
doc.frame_current = 21
doc.frame_current = 20
keys = face.keyframes("position")
assert keys and abs(keys[-1]["value"][0]) < 1e-4, keys
assert abs(face.location_x - 1.0) < 1e-3

# --- " 2": the other direction ------------------------------------------------------------------
sky.data.layers.active = rig
doc.frame_current = 0
assert sky.ops.anim.action_new(name="dial", smart="1") == {'FINISHED'} and doc.action == "dial 2"
doc.frame_current = 10
rig.bone_set("dial", angle=-1.0)
face.location_x = -4.0
doc.frame_current = 0
sky.ops.anim.action_set(name="")
doc.frame_current = 30
dial(-0.5)
assert abs(face.location_x + 2.0) < 1e-3, face.location_x
assert sky.ops.anim.action_new(name="dial", smart="1") == {'CANCELLED'}, "both directions exist"

# --- A bone the action moves, with no keys of its own, moves its points too --------------------------
doc.frame_current = 0
layers.active = rig
sky.ops.armature.bone_add(name="jaw", head_x="0", head_y="-1", tail_x="1", tail_y="-1", parent="")
sky.ops.layer.new()
lip = layers.active
sky.ops.layer.move_to_group(group=str(rig.id))
lip.add_curve([(0.5, -1.0), (0.5, -1.5)])
sky.ops.mesh.select_all()
rig.select_bone("jaw")
assert sky.ops.weight.assign(weight="1") == {'FINISHED'}
layers.active = rig
assert sky.ops.anim.action_set(name="dial") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("jaw", position=(0.0, -2.0))
doc.frame_current = 0
assert sky.ops.anim.action_set(name="") == {'FINISHED'}
assert rig.keyframes('bones["jaw"].position') == [], "no mainline keys: only the action moves it"
doc.frame_current = 40
dial(1.0)
jaw = next(b for b in rig.bones if b["name"] == "jaw")
assert abs(jaw["head"][1] + 2.0) < 1e-3, jaw["head"]
assert abs(lip.evaluated()[0][0][1] + 2.0) < 1e-3, ("its point follows", lip.evaluated()[0])
# Dragging that bone moves it at once, even the first time, before it has keys: the smart action's
# offset must not take in the move.
rig.select_bone("jaw")
assert rig.move_bone_ends((0.3, 0.0))
doc.frame_current = doc.frame_current  # a sync, as the app does every frame while dragging
jaw = next(b for b in rig.bones if b["name"] == "jaw")
assert abs(jaw["head"][0] - 0.3) < 1e-3 and abs(jaw["head"][1] + 2.0) < 1e-3, ("first move", jaw["head"])
doc.frame_current = 41  # records the move
doc.frame_current = 40
jaw = next(b for b in rig.bones if b["name"] == "jaw")
assert abs(jaw["head"][0] - 0.3) < 1e-3 and abs(jaw["head"][1] + 2.0) < 1e-3, ("kept", jaw["head"])
# On the setup frame a smart bone plays nothing: it is the rig at rest.
doc.frame_current = 0
rig.bone_set("dial", angle=1.0)
doc.frame_current = doc.frame_current
jaw = next(b for b in rig.bones if b["name"] == "jaw")
assert abs(jaw["head"][1] + 1.0) < 1e-3, ("frame 0: the jaw stays at rest", jaw["head"])
rig.bone_set("dial", angle=0.0)

# --- Dragged past its last frame, a smart bone holds that frame --------------------------------------
# A half-turn action (as a mouth's open bone, pointing up and turned down): one drag of the tip round
# past the end keeps turning that way, so the action holds its end instead of flipping to the start.
import math
sky.ops.armature.bone_add(name="knob", head_x="0", head_y="3", tail_x="1", tail_y="3", parent="")
sky.ops.layer.new()
slider = layers.active
sky.ops.layer.move_to_group(group=str(rig.id))
layers.active = rig
rig.select_bone("knob")
assert sky.ops.anim.action_new(name="knob", smart="1") == {'FINISHED'} and doc.action == "knob"
doc.frame_current = 10
rig.bone_set("knob", angle=-math.pi)
slider.location_x = 3.0
doc.frame_current = 0
assert sky.ops.anim.action_set(name="") == {'FINISHED'}
doc.frame_current = 60
layers.active = rig
sky.ops.armature.select_none()
rig.select_bone("knob", part="TAIL")
steps = [math.radians(-d) for d in range(0, 235, 15)]  # clockwise, past -180 to -225 degrees
assert rig.drag_bone_ends([(math.cos(a) - 1.0, math.sin(a)) for a in steps])
doc.frame_current = doc.frame_current
knob = next(b for b in rig.bones if b["name"] == "knob")
assert abs(knob["angle"] - steps[-1]) < 1e-3, ("kept turning the same way", knob["angle"])
assert abs(slider.location_x - 3.0) < 1e-3, ("holds the last frame", slider.location_x)

# --- R past the action's end, then back the other way: the bones it drives come back -----------------
# A drag puts the bones back as they were when it began every update, their smart offsets too; the
# replay takes those off first, so turning back past the start doesn't keep the end pose. Alone in a
# new file: with another smart action playing, its offsets were taken off anyway.
import _sky

assert sky.ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
doc = sky.data.document
doc.frame_current = 0
sky.ops.layer.new_bone()
rig = sky.data.layers.active
sky.ops.armature.bone_add(name="lever", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
sky.ops.armature.bone_add(name="chin", head_x="0", head_y="-2", tail_x="1", tail_y="-2", parent="")
sky.ops.armature.select_none()
rig.select_bone("lever")
assert sky.ops.anim.action_new(name="lever", smart="1") == {'FINISHED'} and doc.action == "lever"
doc.frame_current = 10
rig.bone_set("lever", angle=1.0)
rig.bone_set("chin", position=(0.0, -2.5))
doc.frame_current = 0
assert sky.ops.anim.action_set(name="") == {'FINISHED'}
doc.frame_current = 40
sky.ops.armature.select_none()
rig.select_bone("lever")


def chin_y():
    return next(b for b in rig.bones if b["name"] == "chin")["position"][1]


assert abs(chin_y() + 2.0) < 1e-4, chin_y()
_sky.debug_rotate_bones(rig.id, [20, 45, 90, 150])  # past the end (1.0 rad, 57 degrees)
assert abs(chin_y() + 2.5) < 1e-4, ("the action's end", chin_y())
_sky.debug_rotate_bones(rig.id, [-30, -150, -170])  # back past the start, the other way
assert abs(chin_y() + 2.0) < 1e-4, ("clamped at the start, not left at the end", chin_y())

print("Smart action tests passed")
