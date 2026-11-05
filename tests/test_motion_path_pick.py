"""The Motion Path tool's Edit brush: a plain click (no Alt) picks a bone or point, so its path shows. A path
handle, key or dot wins; on the dot of the frame shown, over the bone or point it traces, a click picks and a drag
moves the path; empty space clears only the path selection. Shift adds. Alt keeps its meanings.

Run with:  OpenAnimate --background --python tests/test_motion_path_pick.py
"""

import oanim

wm = oanim.wm
ops = oanim.ops
layers = oanim.data.layers

# --- What a press does, from what is under it -----------------------------------------------------------------------
kind = wm.motion_path_press_kind
assert kind(on_handle=True, on_path=True, thing_under=True) == "HANDLE", "a handle wins"
assert kind(on_path=True, thing_under=True) == "PATH", "a dot on another frame wins over a bone under it"
assert kind(on_path=True) == "PATH"
assert kind(on_path=True, on_frame_shown=True) == "PATH", "the frame shown, nothing under: the path"
assert kind(on_path=True, on_frame_shown=True, thing_under=True) == "PATH_OR_PICK", \
    "the dot of the frame shown on its bone: a click picks, a drag moves"
assert kind(on_path=True, on_frame_shown=True, thing_under=True, alt=True) == "PATH", "Alt: the path's (a key)"
assert kind(thing_under=True) == "PICK", "off the paths, a bone or point is picked without Alt"
assert kind() == "DESELECT", "empty space: the path selection clears"

# --- The pick: bones -------------------------------------------------------------------------------------------------
assert ops.wm.tool_motion_path() == {'FINISHED'}
ops.layer.new_bone()
rig = layers.active
assert ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="2", tail_y="0", parent="") == {'FINISHED'}
assert ops.armature.bone_add(name="leg", head_x="0", head_y="3", tail_x="2", tail_y="3", parent="") == {'FINISHED'}


def picked():
    return sorted(b["name"] for b in rig.bones if b["selected"])


rig.select_bone("arm")
assert picked() == ["arm"]
assert wm.motion_path_pick(1.0, 3.0, apply=False), "a bone is there"
assert picked() == ["arm"], "only asking picks nothing"
assert wm.motion_path_pick(1.0, 3.0), "a click on the other bone"
assert picked() == ["leg"], ("picked alone", picked())
assert wm.motion_path_pick(1.0, 0.0, extend=True)
assert picked() == ["arm", "leg"], "Shift adds"
assert not wm.motion_path_pick(6.0, 6.0), "nothing there"
assert picked() == ["arm", "leg"], "empty space leaves the bones picked"

# --- The pick: points of the drawing being edited --------------------------------------------------------------------
ops.layer.new()
art = layers.active
art.add_curve([(5.0, 5.0), (7.0, 5.0)])
ops.mesh.select_none()
assert wm.motion_path_pick(7.0, 5.0), "a point"
assert [i for i, p in enumerate(art.points) if p[2]] == [1], "picked"


# --- The drawing edited off the paths, as with the Select tool: handles and lines count too ------------------------
ops.mesh.select_none()
art.add_curve([(10.0, 0.0), (12.0, 2.0), (14.0, 0.0)])
mid = len(art.points) - 2
curve = next(i for i in range(8) if len(art.curve_handles(i)) == 3)
_, hin, hout = art.curve_handles(curve)[1]
far = max((hin, hout), key=lambda h: (h[0] - 12.0) ** 2 + (h[1] - 2.0) ** 2)
assert (far[0] - 12.0) ** 2 + (far[1] - 2.0) ** 2 > 0.3 ** 2, ("a handle well away from its point", far)
art.add_curve([(20.0, 0.0), (22.0, 0.0)])
art.select_points([mid])
assert wm.motion_path_vector_under(*far), "a shown handle"
assert not wm.motion_path_vector_under(21.0, 0.0), "a line in Points mode: not picked"
assert ops.mesh.select_mode_edge() == {'FINISHED'}
assert wm.motion_path_vector_under(21.0, 0.0), "a line in Edges mode"
assert ops.mesh.select_mode_point() == {'FINISHED'}
assert wm.motion_path_pick(*far), "a click on the handle"
assert not [i for i, p in enumerate(art.points) if p[2]], "the handle alone picked, as the Select tool's click"

# --- While something moves the dots' ghost waits, until the mouse moves on after ---------------------------------
quiet = wm.path_hover_quiet_update
assert not quiet(False, 0, 0)
assert quiet(True, 50, 50), "moving: no ghost"
assert quiet(False, 50, 50), "the move ended where the mouse is: still none"
assert quiet(False, 51, 50), "a pixel's tremble: still none"
assert not quiet(False, 60, 50), "the mouse moved on: the ghost is back"

print("Motion path pick tests passed")
