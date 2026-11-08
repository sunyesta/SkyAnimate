"""A Custom stretch's timing is a curve (Key::timing, anim/timing_curve.hh): how far along against the share of the
stretch's time, kept with as few points as put every frame's dot within a hair of where its timing put it (painted
speeds, a slid dot, a shape all become such a curve), so it stretches with the stretch when its keys move. The Path
Timing graph edits it: Edit mode moves its points and handles (anim.timing_point_move, anim.timing_handle_move: an
aligned point's other handle turns with it unless free=1), adds (anim.timing_point_add keeps the curve's shape) and
deletes them (anim.timing_point_delete, never an end); Sculpt mode's Grab and Smooth (anim.timing_sculpt) brush it as a
drawing's sculpt does (samples moved, the curve fitted back to them, the pieces a stroke didn't move left as they were),
Lock X keeping it from moving in time, Dyntopo leaving as few points as hold every frame. Files store the curve (format 7); an
older file's per-frame speeds open as the curve they make.

Run with:  OpenAnimate --background --python tests/test_timing_curve.py
"""

import json
import math
import os
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def start():
    assert ops.wm.read_homefile() == {'FINISHED'}
    doc.motion_paths = True
    doc.auto_key = False
    assert ops.wm.tool_motion_path() == {'FINISHED'}
    assert ops.anim.motion_path_brush_set(brush="EDIT", proportional="0") == {'FINISHED'}


def key(layer, frame):
    return next(k for k in layer.keyframes("position") if k["frame"] == frame)


def line_layer(keys):
    """A new layer, its origin keyed along the x axis, {frame: x}, every key Linear."""
    ops.layer.new()
    layer = layers.active
    layer.add_curve([(0.0, 0.0), (0.1, 0.0)])
    doc.frame_current = 0
    # (the setup frame on the same line, so the path is straight whatever its handles)
    layer.location_x, layer.location_y = -1.0, 0.0
    for other in layers:
        other.selected = other.id == layer.id
    layers.active = layer
    for f, x in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y="0", layer=str(layer.id)) == {'FINISHED'}
        if f not in [k["frame"] for k in layer.keyframes("position")]:
            doc.frame_current = f
            layer.keyframe_insert("position", f)
        layer.keyframe_set("position", f, interp="LINEAR")
    return layer


def played(layer, first, last):
    """How far along x (0 to 1) the layer is on each frame first..last."""
    out = []
    for f in range(first, last + 1):
        doc.frame_current = f
        out.append(layer.location_x)
    return out


def progress_of(speeds):
    total = sum(speeds)
    u, out = 0.0, [0.0]
    for s in speeds:
        u += s / total
        out.append(u)
    return out


def args(layer, **kw):
    a = dict(layer=str(layer.id), frame="15")
    a.update({k: str(v) for k, v in kw.items()})
    return a


# --- Painted speeds become a curve of few points, every frame where they put it ---------------------------------------
start()
box = line_layer({10: 0.0, 30: 1.0})
speeds = [1.0 + 0.6 * math.sin(i / 3.0) for i in range(20)]
box.keyframe_set("position", 30, speeds=speeds)
k = key(box, 30)
assert k["interp"] == "CUSTOM"
print("20 painted speeds:", len(k["timing"]), "curve points")
assert 2 < len(k["timing"]) <= 4, ("a smooth timing needs few points", len(k["timing"]))
assert close(played(box, 10, 30), progress_of(speeds), 5e-4), "every frame where its speeds put it"
assert k["timing"][0][:2] == (0.0, 0.0) and k["timing"][-1][:2] == (1.0, 1.0), "from the stretch's start to its key"
# Noise has little to leave out: nearly every frame a point.
noisy = [1.0, 2.0, 0.5, 1.7, 0.6, 1.9, 0.4, 1.5, 0.8, 1.2, 0.3, 2.0, 0.9, 1.1, 0.5, 1.8, 0.7, 1.4, 0.6, 1.0]
box.keyframe_set("position", 30, speeds=noisy)
assert close(played(box, 10, 30), progress_of(noisy), 5e-4)
assert len(key(box, 30)["timing"]) >= 18, len(key(box, 30)["timing"])
# Even speeds: no curve at all (it goes evenly).
box.keyframe_set("position", 30, speeds=[1.0] * 20)
assert "timing" not in key(box, 30), key(box, 30)
assert close(played(box, 10, 30), [f / 20 for f in range(21)])

# The shapes are a curve of their ends alone.
for shape, f in (("EASE_IN", lambda t: t * t), ("EASE_OUT", lambda t: 1 - (1 - t) ** 2),
                 ("EASE_IN_OUT", lambda t: t * t * (3 - 2 * t))):
    assert ops.anim.motion_path_stretch_shape(shape=shape, **args(box)) == {'FINISHED'}
    assert len(key(box, 30)["timing"]) == 2, (shape, key(box, 30)["timing"])
    assert close(played(box, 10, 30), [f(i / 20) for i in range(21)], 3e-3), shape

# --- The curve stretches with the stretch -------------------------------------------------------------------------------
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", **args(box)) == {'FINISHED'}
ops.anim.keys_deselect_all()
assert ops.anim.keys_select(frame_start="30", layers=str(box.id)) == {'FINISHED'}
assert ops.anim.keys_offset(frames="-10") == {'FINISHED'}
assert len(key(box, 20)["timing"]) == 2 and len(key(box, 20)["speeds"]) == 10
assert close(played(box, 10, 20), [(i / 10) ** 2 for i in range(11)], 3e-3), "Ease In over its 10 frames now"

# --- Edit mode: points and handles ---------------------------------------------------------------------------------------
start()
box = line_layer({10: 0.0, 20: 1.0})
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN_OUT", **args(box)) == {'FINISHED'}
before = played(box, 10, 20)
# A point added keeps the curve's shape.
assert ops.anim.timing_point_add(**args(box, x=0.5)) == {'FINISHED'}
t = key(box, 20)["timing"]
assert len(t) == 3 and close(t[1][0], 0.5), t
assert close(played(box, 10, 20), before), "adding a point leaves every frame where it was"
# Moved: the frames follow, the ends stay the keys.
assert ops.anim.timing_point_move(**args(box, index=1, x=0.5, y=0.8)) == {'FINISHED'}
t = key(box, 20)["timing"]
assert close(t[1][:2], (0.5, 0.8)), t
assert close(played(box, 15, 15)[0], 0.8), "half way through, 80% along"
assert played(box, 20, 20)[0] == 1.0 and close(played(box, 10, 10)[0], 0.0)
# Kept between its neighbours in time.
assert ops.anim.timing_point_move(**args(box, index=1, x=1.5, y=0.8)) == {'FINISHED'}
assert key(box, 20)["timing"][1][0] < 1.0, key(box, 20)["timing"]
assert ops.anim.timing_point_move(**args(box, index=1, x=0.5, y=0.8)) == {'FINISHED'}
assert ops.anim.timing_point_move(**args(box, index=0, x=0.2, y=0.3)) == {'CANCELLED'}, "an end is a key"
assert ops.anim.timing_point_move(**args(box, index=2, x=0.2, y=0.3)) == {'CANCELLED'}, "an end is a key"

# A handle moved turns its aligned partner, keeping its length; Alt (free=1) moves it alone.
t = key(box, 20)["timing"]
(_, _, h_in, h_out, aligned) = t[1]
assert aligned
assert ops.anim.timing_handle_move(**args(box, index=1, side="OUT", x=0.15, y=0.15)) == {'FINISHED'}
(_, _, now_in, now_out, _) = key(box, 20)["timing"][1]
assert close(now_out, (0.15, 0.15)), now_out
assert close(math.atan2(-now_in[1], -now_in[0]), math.atan2(0.15, 0.15)), ("the in handle turned into line", now_in)
assert close(math.hypot(*now_in), math.hypot(*h_in)), "keeping its length"
assert ops.anim.timing_handle_move(**args(box, index=1, side="OUT", x=0.1, y=0.0, free=1)) == {'FINISHED'}
(_, _, free_in, free_out, _) = key(box, 20)["timing"][1]
assert close(free_out, (0.1, 0.0)) and close(free_in, now_in), "Alt: the other handle stays"
assert key(box, 20)["timing"][1][4] is False, "moved alone, the point is broken"
# Broken, a plain drag of either handle leaves the other where it is (it doesn't snap back into line).
assert ops.anim.timing_handle_move(**args(box, index=1, side="IN", x=-0.05, y=-0.12)) == {'FINISHED'}
(_, _, plain_in, plain_out, _) = key(box, 20)["timing"][1]
assert close(plain_in, (-0.05, -0.12)) and close(plain_out, (0.1, 0.0)), ("broken: no snap to aligned", plain_in, plain_out)
# A handle points its own way and reaches no further than its segment (the curve stays a function of time).
assert ops.anim.timing_handle_move(**args(box, index=1, side="OUT", x=-0.2, y=0.1)) == {'FINISHED'}
assert key(box, 20)["timing"][1][3][0] >= 0.0, key(box, 20)["timing"][1]
pointing_back = key(box, 20)["timing"]
assert ops.anim.timing_handle_move(**args(box, index=1, side="OUT", x=3.0, y=0.0)) == {'FINISHED'}
assert key(box, 20)["timing"][1][3][0] <= 0.5 + 1e-6, key(box, 20)["timing"][1]
assert ops.anim.timing_handle_move(**args(box, index=0, side="IN", x=0.1, y=0.1)) == {'CANCELLED'}, "the start has no in"
# One undo step each.
ops.ed.undo()
assert key(box, 20)["timing"] == pointing_back, "undone"
# Deleted: never an end.
assert ops.anim.timing_point_delete(**args(box, index=0)) == {'CANCELLED'}
assert ops.anim.timing_point_delete(**args(box, index=1)) == {'FINISHED'}
assert len(key(box, 20)["timing"]) == 2
# On an even stretch, a point makes it Custom from a straight curve.
plain = line_layer({10: 0.0, 20: 1.0})
assert key(plain, 20)["interp"] == "LINEAR"
assert ops.anim.timing_point_add(**args(plain, x=0.3, y=0.6)) == {'FINISHED'}
assert key(plain, 20)["interp"] == "CUSTOM" and len(key(plain, 20)["timing"]) == 3
assert close(played(plain, 13, 13)[0], 0.6)
# Off a stretch: nothing.
assert ops.anim.timing_point_add(layer=str(plain.id), frame="25", x="0.5") == {'CANCELLED'}

# --- Sculpt mode: Grab and Smooth, with Dyntopo ------------------------------------------------------------------------
start()
assert wm.path_brush_settings()["timing_dyntopo"] is False, "Dyntopo is off to start with"
box = line_layer({10: 0.0, 30: 1.0})
# (an undo step to go back to)
assert ops.anim.motion_path_stretch_shape(shape="EVEN", **args(box)) == {'FINISHED'}
even = key(box, 30)
# Grab across from frame 20 to 24, 1 frame a whole way along so the brush reaches 3 frames in time and a third up:
# the curve's middle moves later, so frame 20 is less far along.
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="20,0.5;24,0.5", radius=3, strength=1, aspect=6,
                                     dyntopo=1)) == {'FINISHED'}
k = key(box, 30)
assert k["interp"] == "CUSTOM", k
grabbed = played(box, 10, 30)
assert grabbed[10] < 0.5 - 0.05, ("the lump moved later in time", grabbed[10])
print("Grab stroke:", len(k["timing"]), "curve points")
assert len(k["timing"]) < 21, "decimated after the stroke"
ops.ed.undo()
assert key(box, 30) == even, "one undo step"
# Dragged far sideways, the timing slides along time with the brush: what was at frame 20 (half way) is at frame 27,
# smoothly, nothing piled up into a jump (samples moved one by one stuck against those they would pass).
path = ";".join("%g,0.5" % (20 + i * 0.25) for i in range(29))
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path=path, radius=4, strength=1, aspect=8,
                                     dyntopo=1)) == {'FINISHED'}
slid = played(box, 10, 30)
steps = [slid[i + 1] - slid[i] for i in range(20)]
print("Slid far: frame 27 at %.3f, biggest step %.3f" % (slid[17], max(steps)))
assert close(slid[17], 0.5, 0.04), ("the middle went with the brush", slid[17])
assert max(steps) < 0.2 and min(steps) > -0.05, ("no jump where it slid to", steps)
ops.ed.undo()
# Lock X: a stroke across moves nothing (so is no edit).
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="20,0.5;24,0.5", radius=3, strength=1, aspect=6,
                                     lock_x=1, dyntopo=1)) == {'CANCELLED'}
assert close(played(box, 10, 30), [f / 20 for f in range(21)]), "Lock X: never in time"
# Up and down it still moves.
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="20,0.5;20,0.7", radius=3, strength=1, aspect=6,
                                     lock_x=1, dyntopo=1)) == {'FINISHED'}
assert close(played(box, 20, 20)[0], 0.7, 2e-2), played(box, 20, 20)
# A wide, smooth lump lifted up: few points (the fit fits each piece's handles and takes out what it can, and a stroke
# is held to two pixels on a long path, not half of one: it used fifteen).
assert ops.anim.motion_path_stretch_shape(shape="EVEN", **args(box)) == {'FINISHED'}
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="20,0.5;20,0.7", radius=8, strength=1, aspect=20,
                                     lock_x=1, dyntopo=1)) == {'FINISHED'}
lumped = len(key(box, 30)["timing"])
print("Lifted lump:", lumped, "curve points")
assert lumped <= 6, ("a smooth lump needs few points", lumped)
# Smooth evens a bumpy timing out, and leaves a smooth curve few points.
box.keyframe_set("position", 30, speeds=noisy)


def roughness(u):
    return sum(abs(u[i - 1] - 2 * u[i] + u[i + 1]) for i in range(1, len(u) - 1))


rough = roughness(played(box, 10, 30))
assert ops.anim.timing_sculpt(**args(box, brush="SMOOTH", path="12,0.2;20,0.5;28,0.9", radius=6, strength=1,
                                     aspect=20, dyntopo=1)) == {'FINISHED'}
smooth = roughness(played(box, 10, 30))
print("Smooth: roughness %.3f -> %.3f, %d points" % (rough, smooth, len(key(box, 30)["timing"])))
assert smooth < rough * 0.7, (rough, smooth)
assert len(key(box, 30)["timing"]) < 21
assert ops.anim.timing_sculpt(**args(box, brush="PINCH", path="20,0.5")) == {'CANCELLED'}
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="")) == {'CANCELLED'}

# Grab in time moves what the circle is on, as Blender's and Maya's: a short drag sideways leaves the curve outside the
# circle as it was on both sides (with Dyntopo: without it a curve of two points is one piece, fitted again whole); a
# long one squeezes further ahead only, smoothly (no corner where the speed jumps), and the brush's size still counts
# (a reach both ways that grew with the drag and a straight falloff moved what the circle wasn't on, the same whatever
# the brush, and left kinks).
start()
box = line_layer({10: 0.0, 50: 1.0})
assert ops.anim.motion_path_stretch_shape(shape="EVEN", **args(box)) == {'FINISHED'}
even = played(box, 10, 50)
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="30,0.5;31.5,0.5", radius=4, strength=1, aspect=10,
                                     dyntopo=1)) == {'FINISHED'}
u = played(box, 10, 50)
assert close(u[:16], even[:16], 2e-3) and close(u[26:], even[26:], 2e-3), (
    "outside the circle as it was", [round(v - e, 4) for v, e in zip(u, even)])
ops.ed.undo()
slid = {}
for radius in (2, 5):
    assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="30,0.5;33,0.5;36,0.5", radius=radius, strength=1,
                                         aspect=10, dyntopo=1)) == {'FINISHED'}
    u = played(box, 10, 50)
    slid[radius] = u
    steps = [u[i + 1] - u[i] for i in range(40)]
    jump = max(abs(steps[i + 1] - steps[i]) for i in range(39))
    print("Grab 6 frames later, radius %d: steps %.3f..%.3f, biggest change from one frame's to the next %.4f" %
          (radius, min(steps), max(steps), jump))
    assert close(u[:20 - radius - 1], even[:20 - radius - 1], 2e-3), ("nothing moved behind the circle", radius, u)
    assert close(u[26], 0.5, 0.03), ("the middle went with the brush", radius, u[26])
    assert min(steps) > 0.0 and jump < 0.02, ("squeezed smoothly, no corner", radius, steps)
    ops.ed.undo()
assert max(abs(a - b) for a, b in zip(slid[2], slid[5])) > 0.02, "the brush's size counts"

# --- Sculpt without Dyntopo: as a drawing's sculpt, the brush moves samples of the curve and the curve is fitted back to
# them, every point where its sample went, only the pieces the stroke moved fitted again; none added or taken out -------
start()
box = line_layer({10: 0.0, 30: 1.0})
assert ops.anim.motion_path_stretch_shape(shape="EVEN", **args(box)) == {'FINISHED'}
# Pressed in the middle of a long piece, far from any point, the curve still follows the brush (its handles' slopes and
# lengths fitted to the samples; pushing the points and handle tips near the brush, it hardly moved).
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="20,0.5;20,0.7", radius=4, strength=1, aspect=6,
                                     lock_x=1)) == {'FINISHED'}
assert len(key(box, 30)["timing"]) == 2, key(box, 30)["timing"]
print("A long piece grabbed up: frame 20 at %.3f" % played(box, 20, 20)[0])
assert played(box, 20, 20)[0] > 0.56, ("the curve followed the brush", played(box, 20, 20))
ops.ed.undo()
# Five points: a stroke near the fourth leaves the pieces it didn't move as they were, but for the smooth point between
# (its handle on the piece not moved turns to stay in line with the one fitted, keeping its length).
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN_OUT", **args(box)) == {'FINISHED'}
for x in (0.2, 0.5, 0.8):
    assert ops.anim.timing_point_add(**args(box, x=x)) == {'FINISHED'}
five = key(box, 30)["timing"]
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="26,0.9;27,0.97", radius=2, strength=1,
                                     aspect=6)) == {'FINISHED'}
after = key(box, 30)["timing"]
assert len(after) == 5, after
assert after[0] == five[0] and after[1] == five[1] and close(after[2][:2], five[2][:2], 1e-6), (
    "pieces it didn't move as they were", five, after)
(ix, iy), (ox, oy) = after[2][2], after[2][3]
assert abs(math.hypot(ix, iy) - math.hypot(*five[2][2])) < 1e-5, ("the turned handle keeps its length", five[2], after[2])
assert abs(ix * oy - iy * ox) < 1e-6 and ix < 0 < ox, ("the smooth point beside the stroke stays in line", after[2])
assert close(after[3][:2], (0.85, five[3][1] + 0.07), 2e-3), ("the point went where its sample went", after[3])
hin, hout = after[3][2], after[3][3]
assert abs(hin[0] * hout[1] - hin[1] * hout[0]) < 1e-6 and hin[0] < 0 < hout[0], ("a smooth point's handles in line",
                                                                                after[3])
# With Dyntopo too: the points and pieces it didn't move stay as they were, and it has more where it moved the curve.
ops.ed.undo()
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="26,0.9;27,0.97", radius=2, strength=1, aspect=6,
                                     dyntopo=1)) == {'FINISHED'}
made = key(box, 30)["timing"]
assert len(made) > 5, made
assert made[0] == five[0] and made[1][:3] == five[1][:3], ("the start and the point at 0.2 as they were", five, made)
ops.ed.undo()
assert ops.anim.motion_path_stretch_shape(shape="EVEN", **args(box)) == {'FINISHED'}
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN_OUT", **args(box)) == {'FINISHED'}
assert ops.anim.timing_point_add(**args(box, x=0.5)) == {'FINISHED'}
before = key(box, 30)["timing"]
assert len(before) == 3, before
# Grab on the middle point, a frame later and a tenth up: it goes with the brush (its handles too).
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="20,0.5;21,0.6", radius=3, strength=1,
                                     aspect=6)) == {'FINISHED'}
after = key(box, 30)["timing"]
assert len(after) == 3, ("no points added or taken out", after)
assert close(after[1][:2], (0.55, 0.6), 2e-3), ("the point went with the brush", after[1])
hin, hout = after[1][2], after[1][3]
assert abs(hin[0] * hout[1] - hin[1] * hout[0]) < 1e-5 and hin[0] < 0 < hout[0], ("its handles still in line", after[1])
ops.ed.undo()
# Lock X: up only.
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="20,0.5;21,0.6", radius=3, strength=1, aspect=6,
                                     lock_x=1)) == {'FINISHED'}
assert close(key(box, 30)["timing"][1][:2], (0.5, 0.6), 2e-3), key(box, 30)["timing"][1]
ops.ed.undo()
# Smooth: still three points, the middle one pulled toward the curve around it.
bump = [(0.0, 0.0, (0, 0), (0.1, 0.0)), (0.5, 0.8, (-0.1, 0.0), (0.1, 0.0)), (1.0, 1.0, (-0.1, 0.0), (0, 0))]
box.keyframe_set("position", 30, timing=bump)
assert ops.anim.timing_sculpt(**args(box, brush="SMOOTH", path="18,0.8;22,0.8", radius=6, strength=1,
                                     aspect=20)) == {'FINISHED'}
smoothed = key(box, 30)["timing"]
assert len(smoothed) == 3 and smoothed[1][1] < 0.8 - 0.02, smoothed
# Without Dyntopo the ends never move.
assert smoothed[0][:2] == (0.0, 0.0) and smoothed[-1][:2] == (1.0, 1.0)
# Smooth across a whole ease never makes it steeper by a key, with Dyntopo or without (averaged with the neighbours on
# one side alone, the samples by a key were dragged away from it and the curve shot up to it in the last frame).
for dyn in (0, 1):
    for shape in ("EASE_IN", "EASE_OUT", "EASE_IN_OUT"):
        assert ops.anim.motion_path_stretch_shape(shape="EVEN", **args(box)) == {'FINISHED'}
        assert ops.anim.motion_path_stretch_shape(shape=shape, **args(box)) == {'FINISHED'}
        u = played(box, 10, 30)
        was = max(u[i + 1] - u[i] for i in range(20))
        assert ops.anim.timing_sculpt(**args(box, brush="SMOOTH", path="12,0.1;16,0.3;20,0.5;24,0.7;28,0.9", radius=8,
                                             strength=1, aspect=6, dyntopo=dyn)) == {'FINISHED'}
        u = played(box, 10, 30)
        steps = [u[i + 1] - u[i] for i in range(20)]
        print("Smooth across %s, Dyntopo %d: biggest step %.3f (was %.3f), at the ends %.3f %.3f" %
              (shape, dyn, max(steps), was, steps[0], steps[-1]))
        assert max(steps) <= was + 0.01 and min(steps) > -1e-3, (shape, dyn, was, steps)

# --- Alt drag on the curve: a point put there and dragged, one undo step (anim.timing_point_add with to_x=) ----------
start()
box = line_layer({10: 0.0, 30: 1.0})
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN_OUT", **args(box)) == {'FINISHED'}
shaped = key(box, 30)
assert ops.anim.timing_point_add(**args(box, x=0.4, to_x=0.6, y=0.7)) == {'FINISHED'}
pts = key(box, 30)["timing"]
assert len(pts) == 3 and close(pts[1][:2], (0.6, 0.7)), pts
ops.ed.undo()
assert key(box, 30) == shaped, "one undo step"

# --- Edit mode: picking several points and handles, G R S on them, and handle types (V, the right click menu) ----------
start()
box = line_layer({10: 0.0, 30: 1.0})
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN_OUT", **args(box)) == {'FINISHED'}
for x in (0.25, 0.5, 0.75):
    assert ops.anim.timing_point_add(**args(box, x=x)) == {'FINISHED'}
assert wm.timing_picks()["picks"] == [(3, "POINT")], ("a point added is picked alone", wm.timing_picks())
five = key(box, 30)["timing"]
assert len(five) == 5


def pts():
    return key(box, 30)["timing"]


def turn_of(h):
    """A handle's way as the graph shows it (20 frames; 10 frames a whole way along: aspect 10), in degrees."""
    return math.degrees(math.atan2(h[1] * 10, h[0] * 20))


def in_line(p):
    return abs(p[2][0] * p[3][1] - p[2][1] * p[3][0]) < 1e-6 and p[2][0] < 0 < p[3][0]


# A click picks one; Shift click adds and takes out; the active one follows the last clicked.
assert ops.anim.timing_select(**args(box, index=2)) == {'FINISHED'}
assert ops.anim.timing_select(**args(box, index=1, extend=1)) == {'FINISHED'}
assert sorted(wm.timing_picks()["picks"]) == [(1, "POINT"), (2, "POINT")] and wm.timing_picks()["active"] == 1
assert ops.anim.timing_select(**args(box, index=2, extend=1)) == {'FINISHED'}
assert wm.timing_picks()["picks"] == [(1, "POINT")], "Shift click takes a picked one out"
# G: the picked points move together; others and the ends stay. 2 frames later, 5% further along.
assert ops.anim.timing_select(**args(box, index=2, extend=1)) == {'FINISHED'}
assert ops.anim.timing_transform(mode="MOVE", x=2, y=0.05) == {'FINISHED'}
moved = pts()
for i in (1, 2):
    assert close(moved[i][:2], (five[i][0] + 0.1, five[i][1] + 0.05)), (i, five[i], moved[i])
    assert moved[i][2:4] == five[i][2:4], "its handles come with it"
assert moved[0] == five[0] and moved[3] == five[3] and moved[4] == five[4], "the rest stay"
ops.ed.undo()
assert pts() == five, "one undo step"
# Dragged far later they stop short of the next point not picked, still together.
assert ops.anim.timing_transform(mode="MOVE", x=40) == {'FINISHED'}
far = pts()
assert far[2][0] < five[3][0] and close(far[2][0] - five[2][0], far[1][0] - five[1][0]), (five, far)
ops.ed.undo()
# The ends never move (A picks every point and the ends' handles).
assert ops.anim.timing_select(**args(box, action="ALL")) == {'FINISHED'}
assert len(wm.timing_picks()["picks"]) == 5
assert ops.anim.timing_transform(mode="MOVE", x=1, y=0.1) == {'FINISHED'}
assert pts()[0][:2] == (0.0, 0.0) and pts()[-1][:2] == (1.0, 1.0)
ops.ed.undo()
# R: one point's handles turn about it, as the graph shows them, staying in line and as long.
assert ops.anim.timing_select(**args(box, index=2)) == {'FINISHED'}
assert ops.anim.timing_transform(mode="ROTATE", angle=20, aspect=10) == {'FINISHED'}
p, q = five[2], pts()[2]
assert close(turn_of(q[3]) - turn_of(p[3]), 20, 0.05) and close(turn_of(q[2]) - turn_of(p[2]), 20, 0.05), (p, q)
assert close(math.hypot(q[3][0] * 20, q[3][1] * 10), math.hypot(p[3][0] * 20, p[3][1] * 10), 1e-4), (p, q)
assert in_line(q) and q[:2] == p[:2], q
ops.ed.undo()
# S: one point, its handles longer (within their pieces).
assert ops.anim.timing_transform(mode="SCALE", factor=1.5) == {'FINISHED'}
assert close(pts()[2][3][0], p[3][0] * 1.5, 1e-5) and close(pts()[2][2][1], p[2][1] * 1.5, 1e-5), pts()[2]
ops.ed.undo()
# S on several: their places about their middle; X keeps it to time.
assert ops.anim.timing_select(**args(box, index=1)) == {'FINISHED'}
assert ops.anim.timing_select(**args(box, index=3, extend=1)) == {'FINISHED'}
assert ops.anim.timing_transform(mode="SCALE", factor=0.5, axis="X") == {'FINISHED'}
mid = (five[1][0] + five[3][0]) / 2
s1, s3 = pts()[1], pts()[3]
assert close(s1[0], mid + (five[1][0] - mid) * 0.5) and close(s3[0], mid + (five[3][0] - mid) * 0.5), (s1, s3)
assert close(s1[1], five[1][1]) and close(s3[1], five[3][1]), "X: the way along as it was"
ops.ed.undo()
# A handle picked alone: G moves it, the other turning to stay in line (aligned).
assert ops.anim.timing_select(**args(box, index=2, part="OUT")) == {'FINISHED'}
assert ops.anim.timing_transform(mode="MOVE", x=0, y=0.08) == {'FINISHED'}
h = pts()[2]
assert h[:2] == five[2][:2] and close(h[3][1], five[2][3][1] + 0.08) and in_line(h), h
ops.ed.undo()
# Handle types: Broken lets them go apart; Aligned puts them back in line, each as long; Flat lays them level.
assert ops.anim.timing_select(**args(box, index=2)) == {'FINISHED'}
assert ops.anim.timing_handles_type(type="BROKEN") == {'FINISHED'}
assert pts()[2][4] is False
assert ops.anim.timing_select(**args(box, index=2, part="OUT")) == {'FINISHED'}
assert ops.anim.timing_transform(mode="MOVE", x=0, y=0.08) == {'FINISHED'}
broken = pts()[2]
assert broken[2] == five[2][2] and not in_line(broken), ("broken: the other stays", broken)
assert ops.anim.timing_handles_type(type="ALIGNED", aspect=10) == {'FINISHED'}
fixed = pts()[2]
assert fixed[4] is True and in_line(fixed), ("Align puts them back in line", fixed)
assert close(math.hypot(fixed[2][0] * 20, fixed[2][1] * 10), math.hypot(broken[2][0] * 20, broken[2][1] * 10), 1e-4)
assert close(math.hypot(fixed[3][0] * 20, fixed[3][1] * 10), math.hypot(broken[3][0] * 20, broken[3][1] * 10), 1e-4)
# along the way between theirs, as shown
ways = sorted([turn_of(broken[3]), turn_of((-broken[2][0], -broken[2][1]))])
assert ways[0] + 1 < turn_of(fixed[3]) < ways[1] - 1, (ways, turn_of(fixed[3]))
assert close(turn_of(fixed[3]), (ways[0] + ways[1]) / 2, 0.5), (ways, turn_of(fixed[3]))
assert ops.anim.timing_handles_type(type="FLAT") == {'FINISHED'}
assert pts()[2][2][1] == 0.0 and pts()[2][3][1] == 0.0 and pts()[2][3][0] > 0 > pts()[2][2][0], pts()[2]
ops.ed.undo()
ops.ed.undo()
ops.ed.undo()
ops.ed.undo()
assert pts() == five, pts()
# A box picks the points and handle tips inside it; with SUB it takes them out; NONE picks none.
assert ops.anim.timing_select(**args(box, box="%g,%g,%g,%g" % (0.2, -1, 0.55, 2))) == {'FINISHED'}
got = wm.timing_picks()["picks"]
assert (1, "POINT") in got and (2, "POINT") in got and (3, "POINT") not in got, got
assert ops.anim.timing_select(**args(box, box="%g,%g,%g,%g" % (0.45, -1, 0.55, 2), mode="SUB")) == {'FINISHED'}
got = wm.timing_picks()["picks"]
assert (1, "POINT") in got and (2, "POINT") not in got, got
assert ops.anim.timing_select(**args(box, action="NONE")) == {'FINISHED'}
assert wm.timing_picks()["picks"] == []
assert ops.anim.timing_transform(mode="MOVE", x=1) == {'CANCELLED'}, "nothing picked"
# X takes out every point picked (never an end).
assert ops.anim.timing_select(**args(box, action="ALL")) == {'FINISHED'}
assert ops.anim.timing_point_delete() == {'FINISHED'}
assert len(pts()) == 2 and wm.timing_picks()["picks"] == []

# --- Decimate (the Edit mode's right click menu), its Ratio adjustable after ------------------------------------------
start()
box = line_layer({10: 0.0, 30: 1.0})
box.keyframe_set("position", 30, speeds=noisy)
many = key(box, 30)["timing"]
inside = len(many) - 2
assert ops.anim.timing_decimate(**args(box, ratio=0.4)) == {'FINISHED'}
half = key(box, 30)["timing"]
assert len(half) - 2 == round(inside * 0.4), (len(half), inside)
assert half[0][:2] == (0.0, 0.0) and half[-1][:2] == (1.0, 1.0), "the ends stay"
assert close(wm.path_brush_settings()["timing_decimate_ratio"], 0.4), "the Ratio is kept for next time"
# A Ratio of 1 keeps them all (and still finishes, so raising it in the panel puts points back).
box.keyframe_set("position", 30, speeds=noisy)
assert key(box, 30)["timing"] == many
assert ops.anim.timing_decimate(**args(box, ratio=1)) == {'FINISHED'}
assert key(box, 30)["timing"] == many, "a Ratio of 1 takes nothing out"
# The points that go first are those the curve changes least without: a point on a straight line goes before a bend's.
line = [(0.0, 0.0, (0, 0), (0.1, 0.1)), (0.3, 0.3, (-0.1, -0.1), (0.1, 0.1)), (0.6, 0.6, (-0.1, -0.1), (0.05, 0.2)),
        (0.8, 0.95, (-0.05, -0.05), (0.05, 0.0)), (1.0, 1.0, (-0.05, 0.0), (0, 0))]
box.keyframe_set("position", 30, timing=line)
stored = key(box, 30)["timing"]
assert ops.anim.timing_decimate(**args(box, ratio=0.67)) == {'FINISHED'}
left = key(box, 30)["timing"]
assert close([p[0] for p in left], [0.0, 0.6, 0.8, 1.0]), ("the straight one went", left)
assert close(left[2][2], (-0.05, -0.05)) and close(left[2][3], (0.05, 0.0)), "pieces left alone keep their handles"
# Exactly: the two pieces from 0.6 on were neighbours already, so their handles are the stored ones, not fitted again.
assert left[1][3] == stored[2][3] and left[2][2] == stored[3][2] and left[2][3] == stored[3][3] and         left[3][2] == stored[4][2], (
    "pieces left alone keep their handles exactly", left)
# Points that only split a lopsided curve go to a Ratio of 0, and the curve comes back as it was: the handles across
# are fitted to it (a third of the way each would miss by far more).
lopsided = [(0.0, 0.0, (0, 0), (0.7, 0.0)), (1.0, 1.0, (-0.05, 0.0), (0, 0))]
box.keyframe_set("position", 30, timing=lopsided)
first = played(box, 10, 30)
for x in ("0.3", "0.55", "0.8"):
    assert ops.anim.timing_point_add(**args(box, x=x)) == {'FINISHED'}
assert len(key(box, 30)["timing"]) == 5
assert ops.anim.timing_decimate(**args(box, ratio=0)) == {'FINISHED'}
assert len(key(box, 30)["timing"]) == 2
back = played(box, 10, 30)
print("Decimated back: off by %.4f at most" % max(abs(a - b) for a, b in zip(first, back)))
assert close(back, first, 5e-3), ("the lopsided curve came back", back, first)
# A curve with nothing between its ends: nothing to do.
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", **args(box)) == {'FINISHED'}
assert ops.anim.timing_decimate(**args(box, ratio=0.5)) == {'CANCELLED'}

# The graph's settings.
assert ops.anim.timing_graph_set(mode="SCULPT", brush="SMOOTH", radius="4", strength="0.3", lock_x="1") == \
    {'FINISHED'}
s = wm.path_brush_settings()
assert (s["graph_mode"], s["timing_brush"], s["lock_x"]) == ("SCULPT", "SMOOTH", True), s
assert close(s["timing_brushes"]["SMOOTH"], (4.0, 0.3)) and close(s["timing_brushes"]["GRAB"], (2.0, 1.0)), s
assert ops.anim.timing_graph_set(mode="EDIT", brush="GRAB", lock_x="0") == {'FINISHED'}

# F sizes the brush from the size it has: the mouse starts on the circle's edge, so it keeps its size until it moves.
r = wm.timing_resize(4.0, 12.0, [(0, 0), (12, 0), (-12, 0)])
assert close(r, [4.0, 5.0, 3.0]), ("F starts at the brush's size and grows or shrinks from there", r)

# The graph is never taller than it is wide: a tall, narrow editor leaves room under its controls, not a long box.
assert close(wm.path_timing_graph_height(900.0, 380.0, 52.0), 380.0), "a tall editor: a square graph"
assert close(wm.path_timing_graph_height(200.0, 600.0, 52.0), 200.0), "a wide editor: all the room it has"
assert close(wm.path_timing_graph_height(10.0, 600.0, 52.0), 52.0), "never shorter than the least"

# --- Python: the curve set as it is --------------------------------------------------------------------------------------
box.keyframe_set("position", 30, timing=[(0.0, 0.0, (0, 0), (0.2, 0.0)), (1.0, 1.0, (-0.2, 0.0), (0, 0))])
assert len(key(box, 30)["timing"]) == 2
assert close(played(box, 20, 20)[0], 0.5), "an S curve: half way at half time"
try:
    box.keyframe_set("position", 30, timing=[(0.0, 0.0, (0, 0), (0, 0)), (0.5, 1.0, (0, 0), (0, 0))])
    assert False, "a curve not ending on the key is refused"
except ValueError:
    pass

# --- Files: the curve saved; an older file's speeds open as a curve -----------------------------------------------------
folder = tempfile.mkdtemp(prefix="oanim_test_")
box.keyframe_set("position", 30, speeds=speeds)
kept = played(box, 10, 30)
saved = os.path.join(folder, "timing.oanim")
assert ops.wm.save_mainfile(filepath=saved) == {'FINISHED'}
with zipfile.ZipFile(saved) as archive:
    stored = json.loads(archive.read("document.json"))
text = json.dumps(stored)
assert '"timing"' in text and '"speeds"' not in text, "the curve is stored, not a speed a frame"
assert ops.wm.open_mainfile(filepath=saved) == {'FINISHED'}
box = next(l for l in layers if l.name == box.name)
assert close(played(box, 10, 30), kept, 1e-5), "opened as saved"


def make_old(node):
    """Format 6: a Custom key's speeds, one a frame."""
    if isinstance(node, dict):
        if "timing" in node and isinstance(node["timing"], list) and node.get("interp") == "CUSTOM":
            del node["timing"]
            node["speeds"] = speeds
        for v in node.values():
            make_old(v)
    elif isinstance(node, list):
        for v in node:
            make_old(v)


make_old(stored)
stored["version"] = 6
old = os.path.join(folder, "old.oanim")
with zipfile.ZipFile(saved) as src, zipfile.ZipFile(old, "w") as dst:
    for item in src.infolist():
        data = json.dumps(stored).encode() if item.filename == "document.json" else src.read(item.filename)
        dst.writestr(item, data)
assert ops.wm.open_mainfile(filepath=old) == {'FINISHED'}
box = next(l for l in layers if l.name == box.name)
assert 2 < len(key(box, 30)["timing"]) < 10, key(box, 30)
assert close(played(box, 10, 30), progress_of(speeds), 5e-4), "the old speeds' timing kept"

print("Timing curve: ok")
