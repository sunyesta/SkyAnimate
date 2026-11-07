"""A stretch on 3s (its start key's interval) keeps its timing underneath, played every third frame: the timing chart
says what each frame plays (a held frame its step's, `played` in wm.motion_path_timing), and has ticks only for the
frames that play a pose of their own. Sliding a held frame's dot moves the frame whose pose it holds, so what plays
changes with it.

Run with:  OpenAnimate --background --python tests/test_path_timing_threes.py
"""

import math

import oanim

ops, doc, L, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm
ops.wm.read_homefile()
doc.auto_key = False
assert ops.wm.tool_motion_path() == {'FINISHED'}
ops.layer.new()
v = L.active
v.add_curve([(0.0, 0.0), (0.1, 0.0)])
for layer in L:
    layer.selected = layer.id == v.id
A = dict(layer=str(v.id))
for f, (x, y) in {5: (0.3, 0.3), 10: (0.0, 0.0), 30: (1.0, 0.0)}.items():
    assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), **A) == {'FINISHED'}
assert ops.anim.motion_path_key_interp(frame="10", interval="3", **A) == {'FINISHED'}


def stretch():
    return next(s for s in wm.motion_path_timing()[0]["stretches"] if s["end"] == 30)


def pose(f):
    doc.frame_current = f
    return v.location_x


def check(why):
    s = stretch()
    assert s["interval"] == 3, s["interval"]
    for f in range(11, 30):
        # The line runs along x from 0 to 1, so x is the share of the way.
        assert abs(pose(f) - s["played"][f - 10]) < 2e-3, (why, f, pose(f), s["played"][f - 10])
    for f in (20, 21):
        assert s["played"][f - 10] == s["played"][19 - 10], (why, "held on", f)


check("as it is")
was = stretch()["played"][19 - 10]
before = {f: pose(f) for f in range(11, 30)}

# Frame 20 holds 19's pose: sliding it moves 19, and 20 and 21 with it.
assert ops.anim.motion_path_slide(frame="20", progress="0.6", **A) == {'FINISHED'}
check("slid")
assert abs(stretch()["played"][19 - 10] - 0.6) < 1e-3, ("19 went where 20 was slid", stretch()["played"][9], was)
assert abs(pose(20) - 0.6) < 2e-3 and abs(pose(21) - 0.6) < 2e-3, (pose(20), pose(21))
assert ops.ed.undo() == {'FINISHED'}

# Dragged on the path (to a place, not a share of the way) the same.
assert ops.anim.motion_path_slide(frame="20", x="0.6", y="0", **A) == {'FINISHED'}
assert abs(pose(19) - 0.6) < 2e-3 and abs(pose(20) - 0.6) < 2e-3, ("dragged on the path", pose(19), pose(20))
assert ops.ed.undo() == {'FINISHED'}

# A frame held at the start key's own pose has nothing to slide.
assert ops.anim.motion_path_slide(frame="11", progress="0.3", **A) == {'CANCELLED'}
assert all(abs(pose(f) - before[f]) < 1e-6 for f in before), "a frame held on the key moved the stretch"

# Ease In reshapes the timing; what plays still matches the chart.
assert ops.anim.motion_path_stretch_shape(frame="20", shape="EASE_IN", **A) == {'FINISHED'}
check("Ease In")

# --- A bone's tip on 3s: dragging a held frame's dot turns the frame whose pose it holds -----------------------------------
ops.wm.read_homefile()
doc.auto_key = True
doc.frame_current = 0
assert ops.wm.tool_motion_path() == {'FINISHED'}
ops.layer.new_bone()
rig = L.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
for f, a in ((10, 0.2), (30, math.pi / 2)):
    doc.frame_current = f
    rig.bone_set("arm", angle=a)
    doc.frame_current = f + 1
rig.select_bone("arm")
B = dict(layer=str(rig.id), bone="arm")
assert ops.anim.motion_path_key_interp(frame="10", interval="3", **B) == {'FINISHED'}


def angle(f):
    doc.frame_current = f
    return next(b for b in rig.bones if b["name"] == "arm")["angle"]


assert angle(20) == angle(19), "set up: on 3s"
assert ops.anim.motion_path_slide(frame="20", x=str(math.cos(1.0)), y=str(math.sin(1.0)), **B) == {'FINISHED'}
assert abs(angle(19) - 1.0) < 0.01 and abs(angle(20) - 1.0) < 0.01, ("the tip dragged on a held frame", angle(19),
                                                                   angle(20))

print("test_path_timing_threes: all passed")
