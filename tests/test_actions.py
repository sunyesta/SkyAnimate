"""Actions, as in Moho: named alternative timelines of a bone layer and everything inside it.
Editing one swaps its keys in; frame 0 stays the shared setup pose; layers outside keep the
mainline; switching is undoable; actions are saved.

Run with:  SkyAnimate --background --python tests/test_actions.py
"""

import pathlib
import tempfile

import sky

doc = sky.data.document
layers = sky.data.layers
doc.frame_current = 0

bg = layers.active
assert sky.ops.anim.action_new(name="wave") == {'CANCELLED'}, "not in a bone layer"
sky.ops.layer.new_bone()
rig = layers.active
sky.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")


def angle():
    return next(b for b in rig.bones if b["name"] == "arm")["angle"]


def frames():
    return [k["frame"] for k in rig.keyframes('bones["arm"].angle')]


# The mainline: arm turns by frame 10; the background layer moves by frame 10.
doc.frame_current = 10
rig.bone_set("arm", angle=0.5)
layers.active = bg
bg.location_x = 3.0
layers.active = rig
doc.frame_current = 0
assert frames() == [0, 10]

# --- A new action starts from the setup pose ---------------------------------------------------
doc.frame_current = 10
assert sky.ops.anim.action_new(name="wave") == {'FINISHED'}
assert doc.action == "wave"
assert abs(angle()) < 1e-6 and frames() == [0], "a new action shows the setup keys on frame 0"
assert abs(bg.location_x - 3.0) < 1e-6, "layers outside the bone layer keep the mainline"
doc.frame_current = 5
rig.bone_set("arm", angle=1.0)
doc.frame_current = 6
assert frames() == [0, 5]
doc.frame_current = 5
assert abs(angle() - 1.0) < 1e-6

# --- Back to the mainline, and into the action again --------------------------------------------
assert sky.ops.anim.action_set(name="") == {'FINISHED'} and doc.action == ""
assert frames() == [0, 10]
doc.frame_current = 10
assert abs(angle() - 0.5) < 1e-6
doc.action = "wave"  # Python switches too
assert doc.action == "wave" and frames() == [0, 5]

# Frame 0 is the shared setup pose: changing it in the action changes it everywhere.
doc.frame_current = 0
rig.bone_set("arm", angle=0.2)
doc.frame_current = 1
doc.action = ""
doc.frame_current = 0
assert abs(angle() - 0.2) < 1e-6, "the mainline's setup changed too"
doc.frame_current = 10
assert abs(angle() - 0.7) < 1e-6, "its later keys hold the turn from the setup: they turn with it"
doc.frame_current = 0
rig.bone_set("arm", angle=0.0)
doc.frame_current = 10
assert abs(angle() - 0.5) < 1e-6

# A second action, and the names stay unique.
assert sky.ops.anim.action_new(name="wave") == {'FINISHED'} and doc.action == "wave.001"
assert sky.ops.anim.action_set(name="wave") == {'FINISHED'} and frames() == [0, 5]

# Undo takes back the switch.
sky.ops.ed.undo()
assert doc.action == "wave.001"
sky.ops.ed.redo()
assert doc.action == "wave"

# --- Ctrl C / Ctrl V copy keys, even from one action into another --------------------------------
assert doc.action == "wave"
layers.active = rig
assert sky.ops.anim.keys_select(frame_start="5") == {'FINISHED'}
assert sky.wm.event_simulate("C", ctrl=True, area="Timeline")
assert sky.ops.anim.action_set(name="") == {'FINISHED'}
doc.frame_current = 20
assert sky.wm.event_simulate("V", ctrl=True, area="Timeline")
assert frames() == [0, 10, 20], frames()
key = next(k for k in rig.keyframes('bones["arm"].angle') if k["frame"] == 20)
assert abs(key["value"] - 1.0) < 1e-6 and key["selected"], key
assert abs(angle() - 1.0) < 1e-6, "the pasted pose shows at once"
sky.ops.ed.undo()
assert frames() == [0, 10], "undo takes the paste back"

# Pasted on frame 0, the copied key becomes the setup pose (and the keys after it follow on).
doc.frame_current = 0
setup_before = angle()
assert sky.wm.event_simulate("V", ctrl=True, area="Timeline")
assert frames() == [0, 10], frames()
key = next(k for k in rig.keyframes('bones["arm"].angle') if k["frame"] == 0)
# Keys hold the turn from the setup pose: frame 0's is none; the pasted turn went into the setup.
assert abs(key["value"]) < 1e-6 and key["selected"], key
assert abs(angle() - (setup_before + 1.0)) < 1e-6, "the new setup pose shows at once"
sky.ops.ed.undo()
assert abs(angle() - setup_before) < 1e-6, "undo brings the old setup back"
doc.action = "wave"

# --- A selected bone layer's summary has the animated layers inside it ---------------------------
doc.action = ""
doc.frame_current = 0
layers.active = rig
sky.ops.layer.new()
inner = layers.active
sky.ops.layer.move_to_group(group=str(rig.id))
doc.frame_current = 30
inner.location_x = 1.0
doc.frame_current = 0
layers.active = rig
assert sky.ops.anim.keys_select(frame_start="30") == {'FINISHED'}, "the rig's summary reaches the inner layer"
assert sky.ops.anim.keys_offset(frames="2") == {'FINISHED'}
assert 32 in inner.key_frames and 30 not in inner.key_frames, inner.key_frames
assert sky.ops.anim.keyframe_insert() == {'FINISHED'}
assert 0 in inner.key_frames and len(inner.key_frames) == 2, "I keys only the selected layers"
doc.action = "wave"

# --- Deleting an action (right-click it in the action menu) ---------------------------------------
layers.active = rig
assert sky.ops.anim.action_new(name="spare") == {'FINISHED'} and doc.action == "spare"
doc.frame_current = 7
rig.bone_set("arm", angle=0.9)
assert sky.ops.anim.keyframe_insert() == {'FINISHED'}
assert sky.ops.anim.action_delete(name="spare") == {'FINISHED'}
assert doc.action == "", "deleting the action being edited goes back to the mainline"
assert sky.ops.anim.action_set(name="spare") == {'CANCELLED'}, "it's gone"
assert 7 not in frames()
sky.ops.ed.undo()
assert doc.action == "spare" and 7 in frames(), "undo brings it back"
assert sky.ops.anim.action_set(name="wave") == {'FINISHED'}

# --- Saved, in the middle of editing an action ------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_actions_")) / "actions.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
doc = sky.data.document
rig = next(l for l in sky.data.layers if l.type == "BONE")
assert doc.action == "wave" and frames() == [0, 5]
doc.action = ""
assert frames() == [0, 10]

print("Action tests passed")
