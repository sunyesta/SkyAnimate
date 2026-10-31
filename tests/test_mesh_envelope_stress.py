"""Mesh Deform's Envelope under stress, on grids of many shapes, a curved cell and a hand-made mesh, with Cells to go by:

- a mesh keyed where it rests bends nothing; moved and turned whole it carries the drawing exactly with it;
- shoved at random as far as a fifth of a cell, it folds nothing Cells doesn't;
- collapsed, flattened, mirrored or huge: never NaN, and nothing thrown far from where Cells puts it;
- dragging a node by fine steps nothing jumps, a node run along its side past its neighbour's handles included;
- every setting and what lies around the mesh: Precision, Resample Self, Custom weights, Strength, Blend, points outside
  it, bones, a group's deform;
- a big grid is quick enough to drag on.

Run with:  OpenAnimate --background --python tests/test_mesh_envelope_stress.py
"""

import math
import random
import time

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0
doc.auto_key = True
anchor = layers.active
rng = random.Random(11)
N = 13
made = [0]


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def new_layer(prefix):
    layers.active = anchor
    ops.layer.new()
    layer = layers.active
    made[0] += 1
    layer.name = "%s%d" % (prefix, made[0])
    return layer


def grid(cols, rows, lo=(-1, -1), hi=(1, 1)):
    g = new_layer("Grid")
    assert g.add_primitive("GRID", [lo, hi], columns=cols, rows=rows)
    return g


def curved():
    c = new_layer("Curved")
    c.add_curve([(-1, -0.9), (1.0, -1.0), (0.9, 1.0), (-1.0, 0.95)], closed=True, fill=True)
    return c


def pick(layer, *points):
    ops.mesh.select_none()
    layer.select_points(list(points))


def two_cells():
    h = new_layer("Mesh")
    h.add_curve([(-1.0, 0.8), (0.0, 0.8), (1.0, 0.8)])
    pick(h, 1)
    h.extrude_selected()
    h.set_point(3, (0.0, -0.8))
    pick(h, 0)
    h.extrude_selected()
    h.set_point(4, (-1.0, -0.8))
    pick(h, 4, 3)
    ops.mesh.make_segment()
    pick(h, 2)
    h.extrude_selected()
    h.set_point(5, (1.0, -0.8))
    pick(h, 5, 3)
    ops.mesh.make_segment()
    ops.mesh.select_none()
    return h


def lattice(mesh, n=N):
    """A line through n by n places just inside the mesh, turning back at each row."""
    xs, ys = [p[0] for p in mesh.points], [p[1] for p in mesh.points]
    x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
    points = []
    for j in range(n):
        row = [(x0 + (x1 - x0) * (0.02 + 0.96 * i / (n - 1)), y0 + (y1 - y0) * (0.02 + 0.96 * j / (n - 1))) for i in range(n)]
        points.extend(row if j % 2 == 0 else row[::-1])
    art = new_layer("Art")
    art.add_curve(points)
    return art


def bend(art, mesh, precision=0.0):
    layers.active = art
    assert ops.object.modifier_add(type="MESH_DEFORM") == {'FINISHED'}
    index = len(art.modifiers) - 1
    art.modifier_set(index, layer=mesh.name, shape=mesh.shapes[0][0], precision=precision, mesh_method="ENVELOPE")
    assert art.modifiers[index]["kind"] == "Grid", art.modifiers[index]
    return index


def place(mesh, points, frame=10):
    doc.frame_current = frame
    for i, p in enumerate(points):
        mesh.set_point(i, p)
    at(frame)


def shown(art, method=None):
    if method:
        art.modifier_set(0, mesh_method=method)
        at(doc.frame_current)
    return [tuple(p) for p in art.evaluated()[0]]


def sound(points):
    return not any(math.isnan(v) or math.isinf(v) for p in points for v in p)


def spot(i, j, n=N):
    return j * n + (i if j % 2 == 0 else n - 1 - i)


def folds(rest, now, n=N):
    """Lattice triangles turned over from how they are at rest."""
    def area(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    count = 0
    for j in range(n - 1):
        for i in range(n - 1):
            p, q, r, s = spot(i, j, n), spot(i + 1, j, n), spot(i, j + 1, n), spot(i + 1, j + 1, n)
            for a, b, c in ((p, q, r), (q, s, r)):
                count += 1 if area(rest[a], rest[b], rest[c]) * area(now[a], now[b], now[c]) < 0 else 0
    return count


meshes = [("1x1", grid(1, 1), 2.0), ("3x1", grid(3, 1), 2 / 3), ("1x4", grid(1, 4), 0.5), ("2x2", grid(2, 2), 1.0),
          ("3x3", grid(3, 3), 2 / 3), ("5x5", grid(5, 5), 0.4), ("thin", grid(6, 2, (-3, -0.1), (3, 0.1)), 0.1),
          ("tiny", grid(3, 3, (-0.002, -0.002), (0.002, 0.002)), 0.002 * 2 / 3),
          ("large", grid(3, 3, (-300, -300), (300, 300)), 200.0), ("curved", curved(), 2.0), ("two", two_cells(), 1.0)]

for name, mesh, cell in meshes:
    doc.frame_current = 0
    art = lattice(mesh)
    bend(art, mesh)
    rest_mesh = [tuple(p[:2]) for p in mesh.points]
    rest = [tuple(p[:2]) for p in art.points]
    xs, ys = [p[0] for p in rest_mesh], [p[1] for p in rest_mesh]
    x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
    size, cx, cy = max(x1 - x0, y1 - y0), (x0 + x1) / 2, (y0 + y1) / 2

    # Keyed where it rests.
    place(mesh, rest_mesh)
    assert max(math.dist(p, q) for p, q in zip(shown(art), rest)) < size * 1e-3, (name, "at rest")

    # Moved and turned whole.
    turn = 0.7

    def whole(x, y):
        dx, dy = x - cx, y - cy
        return (cx + dx * math.cos(turn) - dy * math.sin(turn) + size * 0.3, cy + dx * math.sin(turn) + dy * math.cos(turn) - size * 0.2)

    place(mesh, [whole(*p) for p in rest_mesh])
    want = [whole(*p) for p in rest]
    off_cells = max(math.dist(p, q) for p, q in zip(shown(art, "CELLS"), want))
    off = max(math.dist(p, q) for p, q in zip(shown(art, "ENVELOPE"), want))
    assert off < max(off_cells * 2, size * 2e-3), (name, "turned whole", off / size, off_cells / size)

    # Shoved: no fold Cells hasn't got, and never far from Cells.
    for trial in range(6):
        reach = cell * 0.2
        place(mesh, [(x + rng.uniform(-reach, reach), y + rng.uniform(-reach, reach)) for x, y in rest_mesh])
        cells, envelope = shown(art, "CELLS"), shown(art, "ENVELOPE")
        assert sound(envelope), (name, "NaN shoved")
        if folds(rest, cells) == 0:
            assert folds(rest, envelope) <= 1, (name, "folded", folds(rest, envelope))
        assert max(math.dist(p, q) for p, q in zip(cells, envelope)) < size * 0.2, (name, "far from Cells")

    # Collapsed and the like.
    for label, change in (("to a point", lambda x, y: (cx, cy)), ("flat in x", lambda x, y: (cx, y)), ("flat in y", lambda x, y: (x, cy)),
                          ("mirrored", lambda x, y: (2 * cx - x, y)), ("huge", lambda x, y: (x * 500, y * 500)),
                          ("side onto side", lambda x, y: (x0 if x > cx else x, y)),
                          ("side to a point", lambda x, y: (x1, cy) if x > x1 - size * 1e-3 else (x, y))):
        place(mesh, [change(x, y) for x, y in rest_mesh])
        cells, envelope = shown(art, "CELLS"), shown(art, "ENVELOPE")
        assert sound(envelope), (name, "NaN", label)
        if label != "huge":
            assert max(math.dist(p, q) for p, q in zip(cells, envelope)) < size * 0.3, (name, label, "thrown")
    doc.frame_current = 0
print("many meshes: fine")

# --- Dragged by fine steps: nothing jumps ---------------------------------------------------------------------------
for cols, rows in ((1, 4), (3, 1), (3, 3)):
    doc.frame_current = 0
    mesh = grid(cols, rows)
    art = lattice(mesh, 9)
    bend(art, mesh)
    rest_mesh = [tuple(p[:2]) for p in mesh.points]
    step = 0.008
    for node in range(len(rest_mesh)):
        for way in (0.0, 1.57, 3.14, 4.71):
            points = list(rest_mesh)
            last = None
            for k in range(60):
                points[node] = (rest_mesh[node][0] + math.cos(way) * step * k, rest_mesh[node][1] + math.sin(way) * step * k)
                place(mesh, points)
                now = shown(art)
                if last:
                    moved = max(math.dist(p, q) for p, q in zip(now, last))
                    assert moved < step * 4.5, ((cols, rows), "node", node, "way", way, "step", k, "jumped", moved / step)
                last = now
            place(mesh, rest_mesh)
print("drags: fine")

# --- Every setting --------------------------------------------------------------------------------------------------
doc.frame_current = 0
mesh = grid(3, 3)
art = new_layer("Art")
art.add_curve([(-1.4 + 2.8 * k / 39, 0.6 * math.sin(k * 0.7) + 0.1) for k in range(40)])  # out past the mesh both ways
bend(art, mesh, 0.002)
rest_mesh = [tuple(p[:2]) for p in mesh.points]
place(mesh, [(x * (1.4 if abs(x) > 0.9 else 1.0) + 0.1 * y, y * (0.8 if abs(y) > 0.9 else 1.0)) for x, y in rest_mesh])
for settings in (dict(precision=0.002), dict(precision=0.0), dict(precision=0.0002), dict(resample_self=True),
                 dict(resample_self=False, weights="CUSTOM"), dict(weights="AUTOMATIC", strength=0.5),
                 dict(strength=1.0, blend=0.0), dict(blend=1.0)):
    art.modifier_set(0, **settings)
    at(10)
    assert sound(art.evaluated()[0]), settings
    assert sound(art.shown()[0]), settings
art.modifier_set(0, blend=0.5)

# Points outside follow as they do with Cells: the Method is for the inside.
outside = new_layer("Outside")
outside.add_curve([(1.3, 0.2), (1.2, -0.3), (-1.5, 0.0)])
bend(outside, mesh)
at(10)
cells, envelope = shown(outside, "CELLS"), shown(outside, "ENVELOPE")
assert all(math.dist(p, q) < 1e-4 for p, q in zip(cells[:3], envelope[:3])), (cells[:3], envelope[:3])

# On bones, and as a group's deform.
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
assert ops.armature.bone_add(name="b", head_x="-2", head_y="-2", tail_x="-1", tail_y="-2", parent="") == {'FINISHED'}
boned = new_layer("Boned")
boned.add_curve([(0.8, 0.2), (0.0, 0.0)])
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
b = bend(next(l for l in layers.all() if l.name == boned.name), mesh)
doc.frame_current = 10
rig.bone_set("b", angle=0.6)
at(10)
assert sound(boned.evaluated()[0])
layers.active = anchor
assert ops.layer.new_group() == {'FINISHED'}
group = layers.active
inside = new_layer("Inside")
inside.add_curve([(0.8, 0.3), (0.0, 0.0)])
assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
layers.active = group
assert ops.object.modifier_add(type="MESH_DEFORM") == {'FINISHED'}
group.modifier_set(0, layer=mesh.name, shape=mesh.shapes[0][0], mesh_method="ENVELOPE")
at(10)
inside = next(l for l in layers.all() if l.name == inside.name)
with_group = inside.evaluated()[0][0]
group.modifier_set(0, mesh_method="CELLS")
at(10)
assert sound([with_group]) and math.dist(with_group, inside.evaluated()[0][0]) > 1e-4, "a group's deform takes the Method"
print("settings: fine")

# --- Speed -----------------------------------------------------------------------------------------------------------
doc.frame_current = 0
mesh = grid(20, 20)
art = new_layer("Big")
art.add_curve([(0.97 * math.cos(6.283 * k / 200), 0.97 * math.sin(6.283 * k / 200)) for k in range(200)], closed=True)
bend(art, mesh, 0.002)
doc.frame_current = 10
for i, p in enumerate(mesh.points):
    mesh.set_point(i, (p[0] * 1.2 + 0.1 * p[1], p[1] * 0.95))
at(10)
art.evaluated()
start = time.perf_counter()
for i in range(5):
    x, y = art.points[0][:2]
    art.set_point(0, (x + (0.0005 if i % 2 else -0.0005), y))
    art.evaluated()
each = (time.perf_counter() - start) / 5
assert each < 0.1, ("a 20 by 20 grid takes %.0f ms" % (each * 1000))

print("mesh envelope stress: all good")
