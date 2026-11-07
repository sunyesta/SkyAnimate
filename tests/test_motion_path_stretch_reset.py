"""Reset Stretch (the Motion Path tool's right-click menu on a path dot or control point): the stretch made a straight
line at an even speed, or only straightened (its timing kept), or only evened out (its shape kept). Its control points
go; the keys stay on their frames and where they are, and the stretches beside them keep their shape.

Run with:  OpenAnimate --background --python tests/test_motion_path_stretch_reset.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def where(layer, frame):
    doc.frame_current = frame
    return (layer.location_x, layer.location_y)


def key(layer, frame):
    return next(k for k in layer.keyframes("position") if k["frame"] == frame)


def frames(layer):
    return [k["frame"] for k in layer.keyframes("position")]


def lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def shares(points):
    run = [0.0]
    for p, q in zip(points, points[1:]):
        run.append(run[-1] + math.dist(p, q))
    return [r / run[-1] for r in run]


def curved(eased=True):
    """Keys on 10, 20 and 30, Smooth; the stretch 10-20 bent up through a control point, so it's a curve with uneven
    timing (eased, or Smooth's, which goes by the stretches' lengths)."""
    ops.layer.new()
    l = layers.active
    l.add_curve([(0.0, 0.0), (0.1, 0.0)])
    l.key_whole_drawing = False
    doc.frame_current = 0
    l.location_x, l.location_y = -1.0, 0.0
    for other in layers:
        other.selected = other.id == l.id
    layers.active = l
    for f, (x, y) in {10: (0.0, 0.0), 20: (1.0, 0.0), 30: (2.0, 1.0)}.items():
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(l.id)) == {'FINISHED'}
        l.keyframe_set("position", f, interp="SMOOTH")
    assert ops.anim.motion_path_control_add(frame="15", layer=str(l.id)) == {'FINISHED'}
    assert ops.anim.motion_path_control_move(frame="20", index="0", x="0.5", y="0.6", layer=str(l.id)) == {'FINISHED'}
    if eased:
        # (an undo step of its own, so undoing a reset comes back to it)
        assert ops.anim.motion_path_key_interp(frame="20", interp="EASE_IN_OUT", layer=str(l.id)) == {'FINISHED'}
    assert len(key(l, 20)["path_controls"]) == 1
    return l


doc.motion_paths = True
doc.auto_key = False
doc.path_before, doc.path_after = 40, 40
assert ops.wm.tool_motion_path() == {'FINISHED'}

# --- Reset Stretch: a straight line at an even speed ------------------------------------------------------------------
l = curved()
a, b = where(l, 10), where(l, 20)
assert max(abs(where(l, f)[1]) for f in range(10, 21)) > 0.2, "bent before"
after_stretch = [where(l, f) for f in range(20, 31)]
before_stretch = [where(l, f) for f in range(0, 11)]
assert ops.anim.motion_path_stretch_reset(frame="14", layer=str(l.id)) == {'FINISHED'}
for f in range(10, 21):
    assert close(where(l, f), lerp(a, b, (f - 10) / 10)), ("straight and even", f, where(l, f), lerp(a, b, (f - 10) / 10))
assert key(l, 20)["path_controls"] == [] and key(l, 20)["interp"] == "LINEAR", key(l, 20)
assert frames(l) == [0, 10, 20, 30], frames(l)
assert all(close(p, q) for p, q in zip(after_stretch, [where(l, f) for f in range(20, 31)])), "the stretch after as it was"
assert all(close(p, q) for p, q in zip(before_stretch, [where(l, f) for f in range(0, 11)])), "the stretch before too"
ops.ed.undo()
assert len(key(l, 20)["path_controls"]) == 1 and abs(where(l, 15)[1]) > 0.2, "one undo step"

# --- Reset Shape: straight, its dots at the same share of the way as before ------------------------------------------
before = [where(l, f) for f in range(10, 21)]
was = shares(before)
assert not all(close(s, (f - 10) / 10, 0.02) for f, s in zip(range(10, 21), was)), ("uneven before", was)
assert ops.anim.motion_path_stretch_reset(frame="17", what="SHAPE", layer=str(l.id)) == {'FINISHED'}
for f, s in zip(range(10, 21), was):
    # (the shares here are by chords between frames, a touch short of the curve on a bend)
    assert close(where(l, f), lerp(a, b, s), 1e-2), ("straight, the timing kept", f, where(l, f), lerp(a, b, s))
assert key(l, 20)["path_controls"] == [], key(l, 20)
assert all(close(p, q) for p, q in zip(after_stretch, [where(l, f) for f in range(20, 31)])), "the stretch after kept"
ops.ed.undo()

# Smooth timing goes by the stretches' lengths, which straightening shortens: the timing is kept all the same.
sm = curved(eased=False)
sa, sb = where(sm, 10), where(sm, 20)
was_smooth = shares([where(sm, f) for f in range(10, 21)])
assert ops.anim.motion_path_stretch_reset(frame="15", what="SHAPE", layer=str(sm.id)) == {'FINISHED'}
for f, s in zip(range(10, 21), was_smooth):
    assert close(where(sm, f), lerp(sa, sb, s), 1e-2), ("Smooth: the timing kept", f, where(sm, f), lerp(sa, sb, s))

# --- Reset Timing: the same curve, an even speed along it -------------------------------------------------------------
fine = []
for f in range(10, 20):
    p, q = where(l, f), where(l, f + 1)
    fine += [lerp(p, q, t / 20) for t in range(20)]
assert ops.anim.motion_path_stretch_reset(frame="12", what="TIMING", layer=str(l.id)) == {'FINISHED'}
now = [where(l, f) for f in range(10, 21)]
# On the curve it was (as near as the frames' chords show it: the eased frames are far apart in the middle, so the
# chords cut the bend a little; straightened, it would be 0.6 off).
assert all(min(math.dist(p, q) for q in fine) < 0.05 for p in now), "on the curve it was"
assert all(close(s, (f - 10) / 10, 0.02) for f, s in zip(range(10, 21), shares(now))), ("even", shares(now))
assert len(key(l, 20)["path_controls"]) == 1, "its control point stays: the shape is kept"
assert all(close(p, q) for p, q in zip(after_stretch, [where(l, f) for f in range(20, 31)]))
ops.ed.undo()

# --- A control point's stretch: by its key's frame --------------------------------------------------------------------
assert ops.anim.motion_path_stretch_reset(frame="20", layer=str(l.id)) == {'FINISHED'}
assert key(l, 20)["path_controls"] == [] and close(where(l, 15), lerp(a, b, 0.5)), where(l, 15)
ops.ed.undo()

# --- A key turns a corner only where its sides no longer line up -----------------------------------------------------
def path(keys):
    ops.layer.new()
    s = layers.active
    s.add_curve([(0.0, 0.0), (0.1, 0.0)])
    s.key_whole_drawing = False
    for other in layers:
        other.selected = other.id == s.id
    layers.active = s
    doc.frame_current = 0
    s.location_x, s.location_y = -1.0, 0.0
    for f, (x, y) in keys.items():
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(s.id)) == {'FINISHED'}
        s.keyframe_set("position", f, interp="SMOOTH")
    return s


s = path({10: (0.0, 0.0), 20: (1.0, 0.0), 30: (2.0, 0.0)})
assert ops.anim.motion_path_stretch_reset(frame="25", layer=str(s.id)) == {'FINISHED'}
assert key(s, 20)["path_handles"] == "ALIGNED", ("in line already: no corner", key(s, 20))
bent = path({10: (0.0, 0.0), 20: (1.0, 0.0), 30: (2.0, 1.0)})
kept = [where(bent, f) for f in range(10, 21)]
assert ops.anim.motion_path_stretch_reset(frame="25", layer=str(bent.id)) == {'FINISHED'}
assert key(bent, 20)["path_handles"] == "BROKEN", ("a corner where the path turns", key(bent, 20))
assert all(close(p, q) for p, q in zip(kept, [where(bent, f) for f in range(10, 21)])), "the stretch before kept"
for f in range(20, 31):
    assert close(where(bent, f), lerp((1.0, 0.0), (2.0, 1.0), (f - 20) / 10)), f

# --- Shape only on a plain Smooth stretch: its timing (timed by its curve) kept ----------------------------------------
eased = path({10: (0.0, 0.0), 20: (1.0, 0.0), 30: (5.0, 0.0)})
before = [where(eased, f) for f in range(20, 31)]
assert ops.anim.motion_path_stretch_reset(frame="25", what="SHAPE", layer=str(eased.id)) == {'FINISHED'}
after = [where(eased, f) for f in range(20, 31)]
assert all(close(p, q) for p, q in zip(before, after)), ("Reset Shape evened a straight Smooth stretch's timing",
                                                        before, after)

# --- Not between keys: nothing --------------------------------------------------------------------------------------
assert ops.anim.motion_path_stretch_reset(frame="35", layer=str(s.id)) == {'CANCELLED'}, "past the last key"
assert ops.anim.motion_path_stretch_reset(frame="5", what="NONSENSE", layer=str(s.id)) == {'CANCELLED'}

print("Motion path stretch reset: ok")
