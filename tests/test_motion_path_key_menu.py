"""The timing chart's and Path Timing graph's key diamonds: a click selects the key (as clicking it on the path does,
so the timeline's X deletes it), a right click opens a menu to set its interpolation or delete it
(anim.motion_path_key_interp and anim.motion_path_key_delete). The key behind a diamond is the path's own key there
(a whole drawing's: every channel's), else the key of its layer or of a group above that moves it there.

Run with:  OpenAnimate --background --python tests/test_motion_path_key_menu.py
"""

import oanim

ops = oanim.ops
layers = oanim.data.layers
doc = oanim.data.document
doc.auto_key = True
doc.auto_freeze_keys = False


def key(layer, path, frame):
    return next(k for k in layer.keyframes(path) if k["frame"] == frame)


def frames(layer, path):
    return [k["frame"] for k in layer.keyframes(path)]


# --- A point's path: its own key ------------------------------------------------------------------------------------
ops.layer.new()
art = layers.active
art.key_whole_drawing = False
doc.frame_current = 0
art.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
doc.frame_current = 10
art.set_point(0, (0.0, 1.0))
art.set_point(1, (1.0, 1.0))
doc.frame_current = 0
ids = art.point_ids
p0 = {"layer": str(art.id), "point": str(ids[0])}
assert key(art, "points[0].pos", 10)["interp"] == "SMOOTH"
assert ops.anim.motion_path_key_interp(frame="10", interp="LINEAR", **p0) == {'FINISHED'}
assert key(art, "points[0].pos", 10)["interp"] == "LINEAR"
assert key(art, "points[1].pos", 10)["interp"] == "SMOOTH", "only that path's key"
assert ops.anim.motion_path_key_interp(frame="10", interp="EASE_IN", ease="CUBIC", **p0) == {'FINISHED'}
k = key(art, "points[0].pos", 10)
assert k["interp"] == "EASE_IN" and k["ease"] == "CUBIC", k
doc.frame_current = 5
assert abs(art.points[0][1] - 0.875) < 1e-3, ("eased in, cubic: slowing into the key, 7/8 of the way at half time", art.points[0])
doc.frame_current = 0
assert ops.anim.motion_path_key_interp(frame="7", interp="LINEAR", **p0) == {'CANCELLED'}, "no key there"
assert ops.anim.motion_path_key_interp(frame="10", interp="WOBBLY", **p0) == {'CANCELLED'}
assert ops.ed.undo() == {'FINISHED'}
assert key(art, "points[0].pos", 10)["interp"] == "LINEAR", "one undo step each"

# Deleted from the menu.
assert ops.anim.motion_path_key_delete(frame="10", **p0) == {'FINISHED'}
assert 10 not in frames(art, "points[0].pos") and frames(art, "points[1].pos") == [0, 10]
assert abs(art.points[0][1]) < 1e-4, art.points[0]

# --- A click selects the key (as on the path), and the timeline's X deletes it ---------------------------------------
assert ops.anim.motion_path_select(frame="10", layer=str(art.id), point=str(ids[1])) == {'FINISHED'}
assert key(art, "points[1].pos", 10)["selected"], "selected"
assert ops.anim.keys_delete() == {'FINISHED'}
assert frames(art, "points[1].pos") in ([], [0]), frames(art, "points[1].pos")

# --- A whole drawing: every channel ----------------------------------------------------------------------------------
ops.layer.new()
whole = layers.active
assert whole.key_whole_drawing
doc.frame_current = 0
whole.add_curve([(0.0, 0.0), (1.0, 0.0)])
doc.frame_current = 12
whole.set_point(1, (1.0, 2.0))
doc.frame_current = 24
whole.set_point(1, (1.0, 0.0))
doc.frame_current = 0
d = {"layer": str(whole.id), "drawing": "1"}
assert ops.anim.motion_path_key_interp(frame="12", interp="STEP", **d) == {'FINISHED'}
paths = ["points[0].pos", "points[1].pos", "points[0].radius", "curves[0].points[1].curvature",
         "curves[0].points[0].handle_out"]
for path in paths:
    assert key(whole, path, 12)["interp"] == "STEP", (path, key(whole, path, 12))
    assert key(whole, path, 24)["interp"] == "SMOOTH", path
# Selected by a click: all its points' keys there; X takes the pose away.
assert ops.anim.motion_path_select(frame="24", **d) == {'FINISHED'}
assert all(key(whole, path, 24)["selected"] for path in paths)
assert not key(whole, "points[1].pos", 12)["selected"]
assert oanim.wm.event_simulate("X", area="Timeline"), "X over the timeline deletes it"
doc.frame_current = 1
doc.frame_current = 0
for path in paths:
    assert frames(whole, path) == [0, 12], (path, frames(whole, path))
assert ops.anim.motion_path_key_delete(frame="12", **d) == {'FINISHED'}
doc.frame_current = 1
doc.frame_current = 0
for path in paths:
    assert 12 not in frames(whole, path), (path, frames(whole, path))

# --- A layer in a group: the group's key moves it there ------------------------------------------------------------
ops.layer.new()
inner = layers.active
inner.key_whole_drawing = False
inner.add_curve([(0.0, 0.0), (1.0, 0.0)])
assert ops.layer.group() == {'FINISHED'}
group = layers.active
assert group.id != inner.id
doc.frame_current = 20
group.location_x = 3.0
doc.frame_current = 0
assert frames(group, "position") == [0, 20], frames(group, "position")
g = {"layer": str(inner.id)}
assert ops.anim.motion_path_key_interp(frame="20", interp="LINEAR", **g) == {'FINISHED'}
assert key(group, "position", 20)["interp"] == "LINEAR", key(group, "position", 20)
assert ops.anim.motion_path_key_delete(frame="20", **g) == {'FINISHED'}
assert 20 not in frames(group, "position")

print("Motion path key menu tests passed")
