"""Paste Path across kinds of target (anim.motion_path_paste), the cases a bug sweep found: a path pasted onto one of
a Key Together group or a Key Whole Drawing point keeps the others' keys and stays timed as one; turns pasted between a
layer (degrees) and a bone (radians); frame 0's hold and interval; moves on screen under a turned parent bone and in a
scaled layer.

Run with:  OpenAnimate --background --python tests/test_motion_path_paste_kinds.py
"""

import math

import oanim

doc, layers, ops = oanim.data.document, oanim.data.layers, oanim.ops
doc.frame_current = 0
doc.auto_freeze_keys = False


def close(a, b, eps=1e-4):
    return math.dist(a, b) < eps if isinstance(a, tuple) else abs(a - b) < eps


def mk(x=0.0):
    ops.layer.new()
    l = layers.active
    l.key_together = False
    l.key_whole_drawing = False
    l.add_curve([(0.0, 0.0), (0.1, 0.0)])
    l.location_x = x
    return l


def at(l, f):
    doc.frame_current = f
    p = (l.location_x, l.location_y)
    doc.frame_current = 0
    return p


def settle():
    doc.frame_current = 1
    doc.frame_current = 0


# --- A source path keyed on 7 and 15, eased into 15
doc.auto_key = False
src = mk(0.0)
settle()
for f, (x, y) in ((7, (1, 1)), (15, (2, -1))):
    assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(src.id)) == {'FINISHED'}
assert ops.anim.motion_path_key_interp(frame="15", interp="EASE_IN", layer=str(src.id)) == {'FINISHED'}

# --- Onto one layer of a Key Together group: the other's key on 10 stays, the copied keys and timing go to both
a, b = mk(-3.0), mk(3.0)
for l in layers:
    l.selected = l.id in (a.id, b.id)
assert ops.layer.group() == {'FINISHED'}
group = layers.active
group.key_together = True
settle()
assert ops.anim.motion_path_move(frame="10", x="-2", y="1", layer=str(a.id)) == {'FINISHED'}
assert ops.anim.motion_path_move(frame="10", x="3", y="2", layer=str(b.id)) == {'FINISHED'}
b10 = at(b, 10)
assert ops.anim.motion_path_copy(layer=str(src.id)) == {'FINISHED'}
assert ops.anim.motion_path_paste(layer=str(a.id)) == {'FINISHED'}
settle()
assert close(at(b, 10), b10), ("b keeps its key on 10", at(b, 10), b10)
for l in (a, b):
    ks = {k["frame"]: k for k in l.keyframes("position")}
    assert sorted(ks) == [0, 7, 10, 15], (l.name, sorted(ks))
    assert ks[15]["interp"] == "EASE_IN", (l.name, "timed as one", ks[15]["interp"])
s0, a0 = at(src, 0), at(a, 0)
for f in (7, 15):
    s, p = at(src, f), at(a, f)
    assert close((p[0] - a0[0], p[1] - a0[1]), (s[0] - s0[0], s[1] - s0[1])), (f, p, s)
assert ops.ed.undo() == {'FINISHED'}
settle()
assert sorted(k["frame"] for k in b.keyframes("position")) == [0, 10], "one undo step"

# --- Onto a Key Whole Drawing point: point 1's own key on 10 stays, every point timed as the drawing
d = mk(0.0)
d.key_whole_drawing = True
d.add_curve([(0.0, 2.0), (1.0, 2.0), (2.0, 2.0)])
settle()
ids = d.point_ids
assert ops.anim.motion_path_move(frame="10", x="1", y="3", layer=str(d.id), point=str(ids[3])) == {'FINISHED'}


def pt(i, f):
    doc.frame_current = f
    p = tuple(d.points[i][:2])
    doc.frame_current = 0
    return p


p3 = pt(3, 10)
assert ops.anim.motion_path_paste(layer=str(d.id), point=str(ids[2])) == {'FINISHED'}
settle()
assert close(pt(3, 10), p3), ("point 3 keeps its key on 10", pt(3, 10), p3)
interp = {i: next(k["interp"] for k in d.keyframes("points[%d].pos" % i) if k["frame"] == 15) for i in (2, 3, 4)}
assert set(interp.values()) == {"EASE_IN"}, ("one drawing timed as one", interp)
moved = (pt(2, 15)[0] - pt(2, 0)[0], pt(2, 15)[1] - pt(2, 0)[1])
assert close(moved, (2.0, -1.0)), moved

# --- A layer's turn (degrees) onto a bone (radians), and back
doc.auto_key = True
turner = mk(5.0)
ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="B", head_x="1", head_y="0", tail_x="1", tail_y="0.5", parent="")
doc.frame_current = 10
turner.rotation = 30.0
doc.frame_current = 0
assert ops.anim.motion_path_copy(layer=str(turner.id)) == {'FINISHED'}
assert ops.anim.motion_path_paste(layer=str(rig.id), bone="B", head="1") == {'FINISHED'}
k = {x["frame"]: x["value"] for x in rig.keyframes('bones["B"].angle')}
assert close(k[10] - k[0], math.radians(30.0)), ("30 degrees as radians", k)
back = mk(6.0)
assert ops.anim.motion_path_copy(layer=str(rig.id), bone="B", head="1") == {'FINISHED'}
assert ops.anim.motion_path_paste(layer=str(back.id)) == {'FINISHED'}
k = {x["frame"]: x["value"] for x in back.keyframes("rotation")}
assert close(k[10] - k[0], 30.0), ("radians as degrees", k)

# --- Frame 0's hold and interval: how the first stretch plays
doc.auto_key = False
h, g = mk(-8.0), mk(8.0)
settle()
assert ops.anim.motion_path_move(frame="10", x="-7", y="0", layer=str(h.id)) == {'FINISHED'}
h.keyframe_set("position", 0, hold=4, interval=2)
assert ops.anim.motion_path_copy(layer=str(h.id)) == {'FINISHED'}
assert ops.anim.motion_path_paste(layer=str(g.id)) == {'FINISHED'}
k0 = g.keyframes("position")[0]
assert (k0["hold"], k0["interval"]) == (4, 2), k0
for f in (3, 5, 7, 9):
    assert close(at(g, f)[0] - at(g, 0)[0], at(h, f)[0] - at(h, 0)[0]), (f, at(g, f), at(h, f))
assert ops.ed.undo() == {'FINISHED'}
assert ops.anim.motion_path_paste(how="TIMING", layer=str(g.id)) == {'FINISHED'}
k0 = g.keyframes("position")[0]
assert (k0["hold"], k0["interval"]) == (4, 2), ("Timing Only too", k0)

# --- On screen: a root bone's move onto a child of a parent turned 90 degrees, and onto a point in a scaled layer
doc.auto_key = True
ops.layer.new_bone()
arm = layers.active
ops.armature.bone_add(name="A", head_x="-1", head_y="0", tail_x="-1", tail_y="0.5", parent="")
ops.armature.select_none()
ops.armature.bone_add(name="P", head_x="0", head_y="-3", tail_x="0", tail_y="-2", parent="")
ops.armature.select_none()
ops.armature.bone_add(name="C", head_x="2", head_y="-3", tail_x="2", tail_y="-2.5", parent="P")
doc.frame_current = 10
arm.bone_set("A", position=(-0.6, 0.0))
doc.frame_current = 0


def bone_moved(n, f):
    doc.frame_current = 0
    p = arm.to_document(arm.bone_pose(n)[:2])
    doc.frame_current = f
    q = arm.to_document(arm.bone_pose(n)[:2])
    doc.frame_current = 0
    return (q[0] - p[0], q[1] - p[1])


assert ops.anim.motion_path_copy(layer=str(arm.id), bone="A", head="1") == {'FINISHED'}
assert ops.anim.motion_path_paste(layer=str(arm.id), bone="C", head="1") == {'FINISHED'}
assert close(bone_moved("C", 10), bone_moved("A", 10), 1e-3), (bone_moved("C", 10), bone_moved("A", 10))

big = mk(0.0)
big.add_curve([(0.0, 4.0), (1.0, 4.0)])
big.scale_x = big.scale_y = 2.0
settle()
bid = big.point_ids
assert ops.anim.motion_path_paste(layer=str(big.id), point=str(bid[2])) == {'FINISHED'}


def screen(i, f):
    doc.frame_current = f
    p = big.to_document(tuple(big.points[i][:2]))
    doc.frame_current = 0
    return p


on = (screen(2, 10)[0] - screen(2, 0)[0], screen(2, 10)[1] - screen(2, 0)[1])
assert close(on, bone_moved("A", 10), 1e-3), ("a scaled layer's point moves as much on screen", on)

print("motion path paste kinds: ok")
