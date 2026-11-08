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

# The graph's G R S, V and Sculpt at their edges (the second timing sweep).
EVEN5 = [(0.0, 0.0, (0, 0), (0.08, 0.08)), (0.25, 0.25, (-0.08, -0.08), (0.08, 0.08)),
         (0.5, 0.5, (-0.08, -0.08), (0.08, 0.08)), (0.75, 0.75, (-0.08, -0.08), (0.08, 0.08)),
         (1.0, 1.0, (-0.08, -0.08), (0, 0))]

# S on two picked points stops them short of the one not picked, which stays where it was.
start()
box = line_layer({10: 0.0, 30: 1.0})
box.keyframe_set("position", 30, timing=EVEN5)
assert ops.anim.timing_select(index="1", **args(box, frame=11)) == {'FINISHED'}
assert ops.anim.timing_select(index="2", extend="1", **args(box, frame=11)) == {'FINISHED'}
assert ops.anim.timing_transform(mode="SCALE", factor="5") == {'FINISHED'}
t = key(box, 30)["timing"]
assert abs(t[3][0] - 0.75) < 1e-6, ("S moved the point not picked", [p[0] for p in t])
assert t[1][0] < t[2][0] < t[3][0], ("S put the picks out of order", [p[0] for p in t])

# Picks made on a stretch whose end key then moves are let go: G and X edit nothing (the stretch the frame is in now
# was never picked).
for edit in ("G", "X"):
    start()
    box = line_layer({10: 0.0, 20: 1.0, 30: 2.0})
    box.keyframe_set("position", 20, timing=EVEN5)
    box.keyframe_set("position", 30, timing=EVEN5)
    assert ops.anim.timing_select(index="2", **args(box, frame=15)) == {'FINISHED'}
    ops.anim.keys_deselect_all()
    assert ops.anim.keys_select(frame_start="20", layers=str(box.id)) == {'FINISHED'}
    assert ops.anim.keys_offset(frames="-5") == {'FINISHED'}
    before = key(box, 30)["timing"]
    if edit == "G":
        r = ops.anim.timing_transform(mode="MOVE", x="2", y="0.1")
    else:
        r = ops.anim.timing_point_delete()
    assert r == {'CANCELLED'}, (edit, "on stale picks", r)
    assert key(box, 30)["timing"] == before, (edit, "on stale picks edited the 15-30 stretch")
    assert not wm.timing_picks()["picks"], (edit, "kept the stale picks", wm.timing_picks())

# V > Aligned (and Flat) pressed again changes nothing: what the curve shows is what's stored.
for kind in ("ALIGNED", "FLAT"):
    start()
    box = line_layer({10: 0.0, 30: 1.0})
    box.keyframe_set("position", 30, timing=[(0.0, 0.0, (0, 0), (0.2, 0.0)),
                                           (0.5, 0.5, (-0.3, -0.1), (0.4, 0.6), False),
                                           (1.0, 1.0, (-0.2, 0.0), (0, 0))])
    assert ops.anim.timing_select(index="1", **args(box, frame=11)) == {'FINISHED'}
    assert ops.anim.timing_handles_type(type=kind) == {'FINISHED'}
    once, played = key(box, 30)["timing"], xs(box, 10, 30)
    assert ops.anim.timing_handles_type(type=kind) == {'FINISHED'}
    twice = key(box, 30)["timing"]
    assert worst(xs(box, 10, 30), played) < 1e-5, (kind, "twice moved the dots")
    for a, b in zip(once, twice):
        assert worst(a[2] + a[3], b[2] + b[3]) < 1e-6, (kind, "twice changed the handles", once, twice)

# G with both handles of an aligned point picked moves both tips, whichever was picked first.
got = []
for order in (("IN", "OUT"), ("OUT", "IN")):
    start()
    box = line_layer({10: 0.0, 30: 1.0})
    box.keyframe_set("position", 30, timing=[(0.0, 0.0, (0, 0), (0.1, 0.0)),
                                           (0.5, 0.5, (-0.1, -0.1), (0.1, 0.1), True),
                                           (1.0, 1.0, (-0.1, 0.0), (0, 0))])
    assert ops.anim.timing_select(part=order[0], index="1", **args(box, frame=11)) == {'FINISHED'}
    assert ops.anim.timing_select(part=order[1], index="1", extend="1", **args(box, frame=11)) == {'FINISHED'}
    assert ops.anim.timing_transform(mode="MOVE", x="0", y="0.1") == {'FINISHED'}
    p = key(box, 30)["timing"][1]
    got.append(p[2] + p[3])
assert worst(got[0], got[1]) < 1e-5, ("G of both handles depends on pick order", got)
ix, iy, ox, oy = got[0]
assert iy > -0.09 and oy > 0.105, ("G of both handles didn't lift both tips", got[0])
assert abs(ix * oy - iy * ox) < 1e-4, ("the aligned point's handles left line", got[0])

# Grab in time alone without Dyntopo keeps the dots going forward (the brush keeps the samples in order).
start()
box = line_layer({10: 0.0, 30: 1.0})
a = args(box, frame=20)
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN_OUT", **a) == {'FINISHED'}
assert ops.anim.timing_point_add(x="0.5", **a) == {'FINISHED'}
assert ops.anim.timing_sculpt(brush="GRAB", path="20,0.5;29,0.5", radius="3", strength="1", aspect="20",
                              lock_x="0", dyntopo="0", **a) == {'FINISHED'}
u = xs(box, 10, 30)
steps = [u[i + 1] - u[i] for i in range(len(u) - 1)]
assert min(steps) > -1e-4, ("a level Grab without Dyntopo ran the dots backwards", [round(v, 3) for v in u])

# A stroke that moves nothing (a Grab click, no strength) is no edit: the key stays Smooth, no undo step.
start()
box = line_layer({10: 0.0, 30: 1.0}, interp="SMOOTH")
step = wm.undo_history()[1]
for stroke in (dict(path="20,0.5", strength="1"), dict(path="20,0.5;25,0.8", strength="0")):
    for dyn in ("0", "1"):
        r = ops.anim.timing_sculpt(brush="GRAB", radius="3", aspect="20", dyntopo=dyn, **stroke,
                                   **args(box, frame=20))
        assert r == {'CANCELLED'}, ("a stroke that moved nothing", stroke, dyn, r)
assert key(box, 30)["interp"] == "SMOOTH", ("a stroke that moved nothing made the key", key(box, 30)["interp"])
assert wm.undo_history()[1] == step, "a stroke that moved nothing pushed an undo step"

# The frame shown takes its new place at once: after a path key's interpolation changes, and on a channel Time
# Together retimed.
start()
box = line_layer({10: 0.0, 40: 1.0, 70: 1.4})
for frame, at, interp in ((50, 70, "EASE_IN"), (25, 40, "EASE_IN"), (25, 40, "STEP"), (24, 40, "BEZIER")):
    doc.frame_current = frame
    assert ops.anim.motion_path_key_interp(frame=str(at), interp=interp, **args(box)) == {'FINISHED'}
    shown = box.location_x
    doc.frame_current = frame + 1
    doc.frame_current = frame
    assert abs(shown - box.location_x) < 1e-4, ("key", at, interp, "frame", frame, "kept its old place", shown,
                                                 box.location_x)
start()
a = line_layer({10: 0.0, 30: 1.0})
b = line_layer({10: 0.0, 30: 2.0})
doc.time_together = True
doc.frame_current = 20
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", **args(a, frame=20)) == {'FINISHED'}
assert key(b, 30)["interp"] == "CUSTOM", "B took A's timing"
shown = b.location_x
doc.frame_current = 21
doc.frame_current = 20
assert abs(shown - b.location_x) < 1e-4, ("B, retimed by Time Together, kept its old place", shown, b.location_x)

# A sculpt refitting one side of an aligned point whose handles stand straight up keeps them in line (the fit's slope
# can't be straight up; the side it fits turns opposite the side it kept).
for dyn in ("0", "1"):
    start()
    box = line_layer({10: 0.0, 30: 1.0})
    box.keyframe_set("position", 30, timing=[(0.0, 0.0, (0, 0), (0.1, 0.1)),
                                           (0.4, 0.4, (0.0, -0.06), (0.0, 0.06), True),
                                           (0.7, 0.7, (-0.08, -0.08), (0.08, 0.08), True),
                                           (1.0, 1.0, (-0.1, -0.1), (0, 0))])
    assert ops.anim.timing_sculpt(brush="GRAB", path="21.5,0.55;21.5,0.65", radius="1.2", strength="1", aspect="20",
                                  lock_x="1", dyntopo=dyn, **args(box, frame=20)) == {'FINISHED'}
    t = key(box, 30)["timing"]
    upright = [q for q in t if abs(q[0] - 0.4) < 1e-6]
    assert upright, ("the point beside the stroke stays", dyn, [q[:2] for q in t])
    q = upright[0]
    (ix, iy), (ox, oy) = q[2], q[3]
    assert q[4], ("still aligned", dyn, q)
    assert abs(ix * oy - iy * ox) <= 1e-6 * max(1e-9, math.hypot(ix, iy) * math.hypot(ox, oy)) * 1e3 and         ix * ox + iy * oy < 0, ("an aligned point's handles left line after a sculpt beside it", dyn, q)

print("timing robust: all passed")
