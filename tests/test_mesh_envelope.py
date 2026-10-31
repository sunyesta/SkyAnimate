"""Mesh Deform's Envelope method: in the outer cells the outline's sides keep what lies near them at its distance from
them, so a line along the edge keeps its width, easing off toward the next cells in. Inside those, what the mesh's inner
nodes and lines carry is exactly where Cells puts it. Nothing jumps across a cell's side or folds under a hard squeeze.

Run with:  OpenAnimate --background --python tests/test_mesh_envelope.py
"""

import os
import shutil
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True
anchor = layers.active


def close(a, b, eps=1e-3):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def new_layer(name, points=None):
    layers.active = anchor
    ops.layer.new()
    layer = layers.active
    layer.name = name
    if points:
        layer.add_curve(points)
    return layer


def new_grid(name, cells=3):
    layer = new_layer(name)
    assert layer.add_primitive("GRID", [(-1, -1), (1, 1)], columns=cells, rows=cells)
    return layer


def deform(layer, target):
    layers.active = layer
    assert ops.object.modifier_add(type="MESH_DEFORM") == {'FINISHED'}
    layer.modifier_set(0, layer=target.name, shape=target.shapes[0][0], precision=0.0)


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def reshape(layer, change, frame=10):
    doc.frame_current = frame
    for i, p in enumerate(layer.points):
        moved = change(p[0], p[1])
        if not close(moved, p[:2], 1e-6):
            layer.set_point(i, moved)
    at(frame)


def both(layer):
    """The shown points with Cells, then with Envelope (left on)."""
    layer.modifier_set(0, mesh_method="CELLS")
    at(doc.frame_current)
    cells = [tuple(p) for p in layer.evaluated()[0]]
    layer.modifier_set(0, mesh_method="ENVELOPE")
    at(doc.frame_current)
    return cells, [tuple(p) for p in layer.evaluated()[0]]


# --- The setting ----------------------------------------------------------------------------------------------
grid = new_grid("Grid")
art = new_layer("Art", [(0.9, -0.2), (0.9, 0.2), (0.0, 0.0)])
deform(art, grid)
m = art.modifiers[0]
assert m["mesh_method"] == "CELLS" and m["kind"] == "Grid", m
try:
    art.modifier_set(0, mesh_method="SOFT")
except ValueError:
    pass
else:
    raise AssertionError("not a method")

# --- A line along the edge keeps its width as the outline goes out ---------------------------------------------------
assert all(close(p, q[:2]) for p, q in zip(both(art)[1], art.points)), "on the setup frame nothing bends"
reshape(grid, lambda x, y: (1.6, y) if x > 0.99 else (x, y))
cells, envelope = both(art)
assert 1.6 - cells[0][0] > 0.13, ("with Cells it stretches with its cell", cells[0])
assert 1.6 - envelope[0][0] < 0.112, ("with Envelope it keeps its distance", envelope[0])
assert close(cells[2], envelope[2]), ("in the middle cell the mesh places it", cells[2], envelope[2])

# --- What the inner nodes carry stays exact, the outline still --------------------------------------------------------
doc.frame_current = 0
inner = new_grid("Inner")
carried = new_layer("Carried", [(1 / 3, 1 / 3), (0.0, 0.0), (0.1, 1 / 3)])
deform(carried, inner)
target = (0.5, 0.7)
reshape(inner, lambda x, y: target if close((x, y), (1 / 3, 1 / 3)) else (x, y))
cells, envelope = both(carried)
assert close(envelope[0], target), ("on an inner node", envelope[0], target)
assert close(cells[1], envelope[1]), ("in a cell off the outline", cells[1], envelope[1])
assert close(cells[2], envelope[2]), ("on an inner line", cells[2], envelope[2])

# With two cells a side there are no lines inside the outer cells, only the middle node, and it still places exactly.
doc.frame_current = 0
pair = new_grid("Pair", 2)
hub = new_layer("Hub", [(0.0, 0.0), (1.0, 0.3)])
deform(hub, pair)
reshape(pair, lambda x, y: (0.3, 0.4) if close((x, y), (0.0, 0.0)) else (x, y))
cells, envelope = both(hub)
assert close(envelope[0], (0.3, 0.4)), envelope[0]
assert close(envelope[1], (1.0, 0.3)), ("on the outline, which hasn't moved", envelope[1])

# --- Nothing jumps across a side, or at the outline -------------------------------------------------------------------
doc.frame_current = 0
wavy = new_grid("Wavy")
n = 240
rows = (0.5, 0.9)
sweeps = []
for y in rows:
    sweeps.append(new_layer("Row %s" % y, [(-1.3 + 2.6 * k / (n - 1), y) for k in range(n)]))
    deform(sweeps[-1], wavy)
sweeps.append(new_layer("Column", [(0.8, -1.3 + 2.6 * k / (n - 1)) for k in range(n)]))
deform(sweeps[-1], wavy)
reshape(wavy, lambda x, y: (x * 1.4 + 0.2 * y, y * 0.9) if abs(x) > 0.99 or abs(y) > 0.99 else (x + 0.1, y - 0.05))
for sweep in sweeps:
    cells, envelope = both(sweep)
    biggest = max(abs(a[0] - b[0]) + abs(a[1] - b[1]) for a, b in zip(envelope, envelope[1:]))
    usual = max(abs(a[0] - b[0]) + abs(a[1] - b[1]) for a, b in zip(cells, cells[1:]))
    assert biggest < usual * 2.5, (sweep.name, "a jump", biggest, usual)

# --- A squeeze doesn't fold, as far as the cell itself doesn't -----------------------------------------------------------
doc.frame_current = 0
press = new_grid("Press")
squeezed = new_layer("Squeezed", [(0.34 + 0.65 * k / 79, 0.1) for k in range(80)])
deform(squeezed, press)
right = [i for i, p in enumerate(press.points) if p[0] > 0.99]
for side in (0.9, 0.8, 0.7, 0.6):
    doc.frame_current = 10
    for i in right:
        press.set_point(i, (side, press.points[i][1]))
    at(10)
    cells, envelope = both(squeezed)
    for name, shown in (("cells", cells), ("envelope", envelope)):
        xs = [p[0] for p in shown]
        assert all(a <= b + 1e-5 for a, b in zip(xs, xs[1:])), (name, side, "folded", xs)

# --- One cell: the whole of it, as a cage ------------------------------------------------------------------------------
doc.frame_current = 0
one = new_layer("One")
assert one.add_primitive("GRID", [(-1, -1), (1, 1)], columns=1, rows=1)
band = new_layer("Band", [(0.9, 0.0), (0.0, 0.0)])
deform(band, one)
reshape(one, lambda x, y: (1.6, y) if x > 0.99 else (x, y))
cells, envelope = both(band)
assert 1.6 - envelope[0][0] < 1.6 - cells[0][0], (cells[0], envelope[0])

# --- Blend, and saved ----------------------------------------------------------------------------------------------
at(10)
art = next(l for l in layers.all() if l.name == "Art")
art.modifier_set(0, mesh_method="ENVELOPE", blend=0.0)
at(10)
sharp = art.evaluated()[0][0]
art.modifier_set(0, blend=1.0)
at(10)
assert not close(sharp, art.evaluated()[0][0], 1e-4), "Blend says how evenly the sides share it"
kept = art.evaluated()[0][0]

folder = tempfile.mkdtemp()
path = os.path.join(folder, "mesh_envelope.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
shutil.rmtree(folder, ignore_errors=True)
art = next(l for l in layers.all() if l.name == "Art")
assert art.modifiers[0]["mesh_method"] == "ENVELOPE" and art.modifiers[0]["blend"] == 1.0, art.modifiers[0]
at(10)
assert close(art.evaluated()[0][0], kept), (art.evaluated()[0][0], kept)

print("mesh envelope: all good")
