"""Path handles are picked on their own: picking one lets go of the keys, dots and other handles (Shift adds
handles only), and picking a key, a dot, a bone or a point lets go of the handles. With handles picked, G R S move,
turn and scale them alone (each about its key), V sets their keys' type, X puts them back to Auto, and the timeline's
key header edits the stretch each shapes.

Run with:  OpenAnimate --background --python tests/test_motion_path_handle_pick.py
"""

import math

import oanim

wm = oanim.wm
ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers


def close(a, b, eps=1e-3):
    return all(abs(x - y) < eps for x, y in zip(a, b))


doc.auto_key = True
ops.layer.new()
box = layers.active
box.add_curve([(0.0, 0.0), (0.5, 0.0)])
doc.motion_paths = True
args = dict(layer=str(box.id))
for frame, x in ((10, 2.0), (20, 4.0), (30, 6.0)):
    assert ops.anim.motion_path_move(frame=str(frame), x=str(x), y="0", **args) == {'FINISHED'}
doc.frame_current = 20
assert ops.wm.tool_motion_path() == {'FINISHED'}


def key(frame):
    return next(k for k in box.keyframes("position") if k["frame"] == frame)


def picks():
    return sorted((p["frame"], p["out"]) for p in wm.motion_path_handle_picks())


def selected_keys():
    return [k["frame"] for k in box.keyframes("position") if k["selected"]]


# --- A handle picked lets go of the keys and dots; Shift adds handles only -------------------------------------------
assert ops.anim.motion_path_select(frame="20", **args) == {'FINISHED'}
assert ops.anim.motion_path_select(frame="15", extend="1", **args) == {'FINISHED'}
assert selected_keys() == [20] and len(wm.motion_path_dots()) == 1
assert ops.anim.motion_path_handle_select(frame="20", out="1", **args) == {'FINISHED'}
assert picks() == [(20, True)], picks()
assert selected_keys() == [], ("the key let go", selected_keys())
assert wm.motion_path_dots() == [], "and the dot"
assert ops.anim.motion_path_handle_select(frame="10", out="0", extend="1", **args) == {'FINISHED'}
assert picks() == [(10, False), (20, True)], picks()
assert ops.anim.motion_path_handle_select(frame="10", out="0", extend="1", **args) == {'FINISHED'}
assert picks() == [(20, True)], "Shift on a picked one takes it out"
assert ops.anim.motion_path_handle_select(frame="10", out="1", **args) == {'FINISHED'}
assert picks() == [(10, True)], "a plain pick: that one alone"

# --- A key or dot picked lets go of the handles, Shift or not -----------------------------------------------------------
assert ops.anim.motion_path_select(frame="10", extend="1", **args) == {'FINISHED'}
assert picks() == [], "a key added with Shift drops the handles"
assert ops.anim.motion_path_handle_select(frame="20", out="1", **args) == {'FINISHED'}
assert ops.anim.motion_path_select(frame="25", **args) == {'FINISHED'}
assert picks() == [], "a dot drops them"
assert ops.anim.motion_path_handle_select(frame="20", out="1", **args) == {'FINISHED'}
wm.motion_path_pick(4.0, 0.0, apply=True)
assert picks() == [], "a bone or point picked (or a click on nothing) drops them"

# --- G R S on a picked handle: it alone, about its key -----------------------------------------------------------------
assert ops.anim.motion_path_handle_select(frame="20", out="1", **args) == {'FINISHED'}
ops.anim.motion_path_select(frame="10", extend="1", **args)
assert ops.anim.motion_path_handle_select(frame="20", out="1", **args) == {'FINISHED'}
k10 = key(10)
out0 = key(20)["path_out"]
assert ops.anim.motion_path_keys_grab.poll(), "G works with only a handle picked"
assert ops.anim.motion_path_keys_transform(kind="G", x="0", y="1") == {'FINISHED'}
k = key(20)
assert close(k["value"], (4.0, 0.0)), ("the key stays", k)
assert close(k["path_out"], (out0[0], out0[1] + 1.0)), ("the tip moved", out0, k)
assert key(10) == k10, "nothing else moved"
out1 = k["path_out"]
assert ops.anim.motion_path_keys_transform(kind="R", angle=str(math.pi / 2)) == {'FINISHED'}
out2 = key(20)["path_out"]
assert close(out2, (-out1[1], out1[0])), ("turned about its key", out1, out2)
assert ops.anim.motion_path_keys_transform(kind="S", factor="2") == {'FINISHED'}
assert close(key(20)["path_out"], (out2[0] * 2, out2[1] * 2)), "scaled about its key"
ops.ed.undo()
assert close(key(20)["path_out"], out2), "each one undo step"

# --- V and X: the picked handles' keys ---------------------------------------------------------------------------------
assert ops.anim.motion_path_handles_type(mode="BROKEN") == {'FINISHED'}
assert key(20)["path_handles"] == "BROKEN" and key(10)["path_handles"] == "AUTO", "only the picked handle's key"
assert ops.anim.motion_path_handles_reset.poll()
assert ops.anim.motion_path_handles_reset() == {'FINISHED'}
assert key(20)["path_handles"] == "AUTO"
assert [f for f in (10, 20, 30) if any(k["frame"] == f for k in box.keyframes("position"))] == [10, 20, 30], \
    "X reset the handles, deleting no key"
assert ops.anim.motion_path_handles_type(mode="BROKEN") == {'FINISHED'}
assert oanim.wm.event_simulate("X", area="Viewport")
assert key(20)["path_handles"] == "AUTO", "X with a handle picked: its key's handles back to Auto"
assert len(box.keyframes("position")) >= 4, "and no key deleted"
ops.anim.motion_path_handle_select(none="1")
assert not ops.anim.motion_path_handles_reset.poll(), "no handles picked: X is Delete Keys again"

# --- The key header: the stretch the handle shapes ---------------------------------------------------------------------
assert ops.anim.motion_path_handle_select(frame="20", out="1", **args) == {'FINISHED'}
h = wm.timeline_key_header()
assert h["mode"] == "PATH" and h["picked"] and tuple(h["span"]) == (20, 30), ("its stretch, teal", h)
assert wm.timeline_key_header_call("anim.keys_interpolation_set", interp="STEP")
assert key(30)["interp"] == "STEP" and key(20)["interp"] != "STEP", "an out handle: the stretch to the next key"
assert ops.anim.motion_path_handle_select(frame="20", out="0", **args) == {'FINISHED'}
assert wm.timeline_key_header_call("anim.keys_interpolation_set", interp="LINEAR")
assert key(20)["interp"] == "LINEAR", "an in handle: the stretch its key ends"
assert selected_keys() == [], "the header leaves no key selected"
assert picks() == [(20, False)], "and the handle picked"

print("Path handle pick tests passed")
