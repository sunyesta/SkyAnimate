"""Curve Deform against the true geometry, where a deform that quietly does nothing can't pass:

- what follows a line sits where the line truly is: on a smooth bend, with or without Stretch;
- a line bent by a deform of its own (Cage, Curve, Mesh) is followed on every frame, whatever points that deform's
  Precision adds to it (a brow on a warp line on a cage froze on the frames those cuts came in, then jumped), and a cage
  or mesh shape bent that way works too;
- cuts coming and going as the shape moves never make what follows it jump;
- the edit wire and the render agree with the drawing;
- a line collapsed to a point holds what follows it there; a tiny line at rest is no shape.

Run with:  OpenAnimate --background --python tests/test_curve_deform_stress.py
"""
import math

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0
serial = [0]


def new_layer(name):
    serial[0] += 1
    ops.layer.new()
    layer = layers.active
    layer.name = "%s%d" % (name, serial[0])
    return layer


def drawn(name, points, closed=False):
    layer = new_layer(name)
    layer.add_curve(points, closed=closed, fill=closed)
    return layer


def deform(layer, target, kind="CURVE_DEFORM", **settings):
    layers.active = layer
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    settings.setdefault("precision", 0.0)
    layer.modifier_set(len(layer.modifiers) - 1, layer=target.name, shape=target.shapes[0][0], **settings)


def points(layer):
    return [tuple(p[:2]) for p in layer.points]


def shown(layer):
    return layer.evaluated()[0]


def outline(layer):
    return [tuple(p) for p in layer.evaluated_outline(0, 32)[0]]


def key_shape(layer, fn):
    """Key `layer`'s points and handles at frame 10 where fn puts them from rest."""
    doc.frame_current = 0
    at, handles = points(layer), [(i, o) for _, i, o in layer.curve_handles(0)]
    doc.frame_current = 10
    for i, p in enumerate(at):
        layer.set_point(i, fn(p))
    for k, (hi, ho) in enumerate(handles):
        layer.place_handle(0, k, "IN", fn(hi), keep_other=True)
        layer.place_handle(0, k, "OUT", fn(ho), keep_other=True)


def segment_distance(p, a, b):
    d = (b[0] - a[0], b[1] - a[1])
    dd = d[0] * d[0] + d[1] * d[1]
    t = max(0.0, min(1.0, ((p[0] - a[0]) * d[0] + (p[1] - a[1]) * d[1]) / dd)) if dd > 0.0 else 0.0
    return math.dist(p, (a[0] + d[0] * t, a[1] + d[1] * t))


def line_distance(p, v):
    return min(segment_distance(p, v[i], v[i + 1]) for i in range(len(v) - 1))


def place_on(v, s):
    """Where polyline `v` is `s` along it, and its left."""
    for i in range(len(v) - 1):
        length = math.dist(v[i], v[i + 1])
        if s <= length or i == len(v) - 2:
            t = s / length if length > 0.0 else 0.0
            way = ((v[i + 1][0] - v[i][0]) / length, (v[i + 1][1] - v[i][1]) / length)
            return (v[i][0] + (v[i + 1][0] - v[i][0]) * t, v[i][1] + (v[i + 1][1] - v[i][1]) * t), (-way[1], way[0])
        s -= length
    return v[-1], (0.0, 1.0)


# --- A smooth bend: on the line and to either side of it --------------------------------------------------------
xs = [-1 + 2 * i / 8 for i in range(9)]
radius = 4 / math.pi
for stretch in (True, False):
    for precision in (0.0, 0.002):
        doc.frame_current = 0
        line = drawn("Arc", [(x, 0) for x in xs])
        rest = [(x * 0.9, h) for x in xs for h in (0, 0.2, -0.2)]
        art = drawn("ArcArt", rest)
        deform(art, line, stretch=stretch, precision=precision)
        key_shape(line, lambda p: (radius * math.sin(p[0] / radius), radius * (1 - math.cos(p[0] / radius))))
        v = outline(line)
        total = sum(math.dist(a, b) for a, b in zip(v, v[1:]))
        for q, p in zip(shown(art), rest):
            on, left = place_on(v, (p[0] + 1) * total / 2 if stretch else p[0] + 1)
            want = (on[0] + left[0] * p[1], on[1] + left[1] * p[1])
            assert math.dist(q, want) < 0.01, (stretch, precision, p, q, want)

# Stretch spreads what lies along the line, past its ends too; without it, each keeps how far it is from the start.
doc.frame_current = 0
line = drawn("Grow", [(0, 0), (1, 0)])
rest = [(0.25, 0), (0.5, 0), (1.2, 0), (-0.2, 0)]
spread, kept = drawn("Spread", rest), drawn("Kept", rest)
deform(spread, line, stretch=True)
deform(kept, line, stretch=False)
key_shape(line, lambda p: (p[0] * 2, 0))
assert all(math.dist(q, (p[0] * 2, 0)) < 1e-3 for q, p in zip(shown(spread), rest))
assert all(math.dist(q, p) < 1e-3 for q, p in zip(shown(kept), rest))


# --- A line bent by a deform of its own ---------------------------------------------------------------------------
def box_around():
    return drawn("Box", [(-1.5, -1), (1.5, -1), (1.5, 1), (-1.5, 1)], closed=True)


def bend_box(box):
    at = points(box)
    box.set_point(0, (at[0][0] - 0.2, at[0][1] + 1.2))
    box.set_point(3, (at[3][0], at[3][1] + 0.8))


def bend_under():
    under = drawn("Under", [(-1.5, -0.5), (0, -0.5), (1.5, -0.5)])
    return under, lambda: under.set_point(1, (0, 0.5))


def bent_by(kind, precision, line):
    if kind == "cage":
        box = box_around()
        deform(line, box, "CAGE_DEFORM", precision=precision)
        return lambda: bend_box(box)
    if kind == "grid":
        grid = new_layer("Grid")
        assert grid.add_primitive("GRID", [(-1.5, -1), (1.5, 1)], columns=2, rows=2)
        deform(line, grid, "MESH_DEFORM", precision=precision)
        at = points(grid)
        return lambda: grid.set_point(4, (at[4][0], at[4][1] + 0.6))
    under, bend = bend_under()
    deform(line, under, "CURVE_DEFORM" if kind == "curve" else "MESH_DEFORM", precision=precision)
    return bend


cut = False
for kind, precision in (("cage", 0.002), ("cage", 0.0), ("cage", 0.0002), ("curve", 0.002), ("curve", 0.0),
                        ("grid", 0.002), ("wire", 0.002)):
    doc.frame_current = 0
    line = drawn("Warp", [(-1, 0), (1, 0)])
    bend = bent_by(kind, precision, line)
    rest = [(x / 4, 0.0) for x in range(-4, 5)] + [(x / 4, 0.2) for x in range(-4, 5)]
    art = drawn("Brow", rest)
    deform(art, line)
    doc.frame_current = 10
    bend()
    cut = cut or len(shown(line)) > 2
    pts, v = shown(art), outline(line)
    assert max(math.dist(a, b) for a, b in zip(pts, rest)) > 0.1, (kind, precision, "does nothing")
    assert max(line_distance(q, v) for q in pts[:9]) < 0.005, (kind, precision, "left the line")
    assert max(abs(line_distance(q, v) - 0.2) for q in pts[9:]) < 0.005, (kind, precision, "lost its offset")
    render = art.evaluated(render=True)[0]
    assert max(math.dist(a, b) for a, b in zip(pts, render)) < 1e-5, (kind, precision, "render differs")
assert cut, "Precision added points to one of the lines"

# The edit wire follows the same line as the drawing, though it is drawn with no cuts of its own.
doc.frame_current = 0
under, bend = bend_under()
line = drawn("WireLine", [(-1, 0), (1, 0)])
deform(line, under, precision=0.002)
art = drawn("WireArt", [(-0.5, 0.1), (0.5, 0.1)])
deform(art, line)
doc.frame_current = 10
bend()
assert len(shown(line)) > 2
start = art.evaluated_handles(0, shown=True)[0][1]
assert math.dist(start, shown(art)[0]) < 1e-5, (start, shown(art)[0])

# A cage's or mesh's shape bent the same way: its cuts left out, the deform still on.
for kind, make in (("CAGE_DEFORM", lambda: drawn("Cage", [(-1, -1), (1, -1), (1, 1), (-1, 1), (-1.2, 0)], closed=True)),
                   ("MESH_DEFORM", lambda: drawn("Cell", [(-1, -1), (1, -1), (1, 1), (-1, 1)], closed=True))):
    doc.frame_current = 0
    shape = make()
    under, bend = bend_under()
    deform(shape, under, precision=0.002)
    rest = [(x / 4, y / 4) for x in range(-3, 4, 2) for y in range(-3, 4, 2)]
    art = drawn("Inside", rest)
    deform(art, shape, kind)
    doc.frame_current = 10
    bend()
    assert shape.point_count < len(shown(shape)), "Precision added points to the shape"
    assert max(math.dist(a, b) for a, b in zip(shown(art), rest)) > 0.1, (kind, "does nothing")

# --- Cuts coming and going never jump: halving the step halves what a jump can't -----------------------------------
doc.frame_current = 0
under = drawn("Under", [(-1.5, -0.5), (0, -0.5), (1.5, -0.5)])
line = drawn("Warp", [(-1, 0), (1, 0)])
deform(line, under, precision=0.002)
rest = [(x / 5, y / 10) for x in range(-5, 6) for y in range(-3, 4)]
art = drawn("Follows", rest)
deform(art, line, precision=0.002)
doc.frame_current = 10
series, counts = [], set()
step = 0.002
for k in range(240):
    under.set_point(1, (0, -0.5 + step * k))
    series.append(shown(art)[:len(rest)])
    counts.add(len(shown(line)))
assert len(counts) > 2, counts
for i in range(1, len(series) - 1):
    worst = max(math.dist((a[0] - 2 * b[0] + c[0], a[1] - 2 * b[1] + c[1]), (0, 0))
                for a, b, c in zip(series[i - 1], series[i], series[i + 1]))
    assert worst < 0.7 * step, (i, worst / step)

# --- Collapsed and tiny lines ---------------------------------------------------------------------------------------
for stretch in (True, False):
    doc.frame_current = 0
    line = drawn("Shrink", [(-1, 0), (0, 0), (1, 0)])
    rest = [(x / 4, y / 4) for x in range(-4, 5, 2) for y in range(-3, 4, 2)]
    art = drawn("Held", rest)
    deform(art, line, stretch=stretch)
    last = None
    for scale in (1e-3, 1e-4, 1e-5, 1e-6, 1e-7, 0.0):
        key_shape(line, lambda p: (p[0] * scale, p[1] * scale))
        pts = shown(art)
        assert all(math.isfinite(c) for p in pts for c in p)
        assert last is None or max(math.dist(a, b) for a, b in zip(pts, last)) < 0.02, (stretch, scale, "snapped back")
        last = pts

doc.frame_current = 0
line = drawn("Tiny", [(0, 0), (1e-5, 0)])
rest = [(0.5, 0.2)]
art = drawn("Far", rest + [(0.6, 0.2)])
layers.active = art
assert ops.object.modifier_add(type="CURVE_DEFORM") == {'FINISHED'}
art.modifier_set(0, layer=line.name, shape=line.shapes[0][0], precision=0.0)
doc.frame_current = 10
line.set_point(1, (0.5, 0.5))
assert math.dist(shown(art)[0], rest[0]) < 1e-6, "a tiny line at rest bends nothing"
doc.frame_current = 0

print("Curve Deform stress tests passed")
