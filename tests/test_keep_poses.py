"""Keep Poses (the ViewportActionPanel's checkbox for moves on the setup frame): the move changes only
the setup pose; every key on another frame, in the mainline and every action, is worked out again so
it shows the same pose. Points, their handles and bones.

Run with:  OpenAnimate --background --python tests/test_keep_poses.py
"""

import math

import oanim

layers = oanim.data.layers
doc = oanim.data.document
doc.frame_current = 0


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


# A rig: a bone, and a line inside it.
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
oanim.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
oanim.ops.layer.new()
art = layers.active
art.add_curve([(2.0, 0.0), (3.0, 1.0), (4.0, 0.0)])
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
art = next(l for l in layers.all() if l.id == art.id)
doc.frame_current = 1
doc.frame_current = 0


def point(i):
    return tuple(art.points[i][:2])


def handles(i):
    return art.curve_handles(0)[i][1:]


def arm():
    b = next(b for b in rig.bones if b["name"] == "arm")
    return tuple(b["position"]) + (b["angle"],)


# Poses: frame 10 in the mainline, frame 5 in an action.
layers.active = art
doc.frame_current = 10
art.select_points([1])
oanim.ops.mesh.translate(move_x="0.5", move_y="0.25")
oanim.ops.mesh.rotate(angle="40")  # turns point 1's handles too
assert oanim.ops.anim.keyframe_insert() == {'FINISHED'}
layers.active = rig
rig.bone_set("arm", angle=0.5, position=(0.4, -0.3))
doc.frame_current = 11
doc.frame_current = 10
posed_point, posed_handles, posed_arm = point(1), handles(1), arm()
assert oanim.ops.anim.action_new(name="wave") == {'FINISHED'}
doc.frame_current = 5
rig.bone_set("arm", angle=1.0)
doc.frame_current = 6
doc.frame_current = 5
action_arm = arm()
assert oanim.ops.anim.action_set(name="") == {'FINISHED'}

# --- Points on the setup frame, Keep Poses on: the other frames stay -------------------------------
doc.frame_current = 0
layers.active = art
oanim.ops.mesh.select_all()  # an undo step, for the panel's redo to start from
setup_before = point(1)
assert oanim.ops.mesh.translate(move_x="0", move_y="1", setup="1", keep_poses="1") == {'FINISHED'}
assert close(point(1), (setup_before[0], setup_before[1] + 1)), "the setup moved"
last = oanim.wm.last_operation()
assert last[0] == "mesh.translate" and last[1]["keep_poses"] == "1" and last[1]["setup"] == "1", last
# Off in the panel: the same move, and the poses move with the setup as before.
assert oanim.wm.redo_last(move_x="0", move_y="1", setup="1", keep_poses="0") == "FINISHED"
doc.frame_current = 10
assert close(point(1), (posed_point[0], posed_point[1] + 1)), "keys hold changes: they move with it"
doc.frame_current = 0
oanim.ops.ed.undo()
assert close(point(1), setup_before)
# On again: frame 10's pose stays.
assert oanim.ops.mesh.translate(move_x="0", move_y="1", setup="1", keep_poses="1") == {'FINISHED'}
doc.frame_current = 10
assert close(point(1), posed_point), ("frame 10's pose stays", point(1), posed_point)
doc.frame_current = 0

# Turned on the setup frame: the handles keyed on frame 10 keep their shape too.
art.select_points(range(art.point_count), False)
art.select_points([1])
assert oanim.ops.mesh.rotate(angle="30", setup="1", keep_poses="1") == {'FINISHED'}
doc.frame_current = 10
got = handles(1)
assert close(got[0], posed_handles[0], 1e-3) and close(got[1], posed_handles[1], 1e-3), (got, posed_handles)
doc.frame_current = 0

# --- Bones: moved and turned on the setup frame, Keep Poses on ------------------------------------
layers.active = rig
rig.select_bone("arm")
assert oanim.ops.armature.translate(move_x="0.3", move_y="0.2", posing="0", keep_poses="1") == {'FINISHED'}
assert oanim.ops.armature.rotate(angle="15", posing="0", keep_poses="1") == {'FINISHED'}
assert oanim.wm.last_operation()[1]["keep_poses"] == "1"
# (A channel with no keys has no pose of its own to keep: it follows the setup, as an unkeyed point does.)
doc.frame_current = 10
assert close(arm(), posed_arm), ("the mainline's pose stays", arm(), posed_arm)
doc.action = "wave"
doc.frame_current = 5
assert abs(arm()[2] - action_arm[2]) < 1e-4, ("and the action's angle, keyed there", arm(), action_arm)
doc.action = ""

# Off: the old way, the poses follow the setup.
doc.frame_current = 0
assert oanim.ops.armature.rotate(angle="15", posing="0", keep_poses="0") == {'FINISHED'}
doc.frame_current = 10
assert abs(arm()[2] - (posed_arm[2] + math.radians(15))) < 1e-4, (arm(), posed_arm)

print("keep_poses: ok")
