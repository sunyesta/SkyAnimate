"""Copy Path and Paste Path (the Motion Path tool's right-click menu; anim.motion_path_copy / motion_path_paste): a
path's keys kept as changes from where it was, pasted so the target moves the same way from where it is (its setup
kept). Paste Path takes the keys, frames and timing; Shape Only the line on the target's own frames; Timing Only the
timing, keying the target in place; Flipped mirrors it left to right. Two picked keys copy only from the first to the
last. A Key Whole Drawing point is timed as its drawing, so Timing Only refuses it.

Run with:  OpenAnimate --background --python tests/test_motion_path_copy_paste.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm
doc.frame_current = 0
doc.auto_key = True
doc.auto_freeze_keys = False  # keyed channel by channel here


def keys(layer, path):
    try:
        return {k["frame"]: k for k in layer.keyframes(path)}
    except KeyError:
        return {}


def close(a, b, eps=1e-4):
    return math.dist(a, b) < eps if isinstance(a, tuple) else abs(a - b) < eps


# Two eyes: the right one looks over on 10 and back up on 20, turning on 10; the left one is still.
ops.layer.new_bone()
rig = layers.active
rig.name = "rig"
ops.armature.bone_add(name="eye_R", head_x="-1", head_y="0", tail_x="-1", tail_y="0.5", parent="")
ops.armature.select_none()
ops.armature.bone_add(name="eye_L", head_x="1", head_y="0", tail_x="1", tail_y="0.5", parent="")
R, L = 'bones["eye_R"]', 'bones["eye_L"]'
doc.frame_current = 10
rig.bone_set("eye_R", position=(-0.6, 0.0), angle=0.3)
doc.frame_current = 20
rig.bone_set("eye_R", position=(-0.6, 0.4))
doc.frame_current = 0
eye_r = dict(layer=str(rig.id), bone="eye_R", head="1")
eye_l = dict(layer=str(rig.id), bone="eye_L", head="1")
assert ops.anim.motion_path_key_interp(frame="10", interp="EASE_IN", **eye_r) == {'FINISHED'}
assert not keys(rig, L + ".position"), "the left eye has no keys yet"


def head(name, f):
    doc.frame_current = f
    h = rig.bone_pose(name)[:2]
    doc.frame_current = 0
    return h


def moved(name, f, since=0):
    a, b = head(name, since), head(name, f)
    return (b[0] - a[0], b[1] - a[1])


# Copied with nothing picked: all of it. Pasted: the same moves from where the left eye is (a bone's moves are kept
# along its rest axes: these eyes rest pointing up, so on screen is what is checked).
assert ops.anim.motion_path_copy(**eye_r) == {'FINISHED'}
assert ops.anim.motion_path_paste(**eye_l) == {'FINISHED'}
assert wm.undo_history()[0][-1] == "Paste Path", wm.undo_history()[0][-1]
lk = keys(rig, L + ".position")
assert sorted(lk) == [0, 10, 20], sorted(lk)
assert close(head("eye_L", 0), (1.0, 0.0)), "its setup kept"
for f in (5, 10, 15, 20):
    assert close(moved("eye_L", f), moved("eye_R", f)), (f, moved("eye_L", f), moved("eye_R", f))
assert lk[10]["interp"] == "EASE_IN", "its timing too"
la, ra = keys(rig, L + ".angle"), keys(rig, R + ".angle")
turn = ra[10]["value"] - ra[0]["value"]
assert sorted(la) == [0, 10] and abs(turn) > 0.1 and close(la[10]["value"] - la[0]["value"], turn), (la, turn)
assert ops.ed.undo() == {'FINISHED'} and not keys(rig, L + ".position"), "one undo step"

# Flipped: across goes the other way on screen, and so does the turn.
assert ops.anim.motion_path_paste(flipped="1", **eye_l) == {'FINISHED'}
for f in (10, 20):
    r, l = moved("eye_R", f), moved("eye_L", f)
    assert close(l, (-r[0], r[1])), (f, l, r)
la = keys(rig, L + ".angle")
assert close(la[10]["value"] - la[0]["value"], -turn), la
assert ops.ed.undo() == {'FINISHED'}

# Ctrl C and Ctrl V with the Motion Path tool, the mouse over a path (a background run draws none, so
# motion_path_under_set stands in for it): the same as Copy Path and Paste Path. Off a path they are the viewport's.
assert ops.wm.tool_motion_path() == {'FINISHED'}
bid = {b["name"]: b["id"] for b in rig.bones}
wm.motion_path_under_set(layer=rig.id, bone=bid["eye_R"], head=True, frame=15)
assert wm.event_simulate("C", ctrl=True, area="Viewport")
wm.motion_path_under_set(layer=rig.id, bone=bid["eye_L"], head=True, frame=15)
assert wm.event_simulate("V", ctrl=True, area="Viewport")
assert sorted(keys(rig, L + ".position")) == [0, 10, 20], sorted(keys(rig, L + ".position"))
assert close(moved("eye_L", 20), moved("eye_R", 20))
assert ops.ed.undo() == {'FINISHED'} and not keys(rig, L + ".position")
wm.motion_path_under_set()
assert ops.anim.motion_path_paste() == {'CANCELLED'}, "no path under the mouse, none named"
assert ops.wm.tool_select() == {'FINISHED'}

# The left eye keyed on its own frames (5 and 15): Shape Only puts the copied line on them, keeping its frames.
doc.auto_key = False  # keyed by an operator, so undo steps keep them
assert ops.anim.keyframe_toggle(layer=str(rig.id), paths=L + ".position") == {'FINISHED'}  # its setup, on frame 0
for f, y in ((5, 0.1), (15, 0.2)):
    doc.frame_current = f
    rig.bone_set("eye_L", position=(1.0, y))
    assert ops.anim.keyframe_toggle(layer=str(rig.id), paths=L + ".position") == {'FINISHED'}
doc.frame_current = 0
doc.auto_key = True
assert ops.anim.motion_path_paste(how="SHAPE", **eye_l) == {'FINISHED'}
lk = keys(rig, L + ".position")
assert sorted(lk) == [0, 5, 15], ("its own frames", sorted(lk))
for f in (5, 15):
    assert close(moved("eye_L", f), moved("eye_R", f), 1e-3), (f, moved("eye_L", f), moved("eye_R", f))
assert ops.ed.undo() == {'FINISHED'}

# Timing Only: keyed in place on 10 and 20, those keys timed as the right eye's; its own keys and line stay.
before = head("eye_L", 15)
assert ops.anim.motion_path_paste(how="TIMING", **eye_l) == {'FINISHED'}
lk = keys(rig, L + ".position")
assert sorted(lk) == [0, 5, 10, 15, 20], sorted(lk)
assert lk[10]["interp"] == "EASE_IN", lk[10]
assert close(head("eye_L", 15), before), "its keys keep their places"
assert ops.ed.undo() == {'FINISHED'}

# Two keys picked: only from the first to the last of them (10 to 20), from where the target is on 10; its keys
# inside that span replaced, the one before kept.
ops.anim.keys_select_all(action="DESELECT")
for f in ("10", "20"):
    assert ops.anim.motion_path_select(frame=f, extend="1", **eye_r) == {'FINISHED'}
at5 = head("eye_L", 5)
assert ops.anim.motion_path_copy(**eye_r) == {'FINISHED'}
assert ops.anim.motion_path_paste(**eye_l) == {'FINISHED'}
lk = keys(rig, L + ".position")
assert sorted(lk) == [0, 5, 10, 20], sorted(lk)
assert close(head("eye_L", 5), at5), "before the span untouched"
assert close(moved("eye_L", 20, 10), moved("eye_R", 20, 10)), (moved("eye_L", 20, 10), moved("eye_R", 20, 10))
assert ops.ed.undo() == {'FINISHED'}

# --- Points: a drawing keyed point by point; point 0's path pasted onto point 1 ------------------------------------
ops.layer.new()
art = layers.active
art.key_together = False
art.key_whole_drawing = False
art.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
doc.frame_current = 10
art.set_point(0, (0.0, 1.0))
doc.frame_current = 0
ids = art.point_ids
p0 = dict(layer=str(art.id), point=str(ids[0]))
p1 = dict(layer=str(art.id), point=str(ids[1]))
assert ops.anim.motion_path_copy(**p0) == {'FINISHED'}
assert ops.anim.motion_path_paste(**p1) == {'FINISHED'}


def point_at(i, f):
    doc.frame_current = f
    at = tuple(art.points[i][:2])
    doc.frame_current = 0
    return at


assert close(point_at(1, 10), (1.0, 1.0)), point_at(1, 10)
# A bone's moves onto a point: the move pastes.
assert ops.anim.motion_path_copy(**eye_r) == {'FINISHED'}
assert ops.anim.motion_path_paste(**dict(layer=str(art.id), point=str(ids[2]))) == {'FINISHED'}
assert close(point_at(2, 20), (2.4, 0.4)), point_at(2, 20)

# Keyed as a whole drawing: its points' timing is the drawing's, so Timing Only refuses; Paste Path still works.
art.key_whole_drawing = True
doc.frame_current = 1
doc.frame_current = 0
assert ops.anim.motion_path_paste(how="TIMING", **p1) == {'CANCELLED'}
assert ops.anim.motion_path_paste(**p1) == {'FINISHED'}

# Nothing to copy: a still path.
ops.layer.new_bone()
still = layers.active
ops.armature.bone_add(name="b", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
assert ops.anim.motion_path_copy(layer=str(still.id), bone="b", head="1") == {'CANCELLED'}

print("motion path copy paste: ok")
