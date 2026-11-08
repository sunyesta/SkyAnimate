"""Timing edits at their edges: odd numbers given to the graph's operators, edits that should change nothing, frames
held still, stretches that turn, and keys other edits add (Key Whole Drawing's, Time Together's). Each part is a bug
the timing sweep found.

Run with:  OpenAnimate --background --python tests/test_timing_robust.py
"""

import faulthandler
import math

import oanim

ops, wm = oanim.ops, oanim.wm
doc = oanim.data.document
L = oanim.data.layers
TOL = 2e-4  # the timing curve's fit tolerance, as a share of the way


def start():
    global doc
    assert ops.wm.read_homefile() == {'FINISHED'}
    doc = oanim.data.document
    doc.motion_paths = True
    doc.auto_key = False
    doc.frame_end = 400
    assert ops.wm.tool_motion_path() == {'FINISHED'}
    assert ops.anim.motion_path_brush_set(brush="EDIT", proportional="0") == {'FINISHED'}


def pick(layer):
    for other in L:
        other.selected = other.id == layer.id
    L.active = layer


def line_layer(keys, interp="LINEAR", setup_x=-1.0):
    """A new layer, its origin keyed along the x axis, {frame: x}."""
    ops.layer.new()
    layer = L.active
    layer.add_curve([(0.0, 0.0), (0.1, 0.0)])
    layer.key_whole_drawing = False
    doc.frame_current = 0
    layer.location_x, layer.location_y = setup_x, 0.0
    pick(layer)
    for f, x in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y="0", layer=str(layer.id)) == {'FINISHED'}
        layer.keyframe_set("position", f, interp=interp)
    return layer


def path_layer(keys):
    """A new layer, its origin keyed at {frame: (x, y)}, the default interpolation."""
    ops.layer.new()
    layer = L.active
    layer.add_curve([(0.0, 0.0), (0.1, 0.0)])
    layer.key_whole_drawing = False
    doc.frame_current = 0
    layer.location_x, layer.location_y = -1.0, -1.0
    pick(layer)
    for f, (x, y) in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(layer.id)) == {'FINISHED'}
    return layer


def args(layer, **kw):
    a = dict(layer=str(layer.id))
    a.update({k: str(v) for k, v in kw.items()})
    return a


def key(layer, frame, channel="position"):
    return next(k for k in layer.keyframes(channel) if k["frame"] == frame)


def xs(layer, first, last):
    out = []
    for f in range(first, last + 1):
        doc.frame_current = f
        out.append(layer.location_x)
    return out


def places(layer, first, last):
    out = []
    for f in range(first, last + 1):
        doc.frame_current = f
        out.append((layer.location_x, layer.location_y))
    return out


def chart(start_frame):
    for t in wm.motion_path_timing():
        for s in t["stretches"]:
            if s["start"] == start_frame:
                return s
    raise AssertionError("no stretch from %d" % start_frame)


def worst(a, b):
    return max(abs(x - y) for x, y in zip(a, b))


# --- The graph's operators given odd numbers ---------------------------------------------------------------------

# Smooth over a stroke whose length (with the aspect) isn't finite, or is huge, returns at once (it stepped toward the
# end forever). A hang fails the test: the watchdog runs outside Python's lock, so it fires while the op runs.
start()
box = line_layer({10: 0.0, 20: 1.0})
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN_OUT", frame="15", **args(box)) == {'FINISHED'}
for aspect, path in (("1e30", "15,0.5;16,0.6"), ("10", "15,0.5;3e7,0.5")):
    faulthandler.dump_traceback_later(20, exit=True)
    for dyntopo in ("0", "1"):
        ops.anim.timing_sculpt(brush="SMOOTH", frame="15", path=path, radius="3", strength="1", aspect=aspect,
                               dyntopo=dyntopo, **args(box))
    faulthandler.cancel_dump_traceback_later()
    assert all(math.isfinite(p[1]) for p in key(box, 20).get("timing", [])), (aspect, path)

# A huge radius smooths as a big one does: the curve stays (an overflowing reach wiped it to even).
start()
box = line_layer({10: 0.0, 20: 1.0})
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN_OUT", frame="15", **args(box)) == {'FINISHED'}
for radius in ("1e9", "1e10", "1e30"):
    r = ops.anim.timing_sculpt(brush="SMOOTH", frame="15", path="12,0.2;18,0.8", radius=radius, strength="1",
                               aspect="10", dyntopo="1", **args(box))
    k = key(box, 20)
    assert r == {'CANCELLED'} or (k["interp"] == "CUSTOM" and len(k.get("timing", [])) >= 3), (radius, r, k)
    if r == {'FINISHED'}:
        assert ops.ed.undo() == {'FINISHED'}

# A point added with a y= that isn't a number goes on the curve, as with none (it went to 0, the stretch's start).
start()
box = line_layer({10: 0.0, 20: 1.0})
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN_OUT", frame="15", **args(box)) == {'FINISHED'}
assert ops.anim.timing_point_add(frame="15", x="0.6", **args(box)) == {'FINISHED'}
on_curve = next(p for p in key(box, 20)["timing"] if abs(p[0] - 0.6) < 1e-4)[1]
assert 0.6 < on_curve < 0.95, on_curve
assert ops.ed.undo() == {'FINISHED'}
for y in ("nan", "inf", "", "abc"):
    if ops.anim.timing_point_add(frame="15", x="0.6", y=y, **args(box)) == {'FINISHED'}:
        now = next(p for p in key(box, 20)["timing"] if abs(p[0] - 0.6) < 1e-4)[1]
        assert abs(now - on_curve) < 1e-4, ("y=%r" % y, now, on_curve)
        assert ops.ed.undo() == {'FINISHED'}

# --- Curves made by hand stay as made --------------------------------------------------------------------------------

# Reshaping the path (a shape brush's stroke on its key) keeps a Custom stretch's own curve: it goes by share of the
# way, whatever the way is (it was fitted again through each frame's speed, its placed point gone).
start()
box = line_layer({10: 0.0, 20: 1.0})
assert ops.anim.timing_point_add(frame="15", x="0.3", y="0.6", **args(box)) == {'FINISHED'}
hand = key(box, 20)["timing"]
assert len(hand) == 3, hand
assert ops.anim.motion_path_brush_set(brush="GRAB") == {'FINISHED'}
doc.frame_current = 20
assert ops.anim.motion_path_stroke(brush="GRAB", points="1,0;1,0.3", radius="0.3", strength="1",
                                   **args(box)) == {'FINISHED'}
assert abs(key(box, 20)["value"][1] - 0.3) < 1e-4, key(box, 20)["value"]
assert key(box, 20)["timing"] == hand, ("the hand-made curve was fitted again", hand, key(box, 20)["timing"])
assert ops.anim.motion_path_brush_set(brush="EDIT") == {'FINISHED'}

# A Custom position plays its curve: each frame as far along its path as the curve says (a share of the Bezier's
# parameter within each piece of its length table landed up to 0.001 of the way off).
for setup_x, n in ((-10.0, 20), (-1.0, 300)):
    start()
    box = line_layer({10: 0.0, 10 + n: 1.0}, interp="SMOOTH", setup_x=setup_x)
    assert ops.anim.timing_point_add(frame="11", x="0.5", **args(box)) == {'FINISHED'}
    assert key(box, 10 + n)["interp"] == "CUSTOM"
    want = chart(10)["progress"]
    off = worst(xs(box, 10, 10 + n), want)
    assert off <= TOL, ("plays off its curve", setup_x, n, off)

# --- Edits that change nothing change nothing ------------------------------------------------------------------------

# Alt S by 1, or a dot slid to where it is, again and again: the dots stay (each fitted the curve again, a hair off,
# adding up).
for how in ("scale", "slide"):
    start()
    box = line_layer({10: 0.0, 30: 1.0})
    box.keyframe_set("position", 30, speeds=[1.0 + 0.6 * math.sin(i / 2.0) for i in range(20)])
    base = xs(box, 10, 30)
    curve = key(box, 30)["timing"]
    for _ in range(20):
        if how == "scale":
            assert ops.anim.motion_path_scale_spacing(frame="15", factor="1", **args(box)) == {'FINISHED'}
        else:
            assert ops.anim.motion_path_slide(frame="15", progress=repr(base[5]), **args(box)) == {'FINISHED'}
    assert worst(base, xs(box, 10, 30)) <= TOL, (how, "dots drifted", worst(base, xs(box, 10, 30)))
    assert key(box, 30)["timing"] == curve, (how, "the curve was fitted again")

# --- Frames held still -----------------------------------------------------------------------------------------------

# A held stretch inside a Custom one: the chart shows it held (each frame crept on at least 0.02 of the even speed),
# and sliding a dot elsewhere leaves it held.
start()
box = line_layer({10: 0.0, 30: 1.0})
box.keyframe_set("position", 30, speeds=[1.0] * 8 + [0.0] * 4 + [1.0] * 8)
p = xs(box, 10, 30)
assert worst(p, chart(10)["progress"]) <= TOL, ("chart vs play", p[8:13], chart(10)["progress"][8:13])
assert ops.anim.motion_path_slide(frame="14", progress=repr(p[4] + 0.01), **args(box)) == {'FINISHED'}
q = xs(box, 10, 30)
assert max(q[8:13]) - min(q[8:13]) <= TOL, ("sliding dot 14 set the held frames going", q[8:13])

# Elastic (it stands still at its turns) and Smooth on 2s: the chart says what plays, and a dot slid to where it is
# moves nothing.
for interp, interval in (("ELASTIC", 1), ("SMOOTH", 2)):
    start()
    box = line_layer({10: 0.0, 30: 1.0, 40: 1.5}, interp=interp)
    if interval > 1:
        box.keyframe_set("position", 10, interval=interval)
    p = xs(box, 10, 30)
    assert worst(p, chart(10)["played"]) <= TOL, (interp, "chart vs play", worst(p, chart(10)["played"]))
    before = xs(box, 10, 40)
    f = 10 + 2 * interval
    assert ops.anim.motion_path_slide(frame=str(f), progress=repr(p[f - 10]), **args(box)) == {'FINISHED'}
    assert worst(before, xs(box, 10, 40)) <= TOL, (interp, "a dot slid to where it is moved others")

# --- Stretches that turn ---------------------------------------------------------------------------------------------

# Measured along the path, not by straight steps between frames (which cut a turn's corner): a Linear stretch bent by
# a control point shows even, and sliding a dot to where it is moves no other.
start()
box = line_layer({10: 0.0, 30: 1.0, 40: 1.5})
assert ops.anim.motion_path_control_add(frame="20", **args(box)) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="30", index="0", x="0.5", y="1.0", **args(box)) == {'FINISHED'}
assert key(box, 30)["interp"] == "LINEAR"
assert worst(chart(10)["progress"], [k / 20 for k in range(21)]) <= TOL, chart(10)["progress"]
p = places(box, 10, 30)
assert ops.anim.motion_path_slide(frame="15", progress="0.25", **args(box)) == {'FINISHED'}
q = places(box, 10, 30)
moved = max(math.dist(p[i], q[i]) for i in range(21))
assert moved <= TOL * 2.2, ("control point stretch: others moved", moved)

# The same for a plain Smooth stretch that turns.
start()
box = path_layer({10: (0, 0), 20: (1, 1), 30: (2, 0), 40: (3, 1)})
assert key(box, 20)["interp"] == "SMOOTH"
p = places(box, 10, 30)
u = chart(10)["progress"][4]
assert ops.anim.motion_path_slide(frame="14", progress=repr(u), **args(box)) == {'FINISHED'}
q = places(box, 10, 30)
moved = max(math.dist(p[i], q[i]) for i in range(21))
assert moved <= TOL * 1.5, ("Smooth turn: others moved", moved)

# --- Keys other edits add --------------------------------------------------------------------------------------------

# Key Whole Drawing: a drawing's stretch made Custom, then one point moved with Auto Keying. The point never touched
# gets a filler key that keeps its stretch's way and timing (it took the even halves' timing, moving ~0.1).
start()
doc.auto_key = True
doc.auto_freeze_keys = False
doc.frame_end = 100
ops.layer.new()
line = L.active
assert line.key_whole_drawing
doc.frame_current = 0
line.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
doc.frame_current = 10
line.set_point(0, (0.0, 0.1))
doc.frame_current = 30
line.set_point(0, (1.0, 0.1))
line.set_point(1, (1.0, 1.0))
doc.frame_current = 0
pick(line)
assert ops.anim.motion_path_slide(frame="15", progress="0.6", drawing="1", layer=str(line.id)) == {'FINISHED'}
assert line.keyframes("points[1].pos")[-1]["interp"] == "CUSTOM"
before = {}
for f in range(10, 31):
    doc.frame_current = f
    before[f] = tuple(line.points[1][:2])
doc.frame_current = 20
x, y = line.points[0][:2]
line.set_point(0, (x, y + 0.2))
for f in range(10, 31):
    doc.frame_current = f
    q = tuple(line.points[1][:2])
    assert math.dist(q, before[f]) <= 3e-4, ("point 1, untouched, moved", f, q, before[f])
assert 20 in [k["frame"] for k in line.keyframes("points[1].pos")], "point 1 has its filler key"

# Time Together: A's Linear stretch made Custom, B (keyed on the same frames, its path turning) takes the timing and
# keeps its straight way (it ran along Smooth's curve, its handles left Auto).
start()
a = path_layer({10: (0, 0), 20: (1, 0), 30: (2, 0)})
for f in (10, 20, 30):
    a.keyframe_set("position", f, interp="LINEAR")
b = path_layer({10: (0, 0), 20: (1, 1), 30: (2, 0)})
for f in (10, 20, 30):
    b.keyframe_set("position", f, interp="LINEAR")
doc.time_together = True
for o in L:
    o.selected = o.id in (a.id, b.id)
L.active = a
assert ops.anim.motion_path_slide(frame="15", progress="0.45", **args(a)) == {'FINISHED'}
assert key(b, 20)["interp"] == "CUSTOM", "B took A's timing"
off = [abs(x - y) for x, y in places(b, 10, 20)]
assert max(off) < 1e-3, ("B's dots left its straight line", off)
assert abs(places(b, 15, 15)[0][0] - 0.45) < 2e-3, ("B timed as A", places(b, 15, 15))

print("timing robust: all passed")
