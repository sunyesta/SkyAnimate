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

print("Smart action tests passed")
