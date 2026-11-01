"""Curve Deform held to what must always be true, and what a stress test of it found:

- across lines, Parts of lines and of closed shapes, and the settings: nothing moves at rest, a line moved or turned
  whole carries what follows it exactly, and the render and the viewport agree;
- a line dragged into a kink or a loop never throws what follows it across in one step (from a real mouth line, whose
  handle pulled out made the lip jump about a sixth of the mouth's width), nor does an end dragged back along the line;
- a reference matching its original bends as the original does (a mask made of one has to), and one that is the
  original's own shape does nothing, and doesn't hang;
- a shape drawn from Python can be picked at once;
- Part goes through edits of its shape, copies and saving.

Run with:  OpenAnimate --background --python tests/test_curve_deform_robust.py
"""
import math
import os
import tempfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
serial = [0]


def fresh():
    ops.wm.read_homefile()
    doc.frame_current = 0
    doc.auto_key = True


def drawn(name, points, closed=False):
    serial[0] += 1
    ops.layer.new()
    layer = layers.active
    layer.name = "%s%d" % (name, serial[0])
    layer.add_curve(points, closed=closed, fill=closed)
    return layer


def deform(layer, target, kind="CURVE_DEFORM", part=None, **settings):
    layers.active = layer
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    index = len(layer.modifiers) - 1
    settings.setdefault("precision", 0.0)
    layer.modifier_set(index, layer=target.name, shape=target.shapes[0][0], **settings)
    if part is not None:
        layer.modifier_set(index, part=part)
    return index


def points(layer):
    return [tuple(p[:2]) for p in layer.points]


def shown(layer, render=False):
    return [tuple(p[:2]) for p in layer.evaluated(render=render)[0]]


def handles(layer):
    return [(c, k, h[1], h[2]) for c in range(layer.curve_count) for k, h in enumerate(layer.curve_handles(c))]


def key_shape(layer, fn, frame=10):
    doc.frame_current = 0
    at, hs = points(layer), handles(layer)
    doc.frame_current = frame
    for i, p in enumerate(at):
        layer.set_point(i, fn(p))
    for c, k, handle_in, handle_out in hs:
        layer.place_handle(c, k, "IN", fn(handle_in), keep_other=True)
        layer.place_handle(c, k, "OUT", fn(handle_out), keep_other=True)


def worst(a, b):
    return max((math.dist(p, q) for p, q in zip(a, b)), default=0.0)


def biggest_step(series):
    return max(max(math.dist(p, q) for p, q in zip(a, b)) for a, b in zip(series, series[1:]))


def jumps(run, frames):
    """Whether the sweep `run(t)` (what shows at t, 0 to 1) has a jump: a step that stays as big made far finer."""
    series = [run(k / frames) for k in range(frames + 1)]
    steps = [max(math.dist(p, q) for p, q in zip(a, b)) for a, b in zip(series, series[1:])]
    k = steps.index(max(steps))
    fine = [run((k + j / 20) / frames) for j in range(21)]
    return biggest_step(fine) > 0.5 * steps[k] and steps[k] > 4 * sorted(steps)[frames // 2], (k, steps[k], biggest_step(fine))


LOOP = [(1, 0), (0, -0.4), (-1, 0), (0, 0.4)]
TARGETS = (([(-1, 0), (-0.5, 0.2), (0, 0), (0.5, -0.2), (1, 0)], False, None),
           ([(-2, 0), (-1, 0), (0, 0.1), (1, 0), (2, 0)], False, (1, 3)),
           (LOOP, True, (0, 2)),
           (LOOP, True, (2, 0)),
           ([(0, 0.4), (1, 0), (0, -0.4), (-1, 0)], True, (3, 1)),
           (LOOP, True, (0, 1)),
           ([(1, 0), (0, -1e-4), (-1, 0), (0, 1e-4)], True, (0, 2)))
ART = [(x / 4, y / 5) for x in range(-5, 6) for y in (-3, -1, 0, 1, 2)]

# --- What must always be true --------------------------------------------------------------------------------------
for line_points, closed, part in TARGETS:
    for settings in ({}, {"stretch": False}, {"precision": 0.002}, {"resample_self": True}):
        tag = (line_points, part, settings)
        fresh()
        line = drawn("Line", line_points, closed)
        art = drawn("Art", ART)
        index = deform(art, line, part=part, **settings)
        assert art.modifiers[index]["kind"] == "Line", tag
        count = len(ART)
        assert worst(shown(art), ART) < 1e-6, tag
        key_shape(line, lambda p: p)
        assert worst(shown(art)[:count], ART) < 2e-5, ("a keyed line that hasn't moved", tag)
        key_shape(line, lambda p: (p[0] + 0.37, p[1] - 0.21))
        assert worst(shown(art)[:count], [(p[0] + 0.37, p[1] - 0.21) for p in ART]) < 2e-5, ("moved whole", tag)
        for angle in (0.7, math.pi):
            c, s = math.cos(angle), math.sin(angle)
            turn = lambda p: (p[0] * c - p[1] * s, p[0] * s + p[1] * c)
            key_shape(line, turn)
            assert worst(shown(art)[:count], [turn(p) for p in ART]) < 5e-4, ("turned whole", angle, tag)
        key_shape(line, lambda p: (p[0] * 1.3, p[1] + 0.5 * p[0] * p[0]))
        a, b = shown(art), shown(art, True)
        assert len(a) == len(b) and worst(a, b) < 1e-5, ("render differs", tag)
        assert all(math.isfinite(v) for p in a for v in p), tag

# --- A real mouth line: its handle pulled out into a kink ----------------------------------------------------------
fresh()
line = drawn("Mouth", [(0.0431, 0.4249), (0.0, 0.4161), (-0.0431, 0.4249)])
lip = [(0.0273, 0.4182), (0.0, 0.3975), (-0.0273, 0.4182), (0.0, 0.3872), (0.015, 0.39), (-0.015, 0.39)]
art = drawn("Lip", lip)
deform(art, line)
doc.frame_current = 6
for i, p in enumerate([(0.07, 0.434), (0.001, 0.396), (-0.076, 0.433)]):
    line.set_point(i, p)
HANDLES = [(0, "IN", (0.07, 0.434)), (0, "OUT", (0.074, 0.417)), (1, "IN", (0.036, 0.403)), (1, "OUT", (-0.038, 0.402)),
           (2, "IN", (-0.076, 0.413)), (2, "OUT", (-0.082, 0.43))]
for k, side, at in HANDLES:
    line.place_handle(0, k, side, at, keep_other=True)


def pull_handle(t):
    line.place_handle(0, 1, "OUT", (-0.038 + 0.12 * t, 0.402), keep_other=True)
    return shown(art)


# The moment it comes to a point the line has no way it runs there, so a small step is left (it was 0.023, on a mouth 0.15 wide).
jumped, detail = jumps(pull_handle, 240)
print("METRIC kink", detail)
assert detail[1] < 0.008, ("a kink threw the lip across", detail)
line.place_handle(0, 1, "OUT", (-0.038, 0.402), keep_other=True)


def corners_cross(t):
    line.set_point(0, (0.07 - 0.25 * t, 0.434))
    line.set_point(2, (-0.076 + 0.25 * t, 0.433))
    return shown(art)


jumped, detail = jumps(corners_cross, 240)
assert not jumped, ("corners passing each other", detail)

# An end dragged back along the line, handles left behind: it doubles back, and what lies far from it mustn't flip.
fresh()
at = [(-1, 0), (-0.5, 0.2), (0, 0), (0.5, -0.2), (1, 0)]
line = drawn("Line", at)
rest = [(x / 8, y / 8) for x in range(-10, 11, 2) for y in (-6, -3, -1, 0, 1, 3, 6)]
art = drawn("Art", rest)
deform(art, line)
doc.frame_current = 10


def fold(t):
    line.set_point(4, (1 - 3.0 * t, 0.3 * math.sin(3.1 * t)))
    return shown(art)[:len(rest)]


jumped, detail = jumps(fold, 120)
assert not jumped, ("doubled back", detail)

# --- References ----------------------------------------------------------------------------------------------------
for kind, closed in (("CURVE_DEFORM", False), ("CAGE_DEFORM", True), ("MESH_DEFORM", False)):
    fresh()
    shape = drawn("Shape", [(-1, -1), (1, -1), (1, 1), (-1, 1)], closed)
    art = drawn("Art", [(-0.5, 0.2), (0, 0.4), (0.5, 0.2)])
    deform(art, shape, kind)
    layers.active = art
    assert ops.layer.make_reference() == {'FINISHED'}
    copy = layers.active
    assert copy.reference["match_original"]
    doc.frame_current = 10
    shape.set_point(2, (1.5, 1.5))
    shape.set_point(3, (-1.5, 1.5))
    assert worst(shown(art), [(-0.5, 0.2), (0, 0.4), (0.5, 0.2)]) > 0.05, kind
    assert worst(shown(copy), shown(art)) < 1e-5, (kind, "a matching reference bends with its original", shown(copy), shown(art))
    assert worst(shown(copy, True), shown(art, True)) < 1e-5, kind
    # A point moved on the copy lands where it is put, bent as it is.
    layers.active = copy
    assert ops.layer.reference_set(edit_points="1") == {'FINISHED'}
    goal = (shown(copy)[1][0] + 0.05, shown(copy)[1][1] + 0.03)
    copy.place_point(1, goal)
    assert math.dist(shown(copy)[1], goal) < 0.01, (kind, shown(copy)[1], goal)

# A drawing bent by a reference of itself: a loop, no hang, no crash.
fresh()
art = drawn("Art", [(1, 0), (0, -0.4), (-1, 0), (0, 0.4)], True)
layers.active = art
assert ops.layer.make_reference() == {'FINISHED'}
copy = layers.active
index = deform(art, copy, part=(0, 2))
doc.frame_current = 10
art.set_point(0, (2, 0))
assert all(math.isfinite(v) for p in shown(art) + shown(copy) for v in p)

# --- A shape drawn from Python can be picked at once, on any frame -------------------------------------------------
fresh()
line = drawn("Line", LOOP, True)
art = drawn("Art", ART)
index = deform(art, line, part=(0, 2))
for frame in (0, 10):
    doc.frame_current = frame
    other = drawn("Other", [(0, 0), (1, 0)])
    art.modifier_set(index, layer=other.name, shape=other.shapes[0][0])
    assert art.modifiers[index]["kind"] == "Line" and art.modifiers[index]["part"] is None, "another layer takes the Part away"

# --- Part through edits, copies and saving ------------------------------------------------------------------------


def mouth():
    fresh()
    line = drawn("Mouth", [(1, 0), (0, -1e-4), (-1, 0), (0, 1e-4)], True)
    top = drawn("Top", [(-0.5, 0.2), (0, 0.4), (0.5, 0.2)])
    bottom = drawn("Bottom", [(-0.5, -0.2), (0, -0.4), (0.5, -0.2)])
    deform(top, line, part=(0, 2))
    deform(bottom, line, part=(2, 0))
    doc.frame_current = 10
    line.set_point(3, (0, 0.5))
    line.set_point(1, (0, -0.5))
    if shown(top)[1][1] < 0.6:
        top.modifier_set(len(top.modifiers) - 1, part=(2, 0))
        bottom.modifier_set(len(bottom.modifiers) - 1, part=(0, 2))
    return line, top, bottom


def opened(top, bottom, tag):
    doc.frame_current = 10
    a, b = shown(top)[1], shown(bottom)[1]
    assert abs(a[1] - 0.9) < 0.03 and abs(b[1] + 0.9) < 0.03, (tag, a, b)


line, top, bottom = mouth()
opened(top, bottom, "as made")

# A point added inside the stretch, or a middle one taken out: the Part goes on between its ends.
line, top, bottom = mouth()
doc.frame_current = 0
line.split_segment(0, 0, 0.5)
assert top.modifiers[-1]["kind"] == "Line"
opened(top, bottom, "a point added")

line, top, bottom = mouth()
doc.frame_current = 0
layers.active = line
ops.mesh.select_none()
line.select_points([3])
assert ops.mesh.dissolve_points() == {'FINISHED'}
assert top.modifiers[-1]["kind"] == "Line" and top.modifiers[-1]["part"] is not None

# Change Type and back keeps it.
line, top, bottom = mouth()
index, part = len(top.modifiers) - 1, top.modifiers[-1]["part"]
top.modifier_set(index, type="CAGE_DEFORM")
assert top.modifiers[index]["kind"] == "Cage"
top.modifier_set(index, type="CURVE_DEFORM")
assert top.modifiers[index]["part"] == part
opened(top, bottom, "Change Type and back")

# What a group copy follows is the copy.
line, top, bottom = mouth()
names = (line.name, top.name, bottom.name)
for layer in (line, top, bottom):
    layer.select = True
layers.active = line
assert ops.layer.group() == {'FINISHED'}
layers.active = next(l for l in layers.all() if l.name == names[0]).parent
assert ops.layer.duplicate() == {'FINISHED'}
children = {c.name: c for c in layers.active.children}
doc.frame_current = 10
first_top = next(l for l in layers.all() if l.name == names[1] and l.parent.name != layers.active.name)
before = shown(first_top)
assert worst(shown(children[names[1]]), before) < 1e-5
children[names[0]].set_point(3, (0, 0.8))
assert worst(shown(first_top), before) < 1e-6, "a copy's line moves the copy's lips, not the first's"
assert worst(shown(children[names[1]]), before) > 0.05, "the copy's lip follows the copy's line"

# Saved twice.
line, top, bottom = mouth()
doc.frame_current = 10
want = shown(top)
path = os.path.join(tempfile.mkdtemp(), "robust.oanim")
for _ in range(2):
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
top = next(l for l in layers.all() if l.name.startswith("Top"))
doc.frame_current = 10
assert worst(shown(top), want) < 1e-5

print("Curve Deform robust tests passed")
