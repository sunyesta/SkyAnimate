"""What editing a path costs. Where its control points and handles are is found by working whole frames of the document
out on copies (places): a control point is two places (where it is, and the path on its dot's frame), its handles'
tips two more. Moving a control point works out where the control points are, not their tips; G on picked control
points the same; moving one's handle the tips of that one alone. The places are shared out over the CPU's cores.

Run with:  OpenAnimate --background --python tests/test_motion_path_places.py
"""

import os

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm

ops.wm.read_homefile()
doc.auto_key = False
doc.motion_paths = True
doc.path_before, doc.path_after = 40, 40
assert ops.wm.tool_motion_path() == {'FINISHED'}

ops.layer.new()
ball = layers.active
ball.add_curve([(0.0, 0.0), (0.1, 0.0)])
ball.key_whole_drawing = False
doc.frame_current = 0
ball.location_x, ball.location_y = -5.0, -5.0
for other in layers:
    other.selected = other.id == ball.id
layers.active = ball
args = dict(layer=str(ball.id))
for f, (x, y) in ((10, (0.0, 0.0)), (30, (4.0, 0.0))):
    assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), **args) == {'FINISHED'}
    ball.keyframe_set("position", f, interp="LINEAR")
for f in (14, 18, 22, 26):
    assert ops.anim.motion_path_control_add(frame=str(f), **args) == {'FINISHED'}
controls = [c for c in wm.motion_path_controls() if c["end"] == 30]
n = len(controls)
assert n == 4, controls

# --- All of them with their tips: four places each, shared out over the cores -----------------------------------
wm.motion_path_places_clear()
wm.motion_path_controls()
used = wm.motion_path_places()
assert used["places"] == 4 * n, used
if (os.cpu_count() or 1) >= 4:
    assert used["threads"] >= 2, ("side by side", used)

# --- Moving one: where they are, not their tips (the one moved worked out once more as it goes there) -------------
c = controls[1]
wm.motion_path_places_clear()
assert ops.anim.motion_path_control_move(frame="30", index="1", x=str(c["at"][0]), y=str(c["at"][1] + 0.5),
                                         **args) == {'FINISHED'}
moved = wm.motion_path_places()["places"]
assert moved < 4 * n, ("moving one works out no tips (every tip would be four places each)", moved)
c = next(c for c in wm.motion_path_controls() if c["end"] == 30 and c["index"] == 1)
assert abs(c["at"][1] - (controls[1]["at"][1] + 0.5)) < 1e-3, ("it went there", c, controls[1])

# --- Its handle: the tips of that one alone ----------------------------------------------------------------------
wm.motion_path_places_clear()
assert ops.anim.motion_path_control_handle_move(frame="30", index="1", out="1", x=str(c["out_tip"][0] + 0.2),
                                                y=str(c["out_tip"][1]), **args) == {'FINISHED'}
handle = wm.motion_path_places()["places"]
assert handle < 4 * n, ("that one's tips alone", handle)

# --- G on two picked control points: where they are, no tips -----------------------------------------------------
assert ops.anim.motion_path_handle_select(frame="30", control="0", **args) == {'FINISHED'}
assert ops.anim.motion_path_handle_select(frame="30", control="2", extend="1", **args) == {'FINISHED'}
assert sum(1 for p in wm.motion_path_handle_picks() if p["control"] >= 0) == 2, wm.motion_path_handle_picks()
wm.motion_path_places_clear()
assert ops.anim.motion_path_keys_transform(kind="G", x="0", y="0.25") == {'FINISHED'}
moved_two = wm.motion_path_places()["places"]
assert moved_two < 2 * 4 * n, ("two moved, no tips", moved_two)
after = {c["index"]: c for c in wm.motion_path_controls() if c["end"] == 30}
assert all(abs(after[i]["at"][1] - before_y - 0.25) < 1e-3 for i, before_y in
           ((0, controls[0]["at"][1]), (2, controls[2]["at"][1]))), (after, controls)

print("Motion path places tests passed")
