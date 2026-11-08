"""Convert to Control Point (a position key's right-click menu, anim.motion_path_key_to_control): a key with a key either
side becomes a control point where it was, so the path keeps its shape and every dot stays on its frame, the two
stretches now one (Custom, timed as they went: eases, holds and steps on 2s kept). Convert to Key
(anim.motion_path_control_to_key, a control point's menu) is the way back: a key where the control point is, on the frame
whose dot was nearest it. Refused on an end key, the setup frame and a Key Whole Drawing point's key.

Run with:  OpenAnimate --background --python tests/test_motion_path_key_to_control.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm

EPS = 4e-3


def pos(layer, f):
    doc.frame_current = f
    return (layer.location_x, layer.location_y)


def track(layer, last=45):
    return [pos(layer, f) for f in range(0, last + 1)]


def moved(a, b):
    return max(math.dist(p, q) for p, q in zip(a, b))


def frames(layer):
    return [k["frame"] for k in layer.keyframes("position")]


def key(layer, frame):
    return next(k for k in layer.keyframes("position") if k["frame"] == frame)


def mover(keys, interp="SMOOTH"):
    ops.layer.new()
    lay = layers.active
    lay.add_curve([(0.0, 0.0), (0.1, 0.0)])
    lay.key_whole_drawing = False
    doc.frame_current = 0
    lay.location_x, lay.location_y = -2.0, -1.0
    for other in layers:
        other.selected = other.id == lay.id
    layers.active = lay
    for f, (x, y) in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(lay.id)) == {'FINISHED'}
        lay.keyframe_set("position", f, interp=interp)
    return lay


doc.auto_key = False
doc.motion_paths = True
doc.path_before, doc.path_after = 50, 50
assert ops.wm.tool_motion_path() == {'FINISHED'}

# --- A smooth path: the key becomes a control point, the path and every dot staying -----------------------------------
ball = mover({10: (0.0, 0.0), 20: (2.0, 2.0), 30: (4.0, 0.0), 40: (6.0, 1.0)})
args = dict(layer=str(ball.id))
was = track(ball)
assert ops.anim.motion_path_key_to_control(frame="20", **args) == {'FINISHED'}
assert frames(ball) == [0, 10, 30, 40], frames(ball)
k30 = key(ball, 30)
assert len(k30["path_controls"]) == 1, k30
assert math.dist(k30["path_controls"][0], (2.0, 2.0)) < 1e-4, ("where the key was", k30["path_controls"])
assert k30["path_control_handles"][0]["set"] and k30["path_control_handles"][0]["aligned"], k30
assert k30["interp"] == "CUSTOM", k30["interp"]
now = track(ball)
assert moved(was, now) < EPS, ("every dot stays on its frame", moved(was, now))

# --- And back: a key where the control point is, on its frame, the dots staying ----------------------------------------
assert ops.anim.motion_path_control_to_key(end="30", index="0", **args) == {'FINISHED'}
assert frames(ball) == [0, 10, 20, 30, 40], frames(ball)
assert math.dist(key(ball, 20)["value"], (2.0, 2.0)) < 1e-4, key(ball, 20)
assert key(ball, 30)["path_controls"] == [], "the control point became the key"
assert moved(was, track(ball)) < EPS, ("the dots still stay", moved(was, track(ball)))
assert ops.ed.undo() == {'FINISHED'}
assert ops.ed.undo() == {'FINISHED'}
assert frames(ball) == [0, 10, 20, 30, 40] and key(ball, 30)["path_controls"] == [], "one undo step each"
assert moved(was, track(ball)) < 1e-6

# --- Eases, a hold and steps on 2s: kept in the joined stretch's timing -------------------------------------------------
ball.keyframe_set("position", 20, interp="EASE_IN")
ball.keyframe_set("position", 20, hold=3)
ball.keyframe_set("position", 10, interval=2)
ball.keyframe_set("position", 30, interp="LINEAR")
was = track(ball)
assert ops.anim.motion_path_key_to_control(frame="20", **args) == {'FINISHED'}
assert frames(ball) == [0, 10, 30, 40]
assert moved(was, track(ball)) < EPS, ("eased, held and on 2s as before", moved(was, track(ball)))
# Its own control points stay, in order: a key between two with control points of their own.
assert ops.ed.undo() == {'FINISHED'}
assert ops.anim.motion_path_control_add(frame="15", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_add(frame="25", **args) == {'FINISHED'}
was = track(ball)
assert ops.anim.motion_path_key_to_control(frame="20", **args) == {'FINISHED'}
assert len(key(ball, 30)["path_controls"]) == 3, key(ball, 30)["path_controls"]
assert math.dist(key(ball, 30)["path_controls"][1], (2.0, 2.0)) < 1e-4, "the key's place in the middle"
assert moved(was, track(ball)) < EPS, ("with control points either side too", moved(was, track(ball)))
# The middle one back to a key: the others keep their sides.
assert ops.anim.motion_path_control_to_key(end="30", index="1", **args) == {'FINISHED'}
assert frames(ball) == [0, 10, 20, 30, 40], frames(ball)
assert len(key(ball, 20)["path_controls"]) == 1 and len(key(ball, 30)["path_controls"]) == 1
assert moved(was, track(ball)) < EPS, moved(was, track(ball))

# --- Picked ones, from the viewport's menus: the keys selected, the control points picked ------------------------------
assert ops.anim.motion_path_select(frame="30", **args) == {'FINISHED'}
was = track(ball)
assert ops.anim.motion_path_key_to_control() == {'FINISHED'}, "the key selected on the path"
assert frames(ball) == [0, 10, 20, 40], frames(ball)
assert moved(was, track(ball)) < EPS
assert len(key(ball, 40)["path_controls"]) == 2, key(ball, 40)["path_controls"]
assert ops.anim.motion_path_handle_select(frame="40", control="1", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_to_key() == {'FINISHED'}, "the control point picked"
assert frames(ball) == [0, 10, 20, 30, 40], frames(ball)
assert moved(was, track(ball)) < EPS

# --- Refused: an end key, the setup frame, a Key Whole Drawing point's key --------------------------------------------
before = frames(ball)
assert ops.anim.motion_path_key_to_control(frame="40", **args) == {'CANCELLED'}, "no key after it"
assert ops.anim.motion_path_key_to_control(frame="0", **args) == {'CANCELLED'}, "the setup frame"
assert ops.anim.motion_path_key_to_control(frame="25", **args) == {'CANCELLED'}, "no key there"
assert frames(ball) == before
assert ops.anim.motion_path_control_to_key(end="40", index="5", **args) == {'CANCELLED'}, "no such control point"

ops.layer.new()
art = layers.active
art.key_whole_drawing = True
doc.auto_key = True
doc.frame_current = 0
art.add_curve([(0.0, 0.0), (1.0, 0.0)])
for f, y in ((10, 1.0), (20, 0.0), (30, 1.0)):
    doc.frame_current = f
    art.set_point(0, (0.0, y))
doc.frame_current = 0
doc.auto_key = False
p0 = {"layer": str(art.id), "point": str(art.point_ids[0])}
assert 20 in [k["frame"] for k in art.keyframes("points[0].pos")]
assert ops.anim.motion_path_key_to_control(frame="20", **p0) == {'CANCELLED'}, "Key Whole Drawing keys every point"
art.key_whole_drawing = False
assert ops.anim.motion_path_key_to_control(frame="20", **p0) == {'FINISHED'}, "a point's own key without it"
assert 20 not in [k["frame"] for k in art.keyframes("points[0].pos")]

print("motion path key to control: ok")
