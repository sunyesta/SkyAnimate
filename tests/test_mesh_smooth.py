"""Mesh Deform's Smooth: the rows and columns of a drawn grid as smooth lines through its nodes, so what it bends has no corners
at them. The numbers are a real mouth cage's, moved at frame 10, its sides with no handles and one column squeezed some nine
times over, which a plain smooth line would overshoot and fold.

Run with:  OpenAnimate --background --python tests/test_mesh_smooth.py
"""

import math
import os
import shutil
import tempfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
doc.auto_key = True
doc.frame_current = 0
anchor = layers.active
REST = [(-0.06175, 0.37386), (0.00000, 0.37386), (0.06175, 0.37386),
        (-0.06175, 0.41675), (0.00000, 0.41675), (0.06175, 0.41675),
        (-0.06175, 0.45964), (0.00000, 0.45964), (0.06175, 0.45964)]
NOW = [(0.08959, 0.37386), (0.11012, 0.37386), (0.11634, 0.37386),
       (0.11047, 0.41675), (0.16451, 0.41675), (0.17175, 0.41675),
       (0.11047, 0.45964), (0.17222, 0.45964), (0.17945, 0.45964)]
CURVES = [[0, 1, 2], [3, 4, 5], [6, 7, 8], [0, 3, 6], [1, 4, 7], [2, 5, 8]]
made = [0]


def at(frame=10):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def new_layer(prefix):
    layers.active = anchor
    ops.layer.new()
    layer = layers.active
    made[0] += 1
    layer.name = "%s%d" % (prefix, made[0])
    return layer


def cage():
    """The grid at rest, its sides with no handles."""
    doc.frame_current = 0
    g = new_layer("Grid")
    assert g.add_primitive("GRID", [REST[0], REST[-1]], columns=2, rows=2)
    for i, p in enumerate(REST):
        g.set_point(i, p)
    for c in range(6):
        for i in range(3):
            end = g.points[CURVES[c][i]][:2]
            g.move_handle(c, i, "IN", end)
            g.move_handle(c, i, "OUT", end)
    return g


def bent_by(g, points, **settings):
    doc.frame_current = 0
    art = new_layer("Art")
    art.add_curve(points)
    layers.active = art
    assert ops.object.modifier_add(type="MESH_DEFORM") == {'FINISHED'}
    art.modifier_set(0, layer=g.name, shape=g.shapes[0][0], precision=0.0, **settings)
    return art


def move(g, points):
    doc.frame_current = 10
    for i, p in enumerate(points):
        g.set_point(i, p)
    at()


def sharpest(art, n):
    """The sharpest turn from one sample of a line to the next, in degrees."""
    p = [tuple(q) for q in art.evaluated()[0][:n]]
    worst = 0.0
    for a, b, c in zip(p, p[1:], p[2:]):
        u, v = (b[0] - a[0], b[1] - a[1]), (c[0] - b[0], c[1] - b[1])
        if math.hypot(*u) > 1e-7 and math.hypot(*v) > 1e-7:
            worst = max(worst, abs(math.degrees(math.atan2(u[0] * v[1] - u[1] * v[0], u[0] * v[0] + u[1] * v[1]))))
    return worst


x0, y0, x1, y1 = REST[0][0], REST[0][1], REST[-1][0], REST[-1][1]
n = 240
up_a_third = [(x0 + (x1 - x0) * 0.33, y0 + (y1 - y0) * (0.03 + 0.94 * k / (n - 1))) for k in range(n)]
up_right = [(x0 + (x1 - x0) * 0.8, y0 + (y1 - y0) * (0.03 + 0.94 * k / (n - 1))) for k in range(n)]
across = [(x0 + (x1 - x0) * (0.03 + 0.94 * k / (n - 1)), y0 + (y1 - y0) * 0.33) for k in range(n)]
across_near = [(x0 + (x1 - x0) * (0.03 + 0.94 * k / (n - 1)), y0 + (y1 - y0) * 0.1) for k in range(n)]

# --- Off by default, and in the modifier ----------------------------------------------------------------------------------
g = cage()
lines = [bent_by(g, points) for points in (up_a_third, up_right, across, across_near)]
assert lines[0].modifiers[0]["smooth"] is False, lines[0].modifiers[0]
on_a_node = bent_by(g, [REST[4], (x0 + (x1 - x0) * 0.7, y0 + (y1 - y0) * 0.7), REST[8]], smooth=True)
at_rest = [tuple(p) for p in on_a_node.evaluated()[0][:3]]
assert all(abs(a[0] - b[0]) + abs(a[1] - b[1]) < 1e-5 for a, b in zip(at_rest, on_a_node.points)), "nothing bends at rest"
move(g, NOW)

# --- The corners at the nodes go ----------------------------------------------------------------------------------------------
pointy = [sharpest(art, n) for art in lines[:2]]
for art in lines:
    art.modifier_set(0, smooth=True)
at()
smooth = [sharpest(art, n) for art in lines[:2]]
print("METRIC pointy", [round(v, 1) for v in pointy], "smooth", [round(v, 1) for v in smooth], flush=True)
assert min(pointy) > 15.0, ("the corner to take away", pointy)
assert max(smooth) < 3.0, ("still pointy", smooth)

# --- Art on a node goes to the node ---------------------------------------------------------------------------------------------
now = [tuple(p) for p in on_a_node.evaluated()[0][:3]]
assert math.dist(now[0], NOW[4]) < 1e-3, (now[0], NOW[4])
assert math.dist(now[2], NOW[8]) < 1e-3, (now[2], NOW[8])

# --- It never overshoots: a line across the squeezed column stays in order, here and squeezed four times tighter ----------------
tighter = list(NOW)
for row in range(3):
    middle, side = NOW[row * 3 + 1], NOW[row * 3 + 2]
    tighter[row * 3 + 1] = (side[0] + (middle[0] - side[0]) / 4, side[1] + (middle[1] - side[1]) / 4)
folds = []
for pose in (NOW, tighter):
    move(g, pose)
    for art in lines[2:]:
        xs = [p[0] for p in art.evaluated()[0][:n]]
        folds.append(max(a - b for a, b in zip(xs, xs[1:])))
move(g, NOW)
print("METRIC folds", ["%.6f" % v for v in folds], flush=True)
assert max(folds) < 1e-5, ("folded across the squeezed column", folds)

# --- It turns with the grid: a cage turned whole turns what it bends, and bends it no differently ---------------------------------
turning = []
for degrees in (12.0, 37.0, 90.0, 141.0):
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    turned = lambda p: (p[0] * c - p[1] * s, p[0] * s + p[1] * c)
    move(g, NOW)
    wanted = [[turned(p) for p in art.evaluated()[0][:n]] for art in lines]
    move(g, [turned(p) for p in NOW])
    turning.append(max(max(math.dist(p, q) for p, q in zip(art.evaluated()[0][:n], want)) for art, want in zip(lines, wanted)))
move(g, NOW)
print("METRIC turned", [round(v, 5) for v in turning], flush=True)
assert max(turning) < 1e-4, ("bent differently once turned", turning)

# --- A straight, even grid stays as it is ------------------------------------------------------------------------------------
plain = cage()
twin = cage()
a = bent_by(plain, across + up_right[:0])
b = bent_by(twin, across, smooth=True)
shift = lambda p: (p[0] * 1.2 + 0.03, p[1] * 0.9 + 0.05)
move(plain, [shift(p) for p in REST])
move(twin, [shift(p) for p in REST])
gap = max(math.dist(p, q) for p, q in zip(a.evaluated()[0][:n], b.evaluated()[0][:n]))
assert gap < 1e-4, ("moved a straight grid", gap)

# --- Saved -------------------------------------------------------------------------------------------------------------------------
kept = [tuple(p) for p in lines[0].evaluated()[0][:n]]
name = lines[0].name
folder = tempfile.mkdtemp()
path = os.path.join(folder, "mesh_smooth.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
shutil.rmtree(folder, ignore_errors=True)
art = next(l for l in layers.all() if l.name == name)
assert art.modifiers[0]["smooth"] is True, art.modifiers[0]
at()
assert max(math.dist(p, q) for p, q in zip(art.evaluated()[0][:n], kept)) < 1e-5

print("mesh smooth: all good")
