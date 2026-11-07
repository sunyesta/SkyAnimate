"""The Motion Path tool's shape brushes are the Sculpt tool's Grab, Smooth and Surface Smooth on the path as a line:
sampled densely when a stroke starts, keys and control points anchored to their samples, the path given back from
the sculpted line (keys, their handles, control points). No key is added or removed, keys stay on their frames, the
setup key stays, and each stretch reshaped keeps its timing (Custom, by length along it).

Run with:  OpenAnimate --background --python tests/test_motion_path_sculpt.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def frames(layer):
    return [k["frame"] for k in layer.keyframes("position")]


def key(layer, frame):
    return next(k for k in layer.keyframes("position") if k["frame"] == frame)


def where(layer, frame):
    doc.frame_current = frame
    return (layer.location_x, layer.location_y)


def fresh_layer(keys, interp="LINEAR"):
    ops.layer.new()
    l = layers.active
    l.add_curve([(0.0, 0.0), (0.1, 0.0)])
    l.key_whole_drawing = False
    doc.frame_current = 0
    l.location_x, l.location_y = -3.0, -3.0
    for other in layers:
        other.selected = other.id == l.id
    layers.active = l
    for f, (x, y) in keys.items():
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(l.id)) == {'FINISHED'}
        l.keyframe_set("position", f, interp=interp)
    return l


def shares(layer, a, b):
    """Each frame's share of the way along the stretch a..b, by length (fine steps between frames along the line
    through them)."""
    pts = [where(layer, f) for f in range(a, b + 1)]
    run = [0.0]
    for p, q in zip(pts, pts[1:]):
        run.append(run[-1] + math.dist(p, q))
    return [r / run[-1] for r in run]


def wiggle(layer, a, b):
    """How far the path strays from the straight line y = 0 between frames a and b."""
    return max(abs(where(layer, f)[1]) for f in range(a, b + 1))


doc.motion_paths = True
doc.path_before, doc.path_after = 40, 40
doc.auto_key = False
assert ops.wm.tool_motion_path() == {'FINISHED'}

# --- The brushes and their settings ----------------------------------------------------------------------------------
assert ops.anim.motion_path_brush_set(brush="SURFACE_SMOOTH", radius="90", strength="0.6", brush_falloff="SPHERE",
                                      sensitivity="0.4") == {'FINISHED'}
s = wm.path_brush_settings()
assert s["brush"] == "SURFACE_SMOOTH" and s["brush_falloff"] == "SPHERE" and close(s["sensitivity"], 0.4), s
assert ops.anim.motion_path_brush_set(brush="GRAB") == {'FINISHED'}
assert wm.path_brush_settings()["brush_falloff"] == "SMOOTH", "each brush its own falloff"

# --- Grab bends a stretch between keys; the keys stay; the dots keep their timing ----------------------------------
line = fresh_layer({10: (0.0, 0.0), 20: (1.0, 0.0), 30: (2.0, 0.0)})
before = shares(line, 10, 20)
assert ops.anim.motion_path_stroke(brush="GRAB", points="0.5,0;0.5,0.3", radius="0.3", strength="1") == {'FINISHED'}
assert where(line, 15)[1] > 0.05, ("the middle of the stretch follows (as far as its handles can bend it)", where(line, 15))
assert frames(line) == [0, 10, 20, 30], "no keys added or removed"
assert close(where(line, 10), (0.0, 0.0)) and close(where(line, 20), (1.0, 0.0)), "the keys outside the brush stay"
after = shares(line, 10, 20)
assert all(abs(x - y) < 0.03 for x, y in zip(before, after)), ("the dots keep their share of the way", before, after)
assert key(line, 20)["interp"] == "CUSTOM", key(line, 20)
assert close(where(line, 30), (2.0, 0.0)), "a stretch the brush didn't touch, as it was"
ops.ed.undo()
assert abs(where(line, 15)[1]) < 1e-4, "one undo step"

# A key under the brush moves with it, its handles fitted (Aligned).
assert ops.anim.motion_path_stroke(brush="GRAB", points="1,0;1,0.4", radius="0.4", strength="1") == {'FINISHED'}
assert close(where(line, 20), (1.0, 0.4), 0.02), where(line, 20)
assert key(line, 20)["path_handles"] == "ALIGNED", key(line, 20)
assert frames(line) == [0, 10, 20, 30]

# On a Smooth path (its timing not by length) a stretch reshaped still keeps its dots' share of the way.
smooth = fresh_layer({10: (0.0, 0.0), 20: (1.0, 0.0), 30: (2.0, 0.0)}, interp="SMOOTH")
before = shares(smooth, 10, 20)
assert ops.anim.motion_path_stroke(brush="GRAB", points="1,0;1,0.4", radius="0.4", strength="1") == {'FINISHED'}
assert where(smooth, 20)[1] > 0.3, where(smooth, 20)
after = shares(smooth, 10, 20)
assert all(abs(x - y) < 0.03 for x, y in zip(before, after)), ("Smooth: the dots keep their share", before, after)

# --- The setup key (frame 0) never moves --------------------------------------------------------------------------
rest = fresh_layer({10: (0.3, 0.0), 20: (1.0, 0.0)})
doc.frame_current = 0
was = (rest.location_x, rest.location_y)
assert ops.anim.motion_path_stroke(brush="GRAB", points=f"{was[0]},{was[1]};{was[0]},{was[1] + 0.5}", radius="0.4",
                                   strength="1") == {'FINISHED'}
assert close(where(rest, 0), was), ("the setup stays", where(rest, 0))
# Its out handle (how the path leaves the setup) is the brush's to reshape, its point pinned.
setup = fresh_layer({10: (0.0, 0.0), 20: (1.0, 0.0)})
doc.frame_current = 0
start = (setup.location_x, setup.location_y)
out_was = key(setup, 0)["path_out"]
mid = (start[0] * 0.75, start[1] * 0.75)
assert ops.anim.motion_path_stroke(brush="GRAB", points=f"{mid[0]},{mid[1]};{mid[0] - 0.4},{mid[1] + 0.6}",
                                   radius="1.5", strength="1") == {'FINISHED'}
assert close(where(setup, 10), (0.0, 0.0), 1e-6), "the key after out of the brush's reach: only the setup's handle"
assert close(where(setup, 0), start, 1e-6), ("the setup point never moves", where(setup, 0), start)
assert not close(key(setup, 0)["path_out"], out_was, 1e-3), ("its out handle reshaped", out_was, key(setup, 0))
assert frames(setup) == [0, 10, 20]

# --- Smooth and Surface Smooth take a wiggle out, keys on their frames -------------------------------------------
wiggly = {10 + 2 * i: (0.1 * i, 0.06 if i % 2 else -0.06) for i in range(11)}
for brush in ("SMOOTH", "SURFACE_SMOOTH"):
    w = fresh_layer(wiggly, interp="SMOOTH")
    was = wiggle(w, 16, 24)
    points = ";".join(f"{0.1 * i},0" for i in range(4, 7) for _ in range(4))
    assert ops.anim.motion_path_stroke(brush=brush, points=points, radius="0.3", strength="1",
                                       sensitivity="1") == {'FINISHED'}, brush
    assert wiggle(w, 16, 24) < was * 0.7, (brush, "the wiggle smaller", was, wiggle(w, 16, 24))
    assert frames(w) == [0] + sorted(wiggly), (brush, frames(w))
    assert close(where(w, 10), wiggly[10]) and close(where(w, 30), wiggly[30]), (brush, "ends outside, as they were")

# --- The path's ends are the line's: a path showing a later stretch (keys before and after it) keeps its ends --------
# The brush works on the path as shown, as the Sculpt tool on a drawn curve: Smooth and Surface Smooth keep its first
# and last keys where they are, though there are keys beyond them (the line ran on through those, and the path's first
# key, a point in its middle then, was smoothed off where the path starts).
long_wiggle = {10 + 2 * i: (0.1 * i, 0.06 if i % 2 else -0.06) for i in range(21)}
for brush in ("SMOOTH", "SURFACE_SMOOTH"):
    w = fresh_layer(long_wiggle, interp="SMOOTH")
    doc.path_before, doc.path_after = 5, 5
    doc.frame_current = 30
    assert wm.motion_path_range() == (20, 40), wm.motion_path_range()
    outside = {f: key(w, f)["value"] for f in long_wiggle if f < 20 or f > 40}
    for end in (20, 40):
        x, y = long_wiggle[end]
        points = ";".join(f"{x},{y}" for _ in range(6))
        assert ops.anim.motion_path_stroke(brush=brush, points=points, radius="0.35", strength="1",
                                           sensitivity="1") == {'FINISHED'}, brush
        assert close(key(w, end)["value"], (x, y), 1e-5), (brush, "the path's end stays", end, key(w, end)["value"])
    assert key(w, 22)["value"] != (long_wiggle[22][0], long_wiggle[22][1]), (brush, "the rest smoothed")
    assert all(close(key(w, f)["value"], v, 1e-6) for f, v in outside.items()), (brush, "keys off the path stay")
    assert frames(w) == [0] + sorted(long_wiggle), (brush, frames(w))
    # An end key's handles in line: the one off the path turns with the one the brush shaped.
    k20 = key(w, 20)
    hi, ho = k20["path_in"], k20["path_out"]
    assert k20["path_handles"] == "ALIGNED", k20
    assert abs(hi[0] * ho[1] - hi[1] * ho[0]) < 1e-6 * (1 + math.hypot(*hi) * math.hypot(*ho)), (brush, "in line", k20)
    assert hi[0] * ho[0] + hi[1] * ho[1] < 0, (brush, "opposite", k20)
# Broken, the end key's handle off the path stays, and so does its stretch (its timing untouched).
w = fresh_layer(long_wiggle, interp="SMOOTH")
doc.frame_current = 30
doc.path_before, doc.path_after = 5, 5
assert ops.anim.motion_path_select(frame="20", layer=str(w.id)) == {'FINISHED'}
assert ops.anim.motion_path_handles_type(mode="BROKEN") == {'FINISHED'}
was = key(w, 20)
x, y = long_wiggle[20]
assert ops.anim.motion_path_stroke(brush="SURFACE_SMOOTH", points=";".join(f"{x},{y}" for _ in range(6)), radius="0.35",
                                   strength="1", sensitivity="1") == {'FINISHED'}
now = key(w, 20)
assert now["path_out"] != was["path_out"], ("the handle on the path reshaped", was, now)
assert close(now["path_in"], was["path_in"], 1e-6) and now["interp"] == "SMOOTH", ("the stretch before as it was", now)
doc.path_before, doc.path_after = 40, 40

# --- Surface Smooth's ghost: the fit a stroke moves the path toward, as the Sculpt tool shows it ------------------------
g = fresh_layer(wiggly, interp="SMOOTH")
assert ops.anim.motion_path_brush_set(brush="SURFACE_SMOOTH", sensitivity="1") == {'FINISHED'}
ghost = wm.motion_path_surface_ghost()
assert ghost, "the wiggly path has a fit apart from it"
# Each point's fit, by where it is now (the segments' ends).
fit_of = {}
for s in ghost:
    fit_of[tuple(round(v, 5) for v in s["now"][0])] = s["fit"][0]
    fit_of[tuple(round(v, 5) for v in s["now"][3])] = s["fit"][3]
# The line runs from the setup (frame 0, shown) to the last key.
ends = (key(g, 0)["value"], wiggly[30])
for e in ends:
    at = tuple(round(v, 5) for v in e)
    assert at in fit_of and close(fit_of[at], e, 1e-5), ("the path's ends stay in the fit", e, fit_of.get(at))
inner = [f for f in sorted(wiggly) if f < 30]
moved = [f for f in inner if not close(fit_of.get(tuple(round(v, 5) for v in wiggly[f]), wiggly[f]), wiggly[f], 1e-4)]
assert moved, ("the wiggles' keys have a fit elsewhere", fit_of)
# A full stroke (Constant falloff over all of it, full strength, one whole step) takes every key to its fit.
before = {f: key(g, f)["value"] for f in inner}
assert ops.anim.motion_path_stroke(brush="SURFACE_SMOOTH", points="1,0", radius="100", strength="1",
                                   brush_falloff="CONSTANT", sensitivity="1", amount="1") == {'FINISHED'}
for f in inner:
    target = fit_of.get(tuple(round(v, 5) for v in before[f]), before[f])
    assert close(key(g, f)["value"], target, 1e-4), ("the stroke goes where the ghost showed", f, key(g, f)["value"],
                                                    target)
assert ops.anim.motion_path_brush_set(brush="GRAB") == {'FINISHED'}

# --- A control point rides with the path it shapes ----------------------------------------------------------------
c = fresh_layer({10: (0.0, 0.0), 20: (1.0, 0.0)})
doc.frame_current = 15
assert ops.anim.motion_path_control_add(frame="15", layer=str(c.id)) == {'FINISHED'}
stored = key(c, 20)["path_controls"]
assert len(stored) == 1, stored
assert ops.anim.motion_path_stroke(brush="GRAB", points="0.5,0;0.5,0.3", radius="0.25", strength="1") == {
    'FINISHED'}
moved = key(c, 20)["path_controls"]
assert len(moved) == 1 and moved[0][1] > stored[0][1] + 0.1, ("the control point went up", stored, moved)

# --- A Noisy stretch keeps its noise ---------------------------------------------------------------------------------
n = fresh_layer({10: (0.0, 0.0), 20: (1.0, 0.0), 30: (2.0, 0.0)})
for f in (10, 20, 30):
    n.keyframe_set("position", f, interp="NOISY", noise_amplitude=0.2)
before = [where(n, f) for f in range(10, 31)]
assert max(abs(y) for _, y in before) > 0.02, "noisy"
doc.frame_current = 20
assert ops.anim.motion_path_stroke(brush="GRAB", points="1,0;1,0.1", radius="0.3", strength="0.01",
                                   layer=str(n.id)) == {'FINISHED'}
after = [where(n, f) for f in range(10, 31)]
worst = max(math.dist(p, q) for p, q in zip(before, after))
assert worst < 0.01, ("a tiny Grab took the noise away", worst, [k["interp"] for k in n.keyframes("position")])
assert key(n, 20)["interp"] == "NOISY", key(n, 20)
print("Motion path sculpt: ok")
