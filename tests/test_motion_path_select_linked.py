"""L and Shift L with the Motion Path tool's Edit: with the mouse over a path, L picks the keys either end of the stretch
there and its control points (not its dots), adding to what is picked; Shift L lets go of them; off a path L is the
drawing's Select Linked. Keys and control points are picked together (Shift click adds one kind and keeps the other;
handles still go on their own), and G R S, V and X work on them together: about the middle of them all, each moved
once.

The viewport sets the path under the mouse as it draws the paths (checked live on a private copy); a background run
draws none, so oanim.wm.motion_path_under_set stands in for the mouse.

Run with:  OpenAnimate --background --python tests/test_motion_path_select_linked.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def close(a, b, eps=1e-3):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def key(layer, frame):
    return next(k for k in layer.keyframes("position") if k["frame"] == frame)


def frames(layer):
    return [k["frame"] for k in layer.keyframes("position")]


def selected(layer):
    return [k["frame"] for k in layer.keyframes("position") if k["selected"]]


def picks():
    return sorted((p["frame"], p["control"], p["control_handle"]) for p in wm.motion_path_handle_picks())


def control(layer, frame, i=0):
    return tuple(key(layer, frame)["path_controls"][i])


doc.auto_key = False
doc.motion_paths = True
assert ops.wm.tool_motion_path() == {'FINISHED'}

ops.layer.new()
ball = layers.active
ball.add_curve([(0.0, 0.0), (0.1, 0.0)])
doc.frame_current = 0
ball.location_x, ball.location_y = -5.0, -5.0
for other in layers:
    other.selected = other.id == ball.id
args = dict(layer=str(ball.id))
for f, (x, y) in [(10, (0.0, 0.0)), (30, (4.0, 0.0)), (50, (4.0, 4.0))]:
    assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), **args) == {'FINISHED'}
# A control point on each stretch: (2, 1) on 10-30, (5, 2) on 30-50.
assert ops.anim.motion_path_control_add(frame="20", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="30", index="0", x="2", y="1", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_add(frame="40", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="50", index="0", x="5", y="2", **args) == {'FINISHED'}
assert close(control(ball, 30), (2.0, 1.0)) and close(control(ball, 50), (5.0, 2.0))


def none():
    assert ops.anim.motion_path_select(none="1", **args) == {'FINISHED'}
    assert ops.anim.motion_path_handle_select(none="1") == {'FINISHED'}
    assert selected(ball) == [] and picks() == []


# --- L over a dot: the stretch's two keys and its control point, not its dots ----------------------------------------
none()
assert ops.anim.motion_path_select(frame="45", **args) == {'FINISHED'}, "a dot picked"
assert len(wm.motion_path_dots()) == 1
wm.motion_path_under_set(layer=ball.id, frame=20)
assert wm.event_simulate("L", area="Viewport")
assert selected(ball) == [10, 30], ("the stretch's keys", selected(ball))
assert picks() == [(30, 0, 0)], ("its control point", picks())
assert wm.motion_path_dots() == [], "dots let go: picked on their own"

# --- L again over a control point of the next stretch: added -----------------------------------------------------------
wm.motion_path_under_set(layer=ball.id, frame=40, end=50)
assert wm.event_simulate("L", area="Viewport")
assert selected(ball) == [10, 30, 50], selected(ball)
assert picks() == [(30, 0, 0), (50, 0, 0)], picks()

# --- Shift L over the first: let go of, the other kept -----------------------------------------------------------------
wm.motion_path_under_set(layer=ball.id, frame=20)
assert wm.event_simulate("L", area="Viewport", shift=True)
assert selected(ball) == [50], ("its keys let go", selected(ball))
assert picks() == [(50, 0, 0)], picks()

# --- On a key: the stretches either side ---------------------------------------------------------------------------
none()
wm.motion_path_under_set(layer=ball.id, frame=30)
assert wm.event_simulate("L", area="Viewport")
assert selected(ball) == [10, 30, 50], selected(ball)
assert picks() == [(30, 0, 0), (50, 0, 0)], picks()

# --- Handles picked go: they are picked on their own -------------------------------------------------------------------
none()
assert ops.anim.motion_path_handle_select(frame="30", out="1", **args) == {'FINISHED'}
assert picks() == [(30, -1, 0)]
wm.motion_path_under_set(layer=ball.id, frame=20)
assert wm.event_simulate("L", area="Viewport")
assert picks() == [(30, 0, 0)], ("the key handle let go", picks())

# --- Off a path: not L's (the drawing's Select Linked) -----------------------------------------------------------------
none()
wm.motion_path_under_set(None)
assert ops.anim.motion_path_select_linked_pick.poll() is False
wm.event_simulate("L", area="Viewport")
assert selected(ball) == [] and picks() == [], "nothing on the paths picked"
# Nor with another tool.
wm.motion_path_under_set(layer=ball.id, frame=20)
assert ops.wm.tool_select() == {'FINISHED'}
assert ops.anim.motion_path_select_linked_pick.poll() is False
assert ops.wm.tool_motion_path() == {'FINISHED'}
# Called by name: frame=, end=.
assert ops.anim.motion_path_select_linked(end="50", **args) == {'FINISHED'}
assert selected(ball) == [30, 50] and picks() == [(50, 0, 0)], (selected(ball), picks())
assert ops.anim.motion_path_select_linked(frame="0", **args) == {'CANCELLED'} or selected(ball), "off the stretches"

# --- Shift click: a key keeps the control points picked, a dot lets them go ---------------------------------------------
none()
assert ops.anim.motion_path_handle_select(frame="30", control="0", **args) == {'FINISHED'}
assert ops.anim.motion_path_select(frame="50", extend="1", **args) == {'FINISHED'}
assert selected(ball) == [50] and picks() == [(30, 0, 0)], ("key added, control point kept", selected(ball), picks())
assert ops.anim.motion_path_select(frame="45", extend="1", **args) == {'FINISHED'}
assert picks() == [], "a dot: control points go"
# Without Shift a key lets them go.
assert ops.anim.motion_path_handle_select(frame="30", control="0", **args) == {'FINISHED'}
assert ops.anim.motion_path_select(frame="10", **args) == {'FINISHED'}
assert picks() == []
# A control point with Shift keeps the keys; without, lets them go.
none()
assert ops.anim.motion_path_select(frame="10", **args) == {'FINISHED'}
assert ops.anim.motion_path_handle_select(frame="30", control="0", extend="1", **args) == {'FINISHED'}
assert selected(ball) == [10] and picks() == [(30, 0, 0)], ("control point added, key kept", selected(ball), picks())
assert ops.anim.motion_path_handle_select(frame="50", control="0", extend="1", **args) == {'FINISHED'}
assert selected(ball) == [10] and picks() == [(30, 0, 0), (50, 0, 0)], picks()
assert ops.anim.motion_path_handle_select(frame="50", control="0", **args) == {'FINISHED'}
assert selected(ball) == [] and picks() == [(50, 0, 0)], "on its own"
# A handle with Shift: still on its own, the keys let go.
assert ops.anim.motion_path_select(frame="10", **args) == {'FINISHED'}
assert ops.anim.motion_path_handle_select(frame="30", out="1", extend="1", **args) == {'FINISHED'}
assert selected(ball) == [], "handles never with keys"

# --- G: keys and control point moved together, each once ---------------------------------------------------------------
def first_stretch():
    """The first stretch picked with L (again after an undo: picking is no undo step)."""
    none()
    wm.motion_path_under_set(layer=ball.id, frame=20)
    assert wm.event_simulate("L", area="Viewport")
    assert selected(ball) == [10, 30] and picks() == [(30, 0, 0)]


first_stretch()
k10, k30, c30, c50 = key(ball, 10)["value"], key(ball, 30)["value"], control(ball, 30), control(ball, 50)
assert ops.anim.motion_path_keys_transform(kind="G", x="1", y="0.5") == {'FINISHED'}
assert close(key(ball, 10)["value"], (k10[0] + 1, k10[1] + 0.5)), key(ball, 10)["value"]
assert close(key(ball, 30)["value"], (k30[0] + 1, k30[1] + 0.5)), key(ball, 30)["value"]
assert close(control(ball, 30), (c30[0] + 1, c30[1] + 0.5)), ("moved once, with them", control(ball, 30))
assert close(control(ball, 50), c50), "the other stretch's left"
assert ops.ed.undo() == {'FINISHED'}

# --- R and S about the middle of them all, drawn there ---------------------------------------------------------------
first_stretch()
pts = [tuple(key(ball, 10)["value"]), tuple(key(ball, 30)["value"]), control(ball, 30)]
middle = (sum(p[0] for p in pts) / 3, sum(p[1] for p in pts) / 3)
assert close(wm.motion_path_handles_pivot(), middle), ("the pivot", wm.motion_path_handles_pivot(), middle)
assert ops.anim.motion_path_keys_transform(kind="R", angle=str(math.pi)) == {'FINISHED'}
turned = lambda p: (2 * middle[0] - p[0], 2 * middle[1] - p[1])
assert close(key(ball, 10)["value"], turned(pts[0])), (key(ball, 10)["value"], turned(pts[0]))
assert close(key(ball, 30)["value"], turned(pts[1])), (key(ball, 30)["value"], turned(pts[1]))
assert close(control(ball, 30), turned(pts[2])), (control(ball, 30), turned(pts[2]))
assert ops.ed.undo() == {'FINISHED'}
first_stretch()
assert ops.anim.motion_path_keys_transform(kind="S", factor="2") == {'FINISHED'}
assert close(control(ball, 30), (2 * pts[2][0] - middle[0], 2 * pts[2][1] - middle[1])), control(ball, 30)
assert close(key(ball, 10)["value"], (2 * pts[0][0] - middle[0], 2 * pts[0][1] - middle[1])), key(ball, 10)["value"]
assert ops.ed.undo() == {'FINISHED'}

# --- V: the keys' handle types ---------------------------------------------------------------------------------------
first_stretch()
handles_50 = key(ball, 50)["path_handles"]
assert key(ball, 10)["path_handles"] != "BROKEN" or key(ball, 30)["path_handles"] != "BROKEN", "a change to see"
assert ops.anim.motion_path_handles_type(mode="BROKEN") == {'FINISHED'}
assert key(ball, 10)["path_handles"] == "BROKEN" and key(ball, 30)["path_handles"] == "BROKEN"
assert key(ball, 50)["path_handles"] == handles_50, ("not picked: left", key(ball, 50)["path_handles"])
assert ops.ed.undo() == {'FINISHED'}

# --- X: the control point and the keys deleted -------------------------------------------------------------------------
first_stretch()
assert wm.event_simulate("X", area="Viewport")
assert 10 not in frames(ball) and 30 not in frames(ball), frames(ball)
assert 50 in frames(ball), frames(ball)
assert picks() == []
assert ops.ed.undo() == {'FINISHED'}
assert frames(ball) == [0, 10, 30, 50], frames(ball)

print("Motion path L: ok")
