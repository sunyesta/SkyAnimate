"""Control points' handles work as a vector point's: worked out smooth from the points either side until one is moved,
then kept; Aligned (as they come), moving one turns the other to stay in line, keeping its length; Alt (alone=1) moves
one alone and unaligns the point; Align and Unalign as the vector ones; G R S on a picked handle; X on a handle makes
them smooth again, X on the point deletes it. They shape the path only (the timing stays), ride with the keys, the
sculpt brushes fit them, and they are saved.

Run with:  OpenAnimate --background --python tests/test_motion_path_control_handles.py
"""

import math
import os
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def pos(layer, f):
    doc.frame_current = f
    return (layer.location_x, layer.location_y)


def key(layer, frame):
    return next(k for k in layer.keyframes("position") if k["frame"] == frame)


def handles(layer):
    return key(layer, 30)["path_control_handles"][0]


def unit(v):
    n = math.hypot(*v)
    return (v[0] / n, v[1] / n)


def in_line(h):
    a, b = unit(h["in"]), unit(h["out"])
    return close(a, (-b[0], -b[1]), 1e-3)


def shares(points):
    run = [0.0]
    for a, b in zip(points, points[1:]):
        run.append(run[-1] + math.dist(a, b))
    return [r / run[-1] for r in run]


def mover(keys, interp="LINEAR"):
    ops.layer.new()
    lay = layers.active
    lay.add_curve([(0.0, 0.0), (0.1, 0.0)])
    lay.key_whole_drawing = False
    doc.frame_current = 0
    lay.location_x, lay.location_y = -5.0, -5.0
    for other in layers:
        other.selected = other.id == lay.id
    layers.active = lay
    for f, (x, y) in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(lay.id)) == {'FINISHED'}
        lay.keyframe_set("position", f, interp=interp)
    return lay


doc.auto_key = False
doc.motion_paths = True
doc.path_before, doc.path_after = 40, 40
assert ops.wm.tool_motion_path() == {'FINISHED'}

ball = mover({10: (0.0, 0.0), 30: (4.0, 0.0)})
args = dict(layer=str(ball.id))
assert ops.anim.motion_path_control_add(frame="20", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="30", index="0", x="2", y="1", **args) == {'FINISHED'}

# --- At first: smooth, worked out from the points either side, in line, not set --------------------------------------
h = handles(ball)
assert h["set"] is False and h["aligned"] is True, h
assert in_line(h), ("smooth through it", h)
arc = [pos(ball, f) for f in range(10, 31)]
# They follow the points either side: moving the key after turns them, as they aren't its own yet.
assert ops.anim.motion_path_move(frame="30", x="4", y="2", **args) == {'FINISHED'}
assert not close(handles(ball)["out"], h["out"], 1e-3), "worked out from its neighbours until moved"
assert ops.ed.undo() == {'FINISHED'}

# --- Dragging one: its own from then on, the other turning to stay in line, keeping its length -------------------------
cx, cy = 2.0, 1.0
control_was = key(ball, 30)["path_controls"][0]
in_len = math.hypot(*h["in"])
assert ops.anim.motion_path_control_handle_move(frame="30", index="0", out="1", x=str(cx + 0.5), y=str(cy + 0.8),
                                                **args) == {'FINISHED'}
h2 = handles(ball)
assert h2["set"] is True and h2["aligned"] is True, h2
assert close(h2["out"], (0.5, 0.8), 1e-3), ("the tip where it was put", h2)
assert in_line(h2), ("the in handle turned to stay in line", h2)
assert close(math.hypot(*h2["in"]), in_len, 1e-3), "keeping its length"
bent = [pos(ball, f) for f in range(10, 31)]
assert close(key(ball, 30)["path_controls"][0], control_was, 1e-6), "the control point stays"
assert max(abs(a[1] - b[1]) for a, b in zip(arc, bent)) > 0.05, "the path bends by the handle"
assert max(abs(a - b) for a, b in zip(shares(arc), shares(bent))) < 0.03, "the timing kept (each frame its share)"
assert [k["frame"] for k in ball.keyframes("position")] == [0, 10, 30], "no key made"
# Now its own: the key moving no longer turns them (they ride with it, kept relative).
stored = h2
assert ops.anim.motion_path_move(frame="30", x="4", y="0.5", **args) == {'FINISHED'}
assert in_line(handles(ball)), "still in line"
assert ops.ed.undo() == {'FINISHED'}
assert close(handles(ball)["out"], stored["out"], 1e-4)

# --- Alt: one alone, the point unaligned; then a drag turns the other no more ----------------------------------------
out_was = handles(ball)["out"]
assert ops.anim.motion_path_control_handle_move(frame="30", index="0", out="0", x=str(cx - 0.6), y=str(cy + 0.4),
                                                alone="1", **args) == {'FINISHED'}
h3 = handles(ball)
assert h3["aligned"] is False and close(h3["out"], out_was, 1e-4), ("Alt: the other stays", h3)
assert close(h3["in"], (-0.6, 0.4), 1e-3), h3
assert ops.anim.motion_path_control_handle_move(frame="30", index="0", out="1", x=str(cx + 0.9), y=str(cy),
                                                **args) == {'FINISHED'}
assert close(handles(ball)["in"], (-0.6, 0.4), 1e-3), "unaligned: each its own way"

# --- Align and Unalign (the right-click menu), as the vector ones -----------------------------------------------------
assert ops.anim.motion_path_handle_select(frame="30", control="0", control_handle="2", **args) == {'FINISHED'}
assert wm.motion_path_handle_picks()[0]["control_handle"] == 2, wm.motion_path_handle_picks()
in_was = handles(ball)["in"]
assert ops.anim.motion_path_controls_align(aligned="1") == {'FINISHED'}
h4 = handles(ball)
assert h4["aligned"] is True and in_line(h4), h4
assert close(h4["in"], in_was, 1e-4), "the handle picked is the one that turns into line"
assert ops.anim.motion_path_controls_align(aligned="0") == {'FINISHED'}
assert handles(ball)["aligned"] is False
# The point picked: both turn, halfway each.
ops.anim.motion_path_control_handle_move(frame="30", index="0", out="1", x=str(cx + 0.9), y=str(cy - 0.5), **args)
assert ops.anim.motion_path_handle_select(frame="30", control="0", **args) == {'FINISHED'}
before = handles(ball)
assert ops.anim.motion_path_controls_align(aligned="1") == {'FINISHED'}
h5 = handles(ball)
assert in_line(h5) and not close(h5["in"], before["in"], 1e-3) and not close(h5["out"], before["out"], 1e-3), (before, h5)

# --- G R S on a picked handle: its tip, about its point ----------------------------------------------------------------
assert ops.anim.motion_path_handle_select(frame="30", control="0", control_handle="2", **args) == {'FINISHED'}
was = handles(ball)
assert ops.anim.motion_path_keys_transform(kind="G", x="0.2", y="0.1") == {'FINISHED'}
assert close(handles(ball)["out"], (was["out"][0] + 0.2, was["out"][1] + 0.1), 1e-3), (was, handles(ball))
assert in_line(handles(ball)), "aligned: the other follows"
assert ops.anim.motion_path_keys_transform(kind="S", factor="2") == {'FINISHED'}
g = handles(ball)["out"]
assert ops.ed.undo() == {'FINISHED'}
s = handles(ball)["out"]
assert close(g, (s[0] * 2, s[1] * 2), 1e-3), ("S scales it about its point", s, g)


def turned(v, a):
    return (v[0] * math.cos(a) - v[1] * math.sin(a), v[0] * math.sin(a) + v[1] * math.cos(a))


# --- R S on a control point, as on a vector point: its handles turn and scale with it (alone, about it) ---------------
assert ops.anim.motion_path_handle_select(frame="30", control="0", **args) == {'FINISHED'}
was, at_was = handles(ball), key(ball, 30)["path_controls"][0]
assert ops.anim.motion_path_keys_transform(kind="R", angle=str(math.pi / 2)) == {'FINISHED'}
now = handles(ball)
assert close(key(ball, 30)["path_controls"][0], at_was, 1e-4), "alone: the point stays"
assert close(now["in"], turned(was["in"], math.pi / 2), 1e-3) and close(now["out"], turned(was["out"], math.pi / 2), 1e-3), \
    ("R turns both handles about it", was, now)
assert now["aligned"] == was["aligned"], "as in line as they were"
assert ops.anim.motion_path_keys_transform(kind="S", factor="0.5") == {'FINISHED'}
assert close(handles(ball)["out"], (now["out"][0] * 0.5, now["out"][1] * 0.5), 1e-3), ("S scales them", handles(ball))
assert close(handles(ball)["in"], (now["in"][0] * 0.5, now["in"][1] * 0.5), 1e-3)
assert ops.ed.undo() == {'FINISHED'} and ops.ed.undo() == {'FINISHED'}
assert close(handles(ball)["out"], was["out"], 1e-4), "undone"
# Worked-out handles (not set yet) turn too, becoming its own.
pair = mover({10: (0.0, 0.0), 30: (6.0, 0.0)})
a5 = dict(layer=str(pair.id))
assert ops.anim.motion_path_control_add(frame="16", **a5) == {'FINISHED'}
assert ops.anim.motion_path_control_add(frame="24", **a5) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="30", index="0", x="2", y="1", **a5) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="30", index="1", x="4", y="1", **a5) == {'FINISHED'}
both_was = key(pair, 30)["path_control_handles"]
assert not both_was[0]["set"] and not both_was[1]["set"], both_was
tips_was = [(both_was[j]["in"], both_was[j]["out"]) for j in (0, 1)]
assert ops.anim.motion_path_handle_select(frame="30", control="0", **a5) == {'FINISHED'}
assert ops.anim.motion_path_handle_select(frame="30", control="1", extend="1", **a5) == {'FINISHED'}
assert ops.anim.motion_path_keys_transform(kind="R", angle=str(math.pi / 2)) == {'FINISHED'}
cs = key(pair, 30)["path_controls"]
assert not close(cs[0], (2.0, 1.0), 1e-2), ("two: they turn about their middle", cs)
hs = key(pair, 30)["path_control_handles"]
for j in (0, 1):
    assert hs[j]["set"] is True, ("turned: its own", hs[j])
    assert close(hs[j]["out"], turned(tips_was[j][1], math.pi / 2), 1e-3), ("and their handles with them", j, hs[j])

# --- Saved ---------------------------------------------------------------------------------------------------------------
kept = handles(ball)
path = os.path.join(tempfile.mkdtemp(), "control_handles.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
ball = next(l for l in layers if l.name == ball.name)
args = dict(layer=str(ball.id))
h6 = handles(ball)
assert h6["set"] == kept["set"] and h6["aligned"] == kept["aligned"] and close(h6["out"], kept["out"], 1e-4), (kept, h6)
assert ops.wm.tool_motion_path() == {'FINISHED'}

# --- X on a handle: smooth again; X on the point: deleted ------------------------------------------------------------
assert ops.anim.motion_path_handle_select(frame="30", control="0", control_handle="1", **args) == {'FINISHED'}
assert wm.event_simulate("X", area="Viewport")
h7 = handles(ball)
assert h7["set"] is False and h7["aligned"] is True and in_line(h7), h7
assert len(key(ball, 30)["path_controls"]) == 1, "the control point stays"
assert ops.anim.motion_path_handle_select(frame="30", control="0", **args) == {'FINISHED'}
assert wm.event_simulate("X", area="Viewport")
assert key(ball, 30)["path_controls"] == [] and key(ball, 30)["path_control_handles"] == []

# --- A new control point between others: its own smooth handles, the others keep theirs --------------------------------
two = mover({10: (0.0, 0.0), 30: (4.0, 0.0)})
a2 = dict(layer=str(two.id))
assert ops.anim.motion_path_control_add(frame="15", **a2) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="30", index="0", x="1", y="1", **a2) == {'FINISHED'}
assert ops.anim.motion_path_control_handle_move(frame="30", index="0", out="1", x="1.4", y="1.6", **a2) == {
    'FINISHED'}
first = key(two, 30)["path_control_handles"][0]
assert ops.anim.motion_path_control_add(frame="12", **a2) == {'FINISHED'}
hs = key(two, 30)["path_control_handles"]
assert len(hs) == 2 and hs[0]["set"] is False and hs[1]["set"] is True, hs
assert close(hs[1]["out"], first["out"], 1e-4), "the other keeps its own"

# --- The sculpt brushes fit them ------------------------------------------------------------------------------------
lump = mover({10: (0.0, 0.0), 30: (4.0, 0.0)})
a3 = dict(layer=str(lump.id))
assert ops.anim.motion_path_control_add(frame="20", **a3) == {'FINISHED'}
was = key(lump, 30)["path_control_handles"][0]
assert was["set"] is False
assert ops.anim.motion_path_stroke(brush="GRAB", points="2,0;2,0.8", radius="0.6", strength="1") == {'FINISHED'}
assert key(lump, 30)["path_controls"][0][1] > 0.1, ("the brush moved the control point", key(lump, 30))
now = key(lump, 30)["path_control_handles"][0]
assert now["set"] is True, ("reshaped by the brush: its own", now)
# --- Alt on a picked handle keeps it: the Alt click's pick lets the handles go only once it picks something -----------
alt = mover({10: (0.0, 0.0), 30: (4.0, 0.0)})
a4 = dict(layer=str(alt.id))
assert ops.anim.motion_path_control_add(frame="20", **a4) == {'FINISHED'}
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert ops.anim.motion_path_handle_select(frame="30", control="0", control_handle="2", **a4) == {'FINISHED'}
handle_pick = wm.motion_path_handle_picks()
assert [p["control_handle"] for p in handle_pick] == [2], handle_pick
doc.frame_current = 10
assert not wm.motion_path_pick(2.0, 3.0), "nothing there"
assert wm.motion_path_handle_picks() == handle_pick, ("a pick of nothing keeps the handle picked",
                                                      wm.motion_path_handle_picks())
assert wm.motion_path_pick(0.1, 0.0), "the drawing's point"
assert wm.motion_path_handle_picks() == [], "a point picked: the handles let go"
print("Motion path control handles: ok")
