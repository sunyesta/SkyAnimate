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

print("Motion path pick tests passed")
