"""Frame 0's key on a motion path: the setup pose, so it never moves, but it is picked for its out handle (how the path
leaves the setup), and each action has its own, kept as the action is put away and brought back, saved and
renamed."""

import os
import tempfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def key0(rig):
    return next(k for k in rig.keyframes('bones["arm_L"].position') if k["frame"] == 0)


ops.wm.read_homefile()
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
assert ops.armature.bone_add(name="arm_L", head_x="1", head_y="0", tail_x="2", tail_y="0", parent="") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("arm_L", position=(1.0, 1.0))
doc.frame_current = 5
rig.select_bone("arm_L")
doc.motion_paths = True
ops.wm.tool_motion_path()
head = dict(layer=str(rig.id), bone="arm_L", head="1")

# --- Picked, but locked --------------------------------------------------------------------------------------------
assert ops.anim.motion_path_select(frame="0", **head) == {'FINISHED'}
assert key0(rig)["selected"], "frame 0's key picked"
assert [k for k in oanim.wm.motion_path_dots()] == [], "a key, not a dot"
ops.anim.motion_path_keys_delete()
assert key0(rig)["frame"] == 0, "the setup's key isn't deleted"

# --- Its out handle ------------------------------------------------------------------------------------------------
assert ops.anim.motion_path_handle(frame="0", side="OUT", x="1.5", y="0", **head) == {'FINISHED'}
k = key0(rig)
assert k["path_handles"] == "ALIGNED" and close(k["path_out"], (0.5, 0.0)), k
mainline_out = k["path_out"]

# --- Each action its own ------------------------------------------------------------------------------------------
assert ops.anim.action_new(name="wave_L") == {'FINISHED'}
doc.frame_current = 8
rig.bone_set("arm_L", position=(1.0, -1.0))
doc.frame_current = 4
assert key0(rig)["path_handles"] == "AUTO", "a new action starts with Auto"
assert ops.anim.motion_path_handle(frame="0", side="OUT", x="1", y="-0.5", **head) == {'FINISHED'}
assert close(key0(rig)["path_out"], (0.0, -0.5))
assert ops.anim.action_set(name="") == {'FINISHED'}
assert close(key0(rig)["path_out"], mainline_out), "the mainline keeps its own"
assert ops.anim.action_set(name="wave_L") == {'FINISHED'}
k = key0(rig)
assert k["path_handles"] == "ALIGNED" and close(k["path_out"], (0.0, -0.5)), ("the action keeps its own", k)

# --- Renamed and saved ------------------------------------------------------------------------------------------
assert ops.anim.action_set(name="") == {'FINISHED'}
assert ops.anim.action_rename(name="wave_L", to="reach_L") == {'FINISHED'}
folder = tempfile.mkdtemp()
path = os.path.join(folder, "setup_handle.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
rig = next(l for l in oanim.data.layers if l.name == rig.name)
assert close(key0(rig)["path_out"], mainline_out)
assert ops.anim.action_set(name="reach_L") == {'FINISHED'}
k = key0(rig)
assert k["path_handles"] == "ALIGNED" and close(k["path_out"], (0.0, -0.5)), ("renamed and saved", k)

print("Motion path setup handle tests passed")
