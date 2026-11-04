"""The Motion Path tool (stage 2): a path's dots edited, every edit landing on keys. Clicking a dot goes to its
frame and selects its key (between keys, the key ending that stretch); moving a dot keys it there, whatever
auto-key says (a bone's tip turns the bone); Ctrl puts it on the line between the keys either side; Shift between
keys makes a breakdown along the arc; Smooth Arc evens out a lumpy path. The tool runs these operators.

Run with:  OpenAnimate --background --python tests/test_motion_path_tool.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return all(abs(x - y) < eps for x, y in zip(a, b))
    return abs(a - b) < eps


def path(label):
    return next(p for p in oanim.wm.motion_paths() if p["label"] == label)


def at(p, frame):
    return p["points"][frame - p["first"]]


def key_frames(layer, channel):
    return [k["frame"] for k in layer.keyframes(channel) if k["frame"] > 0]


# --- The tool -------------------------------------------------------------------------------------------------
assert ops.wm.tool_motion_path() == {'FINISHED'} and oanim.wm.tool() == "MOTION_PATH"

# --- A layer's origin: a key moved, an in-between keyed even with auto-key off ------------------------------------
doc.auto_key = True
ops.layer.new()
box = layers.active
box.add_curve([(0.0, 0.0), (0.5, 0.0)])
for frame, x in ((10, 1.0), (20, 2.0)):
    doc.frame_current = frame
    box.location_x = x
doc.frame_current = 15
doc.motion_paths = True
args = dict(layer=str(box.id))
assert ops.anim.motion_path_move(frame="20", x="2.0", y="1.0", **args) == {'FINISHED'}
def key_value(layer, channel, frame):
    return next((k["value"] for k in layer.keyframes(channel) if k["frame"] == frame), None)


assert doc.frame_current == 15, "the frame shown stays"
assert close(key_value(box, "position", 20), (2.0, 1.0)), "the key moved"
assert key_frames(box, "position") == [10, 20]
ops.ed.undo()
undone = key_value(box, "position", 20)
assert undone is None or close(undone[1], 0.0), "one undo step"
ops.ed.redo()

doc.auto_key = False
assert ops.anim.motion_path_move(frame="15", x="1.5", y="2.0", **args) == {'FINISHED'}
assert key_frames(box, "position") == [10, 15, 20], "an in-between dot gets a key"
assert not doc.auto_key, "auto-key left as it was"
doc.frame_current = 12
assert close(at(path(box.name), 15), (1.5, 2.0)) and not close(at(path(box.name), 12)[1], 0.0), "the path bends"
doc.auto_key = True

# Ctrl: onto the straight line between the keys either side (here 15 (1.5, 2) and 20 (2, 1)), nearest the mouse:
# (1, 1) is 0.6 of the way along.
assert ops.anim.motion_path_move(frame="17", x="1", y="1", straight="1", **args) == {'FINISHED'}
assert close(key_value(box, "position", 17), (1.8, 1.4)), key_value(box, "position", 17)
assert ops.anim.motion_path_move(frame="5", x="0", y="0", straight="1", **args) == {'CANCELLED'}, "needs two keys"

# --- A bone: dragging its tip turns it; its head moves it ----------------------------------------------------------
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
assert ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="2", tail_y="0", parent="") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("arm", angle=0.2)
doc.frame_current = 20
rig.bone_set("arm", angle=1.0)
rig.select_bone("arm")
bone_args = dict(layer=str(rig.id), bone="arm")
assert ops.anim.motion_path_move(frame="20", x="0", y="5", **bone_args) == {'FINISHED'}
arm = next(b for b in rig.bones if b["name"] == "arm")
assert close(arm["angle"], math.pi / 2) and close(arm["position"], (0.0, 0.0)), arm
doc.frame_current = 15
assert close(at(path("arm tip"), 20), (0.0, 2.0)), "the tip turned onto the mouse's line"

ops.anim.motion_path_style(head="1", **bone_args)
assert ops.anim.motion_path_move(frame="20", x="0.5", y="0.25", **bone_args) == {'FINISHED'}
head = key_value(rig, 'bones["arm"].position', 20)
assert close(head, (0.5, 0.25)), ("the head moves the bone", head)
ops.ed.undo()
ops.anim.motion_path_style(head="0", **bone_args)

# --- Shift between keys: a breakdown, staying on the arc -----------------------------------------------------------
assert ops.anim.motion_path_breakdown(frame="12", factor="0.25", **bone_args) == {'FINISHED'}
angle = key_value(rig, 'bones["arm"].angle', 12)
assert close(angle, 0.2 + 0.25 * (math.pi / 2 - 0.2)), angle
assert 12 in key_frames(rig, 'bones["arm"].angle')
doc.frame_current = 15
assert close(math.hypot(*at(path("arm tip"), 12)), 2.0), "on the arc"
assert ops.anim.motion_path_breakdown(frame="25", factor="0.5", **bone_args) == {'CANCELLED'}, "past the last key"

# --- Clicking: a key selects that key, a dot between keys that dot; go=1 (double click) also goes to its frame ------
def selected(channel):
    return [k["frame"] for k in rig.keyframes(channel) if k["selected"]]


doc.frame_current = 5
assert ops.anim.motion_path_select(frame="15", **bone_args) == {'FINISHED'}
assert doc.frame_current == 5, "a plain click keeps the frame"
assert selected('bones["arm"].angle') == [] and [d["frame"] for d in oanim.wm.motion_path_dots()] == [15]
assert ops.anim.motion_path_select(frame="15", go="1", **bone_args) == {'FINISHED'}
assert doc.frame_current == 15 and [d["frame"] for d in oanim.wm.motion_path_dots()] == [15]
assert ops.anim.motion_path_select(frame="12", **bone_args) == {'FINISHED'}
assert oanim.wm.motion_path_dots() == [], "a key: the dots let go"
assert selected('bones["arm"].angle') == [12], "on a key: that key"
assert ops.anim.keys_interpolation_set(interp="LINEAR") == {'FINISHED'}
assert [k["interp"] for k in rig.keyframes('bones["arm"].angle') if k["frame"] == 12] == ["LINEAR"]

# --- Smooth Arc: a lumpy key back onto the curve through its neighbours ----------------------------------------------
ops.layer.new()
ball = layers.active
ball.add_curve([(0.0, 0.0), (0.1, 0.0)])
doc.auto_key = True


def arc_y(f):
    return 2.0 - (f - 15) ** 2 / 50.0


for f in (5, 10, 15, 20, 25):
    doc.frame_current = f
    ball.location_x = f / 10.0
    ball.location_y = arc_y(f) + (1.0 if f == 15 else 0.0)
for l in layers:
    l.selected = l.name == ball.name
layers.active = ball
doc.frame_current = 15
doc.path_before, doc.path_after = 1, 1
assert oanim.wm.motion_path_range() == (10, 20)
assert ops.anim.motion_path_smooth_arc() == {'FINISHED'}
assert doc.frame_current == 15 and close((ball.location_x, ball.location_y), (1.5, arc_y(15))), (
    ball.location_x, ball.location_y)
doc.frame_current = 10
assert close(ball.location_y, arc_y(10)), "only the keys inside the range move"
doc.frame_current = 15
assert ops.anim.motion_path_smooth_arc() == {'FINISHED'}, "already smooth: nothing lost"
assert close(ball.location_y, arc_y(15))

print("Motion path tool tests passed")
