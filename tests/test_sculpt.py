"""The Sculpt tool's brushes (anim/sculpt.hh) through sculpt.stroke, its presets, and its settings.

Run with:  OpenAnimate --background --python tests/test_sculpt.py
"""

import math
import os
import shutil
import tempfile

import oanim


def close(a, b, eps=1e-4):
    return abs(a - b) < eps


def points(layer):
    return [(p[0], p[1]) for p in layer.points]


def path(*pts):
    return ";".join("%r,%r" % p for p in pts)


layer = oanim.data.layers.active


def fresh_line():
    """A line along x: points at x = 0..4, y = 0 (layer space is document space here)."""
    while len(layer.points):
        oanim.ops.mesh.select_all()
        oanim.ops.mesh.delete()
    layer.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0), (3.0, 0.0), (4.0, 0.0)])
    oanim.ops.mesh.select_none()


# The tool is on the tool bar; its settings start at the Grab preset.
oanim.ops.wm.tool_sculpt()
s = oanim.wm.sculpt_settings()
assert s["brush"] == "GRAB" and s["preset"] == "Grab", s

# Built-in presets, grouped Move and Shape; picking one copies it into the tool.
presets = {p["name"]: p for p in oanim.wm.sculpt_presets()}
for name, brush, category in [("Grab", "GRAB", "Move"), ("Push", "PUSH", "Move"), ("Pinch", "PINCH", "Move"),
                              ("Twist", "TWIST", "Move"), ("Smooth", "SMOOTH", "Shape"),
                              ("Thickness", "THICKNESS", "Shape")]:
    assert presets[name]["brush"] == brush and presets[name]["category"] == category and presets[name]["builtin"], name
oanim.ops.sculpt.preset_load(name="Smooth Strong")
s = oanim.wm.sculpt_settings()
assert s["brush"] == "SMOOTH" and s["falloff"] == "SPHERE" and close(s["strength"], 1.0), s
oanim.ops.sculpt.brush_set(brush="PUSH")
assert oanim.wm.sculpt_settings()["preset"] == "Push"
oanim.wm.set_sculpt(radius=120, strength=0.3, falloff="CONSTANT", selected_only=True)
s = oanim.wm.sculpt_settings()
assert s["radius"] == 120 and close(s["strength"], 0.3) and s["falloff"] == "CONSTANT" and s["selected_only"], s
oanim.wm.set_sculpt(selected_only=False)

# Grab: the points under the brush where the stroke starts follow the mouse, by the falloff.
fresh_line()
count = len(layer.points)
oanim.ops.sculpt.stroke(brush="GRAB", path=path((2.0, 0.0), (2.0, 0.5), (2.0, 1.0)), radius=1.5, strength=1.0,
                        falloff="CONSTANT")
p = points(layer)
assert len(p) == count, "it never adds points"
assert close(p[2][1], 1.0) and close(p[1][1], 1.0) and close(p[3][1], 1.0), p
assert close(p[0][1], 0.0) and close(p[4][1], 0.0), "outside the brush: untouched"
oanim.ops.ed.undo()
assert close(points(layer)[2][1], 0.0), "one undo step"

# Smooth falloff: halfway out gets 1 - smoothstep(2/3) of the move.
oanim.ops.sculpt.stroke(brush="GRAB", path=path((2.0, 0.0), (2.0, 1.0)), radius=1.5, strength=1.0, falloff="SMOOTH")
t = 1.0 / 1.5
assert close(points(layer)[1][1], 1.0 - t * t * (3 - 2 * t)), points(layer)
oanim.ops.ed.undo()

# Grab keeps to the points it took: dragging far away doesn't pick up others.
oanim.ops.sculpt.stroke(brush="GRAB", path=path((0.0, 0.0), (1.0, 0.0), (2.0, 0.0)), radius=0.5, strength=1.0,
                        falloff="CONSTANT")
p = points(layer)
assert close(p[0][0], 2.0) and close(p[1][0], 1.0) and close(p[2][0], 2.0), p
oanim.ops.ed.undo()

# Push: whatever is under the brush as it goes moves along with it.
oanim.ops.sculpt.stroke(brush="PUSH", path=path((0.0, 0.0), (0.0, 0.1), (1.0, 0.1)), radius=0.3, strength=1.0,
                        falloff="CONSTANT")
p = points(layer)
assert close(p[0][1], 0.1) and p[1][1] == 0.0, ("point 1 was never under the brush", p)
oanim.ops.ed.undo()

# Selected Only: an unselected point stays put though the brush covers it.
layer.select_points([2])
oanim.ops.sculpt.stroke(brush="GRAB", path=path((2.0, 0.0), (2.0, 1.0)), radius=1.5, strength=1.0,
                        falloff="CONSTANT", selected_only="1")
p = points(layer)
assert close(p[2][1], 1.0) and close(p[1][1], 0.0) and close(p[3][1], 0.0), p
oanim.ops.ed.undo()
oanim.ops.mesh.select_none()

# Pinch pulls toward the middle (invert: away); Twist turns about it.
oanim.ops.sculpt.stroke(brush="PINCH", path=path((2.0, 0.0)), radius=1.5, strength=1.0, falloff="CONSTANT")
p = points(layer)
assert close(p[1][0], 1.5) and close(p[3][0], 2.5) and close(p[2][0], 2.0), p
oanim.ops.ed.undo()
oanim.ops.sculpt.stroke(brush="PINCH", path=path((2.0, 0.0)), radius=1.5, strength=1.0, falloff="CONSTANT", invert="1")
assert close(points(layer)[1][0], 0.5), points(layer)
oanim.ops.ed.undo()
oanim.ops.sculpt.stroke(brush="TWIST", path=path((2.0, 0.0)), radius=1.5, strength=1.0, falloff="CONSTANT")
p = points(layer)
assert close(p[3][0], 2.0 + math.cos(0.5)) and close(p[3][1], math.sin(0.5)), ("turned half a radian", p)
oanim.ops.ed.undo()

# Thickness widens the line at the points (invert: thins it).
oanim.ops.sculpt.stroke(brush="THICKNESS", path=path((2.0, 0.0)), radius=0.5, strength=1.0, falloff="CONSTANT")
r = layer.radii
assert r[2] > 1.0 and r[1] == 1.0 and r[0] == 1.0, ("only the point under the brush", r)
oanim.ops.ed.undo()
oanim.ops.sculpt.stroke(brush="THICKNESS", path=path((2.0, 0.0)), radius=0.5, strength=1.0, falloff="CONSTANT",
                        invert="1")
assert layer.radii[2] < 1.0, layer.radii
oanim.ops.ed.undo()

# Smooth evens a zigzag out toward its neighbours; an open curve's ends stay.
while len(layer.points):
    oanim.ops.mesh.select_all()
    oanim.ops.mesh.delete()
layer.add_curve([(0.0, 0.0), (1.0, 1.0), (2.0, -1.0), (3.0, 1.0), (4.0, 0.0)])
oanim.ops.mesh.select_none()
before = sum(abs(y) for _, y in points(layer)[1:4])
oanim.ops.sculpt.stroke(brush="SMOOTH", path=path((2.0, 0.0), (2.0, 0.0), (2.0, 0.0)), radius=5.0, strength=1.0,
                        falloff="CONSTANT")
p = points(layer)
assert sum(abs(y) for _, y in p[1:4]) < before * 0.8, p
assert p[0] == (0.0, 0.0) and p[4] == (4.0, 0.0), ("ends stay", p)
oanim.ops.ed.undo()

# The brush works on the line, not just its points: a small Grab between two far-apart points bends the
# curve there (its points stay, its handles are fitted to the dragged line).
def bezier(p0, p1, p2, p3, t):
    s_ = 1 - t
    return tuple(s_ ** 3 * a + 3 * s_ * s_ * t * b + 3 * s_ * t * t * c + t ** 3 * d for a, b, c, d in zip(p0, p1, p2, p3))


def segment(lay, curve, i):
    pts, curves = lay.shown()
    cps = curves[curve][0]
    return pts[cps[i][0]], cps[i][2], cps[i + 1][1], pts[cps[i + 1][0]]


while len(layer.points):
    oanim.ops.mesh.select_all()
    oanim.ops.mesh.delete()
layer.add_curve([(0.0, 0.0), (4.0, 0.0)])
oanim.ops.mesh.select_none()
oanim.ops.sculpt.stroke(brush="GRAB", path=path((2.0, 0.0), (2.0, 0.5)), radius=0.6, strength=1.0, falloff="SMOOTH")
p = points(layer)
assert close(p[0][1], 0.0) and close(p[1][1], 0.0), ("its points stay", p)
middle = bezier(*segment(layer, 0, 0), 0.5)
assert middle[1] > 0.1, ("the line between them rose", middle)
oanim.ops.ed.undo()

# Segments the brush didn't touch keep their points and handles exactly.
while len(layer.points):
    oanim.ops.mesh.select_all()
    oanim.ops.mesh.delete()
layer.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0), (3.0, 0.0), (4.0, 0.0)])
oanim.ops.mesh.select_none()
before = segment(layer, 0, 0)
oanim.ops.sculpt.stroke(brush="GRAB", path=path((3.5, 0.0), (3.5, 0.4)), radius=0.3, strength=1.0, falloff="SMOOTH")
assert segment(layer, 0, 0) == before, (segment(layer, 0, 0), before)
assert bezier(*segment(layer, 0, 3), 0.5)[1] > 0.1, "the touched one bent"

# A smooth (aligned) point keeps its handles in line across a refitted segment.
oanim.ops.ed.undo()
oanim.ops.mesh.select_all()
oanim.ops.mesh.handles_align()
oanim.ops.mesh.select_none()
oanim.ops.sculpt.stroke(brush="GRAB", path=path((2.5, 0.0), (2.5, 0.6)), radius=0.6, strength=1.0, falloff="SMOOTH")
shown_points = layer.shown()[0]
for (index, hin, hout, aligned, *_rest) in layer.shown()[1][0][0][1:-1]:
    pt = shown_points[index]
    a = (hin[0] - pt[0], hin[1] - pt[1])
    b = (hout[0] - pt[0], hout[1] - pt[1])
    cross = a[0] * b[1] - a[1] * b[0]
    assert abs(cross) < 1e-3 * max(1e-9, (a[0] ** 2 + a[1] ** 2) ** 0.5 * (b[0] ** 2 + b[1] ** 2) ** 0.5), (pt, hin, hout)
oanim.ops.ed.undo()

# Starting a stroke doesn't move the line: a brush that barely moves leaves even sharply bent segments (where
# a fit by distance along the samples would bend them out of shape) as they were.
while len(layer.points):
    oanim.ops.mesh.select_all()
    oanim.ops.mesh.delete()
layer.add_curve([(0.0, 0.0), (1.0, 0.0), (1.05, 1.0), (0.0, 1.1), (0.1, 0.15), (0.9, 0.2), (0.3, 0.8)])
oanim.ops.mesh.select_none()
segments = len(layer.shown()[1][0][0]) - 1
before = [[bezier(*segment(layer, 0, i), k / 16) for k in range(17)] for i in range(segments)]
for i in range(segments):
    a, _, _, d = segment(layer, 0, i)
    m = ((a[0] + d[0]) / 2, (a[1] + d[1]) / 2)
    oanim.ops.sculpt.stroke(brush="GRAB", path=path(m, (m[0] + 1e-6, m[1])), radius=1.0, strength=1.0)
    after = [[bezier(*segment(layer, 0, j), k / 16) for k in range(17)] for j in range(segments)]
    worst = max(abs(p[0] - q[0]) + abs(p[1] - q[1]) for sa, sb in zip(before, after) for p, q in zip(sa, sb))
    assert worst < 1e-3, ("the line jumped when the stroke started", i, worst)
    oanim.ops.ed.undo()

# The same with detail: a handle facing finer detail shows shorter than it is stored, and stays as it shows.
while len(layer.points):
    oanim.ops.mesh.select_all()
    oanim.ops.mesh.delete()
layer.add_curve([(0.0, 0.0), (1.0, 0.0), (1.2, 1.0), (0.0, 1.1)], closed=True, fill=True)
oanim.ops.mesh.select_all()
assert oanim.ops.mesh.detail_level_set(level="1") == {'FINISHED'}
layer.split_segment(0, 0, 0.3)
layer.split_segment(0, 3, 0.6)
oanim.ops.mesh.select_none()
before = layer.shape_outline(0, 32)[0]
pts, curves = layer.shown()
cps = curves[0][0]
for i in range(len(cps)):
    a, d = pts[cps[i][0]], pts[cps[(i + 1) % len(cps)][0]]
    m = ((a[0] + d[0]) / 2, (a[1] + d[1]) / 2)
    oanim.ops.sculpt.stroke(brush="GRAB", path=path(m, (m[0] + 1e-6, m[1])), radius=0.5, strength=1.0)
    after = layer.shape_outline(0, 32)[0]
    worst = max(abs(p[0] - q[0]) + abs(p[1] - q[1]) for p, q in zip(before, after))
    assert len(after) == len(before) and worst < 1e-3, ("the line jumped where it has detail", i, worst)
    oanim.ops.ed.undo()

# Away from frame 0 it poses the drawing: auto-key keys the points it moved.
fresh_line()
doc = oanim.data.document
doc.frame_current = 10
oanim.ops.sculpt.stroke(brush="GRAB", path=path((2.0, 0.0), (2.0, 1.0)), radius=0.5, strength=1.0,
                        falloff="CONSTANT")
doc.frame_current = 11
assert [k["frame"] for k in layer.keyframes("points[2].pos")] == [0, 10], layer.keyframes("points[2].pos")
assert layer.keyframes("points[0].pos") == [], "points it didn't touch get no keys"
doc.frame_current = 0
assert close(points(layer)[2][1], 0.0), "the setup pose is unchanged"
doc.frame_current = 10
assert close(points(layer)[2][1], 1.0)

# Saved presets go to the asset library, under Mine; built-ins can't be deleted.
library = tempfile.mkdtemp(prefix="oanim_sculpt_")
oanim.ops.preferences.asset_library_set(directory=library)
oanim.wm.set_sculpt(brush="PINCH", radius=33, strength=0.7, falloff="SPHERE")
oanim.ops.sculpt.preset_save(name="Tight Pinch")
assert oanim.wm.sculpt_settings()["preset"] == "Tight Pinch"
assert os.path.exists(os.path.join(library, "Sculpt Brushes", "Tight Pinch.oasculpt"))
saved = {p["name"]: p for p in oanim.wm.sculpt_presets()}["Tight Pinch"]
assert saved["category"] == "Mine" and not saved["builtin"] and saved["brush"] == "PINCH" and saved["radius"] == 33, saved
oanim.ops.sculpt.preset_save(name="Tight Pinch")
assert "Tight Pinch.001" in {p["name"] for p in oanim.wm.sculpt_presets()}, "a taken name gets a number"
oanim.ops.sculpt.preset_remove(name="Tight Pinch.001")
assert "Tight Pinch.001" not in {p["name"] for p in oanim.wm.sculpt_presets()}
assert oanim.ops.sculpt.preset_remove(name="Grab") == {"CANCELLED"}, "built-in"

# The Sculpt keymap picks brushes while the tool is on.
oanim.ops.wm.tool_sculpt()
assert oanim.wm.event_simulate("S", area="Viewport")
assert oanim.wm.sculpt_settings()["brush"] == "SMOOTH"
assert oanim.wm.event_simulate("G", area="Viewport")
assert oanim.wm.sculpt_settings()["brush"] == "GRAB"

shutil.rmtree(library, ignore_errors=True)

# X mirror: across the layer's own axis (a layer without a side), widths too.
doc.frame_current = 0
oanim.ops.layer.new()
sym = oanim.data.layers.active
sym.add_curve([(-2.0, 0.0), (-1.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
oanim.ops.mesh.select_none()
oanim.ops.sculpt.stroke(brush="GRAB", path=path((2.0, 0.0), (2.0, 0.5)), radius=0.3, strength=1.0,
                        falloff="CONSTANT", mirror=1)
p = points(sym)
assert close(p[3][1], 0.5) and close(p[0][1], 0.5), ("the other side follows", p)
before = sym.radii
oanim.ops.sculpt.stroke(brush="THICKNESS", path=path((2.0, 0.5), (2.0, 0.5)), radius=0.3, strength=1.0,
                        falloff="CONSTANT", mirror=1)
after = sym.radii
assert after[3] > before[3] and abs(after[0] - after[3]) < 1e-5, ("widths mirror", before, after)
# Whatever is selected (sculpting doesn't go by the selection): every point selected, the other side still
# follows, dab after dab, on the setup frame and posing.
oanim.ops.mesh.select_all()
oanim.ops.sculpt.stroke(brush="GRAB", path=path((2.0, 0.5), (2.0, 1.0)), radius=0.3, strength=1.0,
                        falloff="CONSTANT", mirror=1)
p = points(sym)
assert close(p[3][1], 1.0) and close(p[0][1], 1.0), ("all selected, the other side follows", p)
oanim.ops.sculpt.stroke(brush="PUSH", path=path((2.0, 1.0), (2.0, 1.2), (2.0, 1.4)), radius=0.3, strength=1.0,
                        falloff="CONSTANT", mirror=1)
p = points(sym)
assert close(p[0][1], p[3][1]) and p[3][1] > 1.0, ("dab after dab", p)
doc.frame_current = 12
oanim.ops.sculpt.stroke(brush="GRAB", path=path((2.0, p[3][1]), (2.0, p[3][1] + 0.3)), radius=0.3, strength=1.0,
                        falloff="CONSTANT", mirror=1)
q = points(sym)
assert close(q[0][1], q[3][1]) and close(q[3][1], p[3][1] + 0.3), ("posing too", q)
doc.frame_current = 0
oanim.ops.mesh.select_none()

# A point on the axis (the top of a head) is its own image: it stays on the axis, and its two handles stay
# each other's mirror image, so a brush beside it doesn't make the shape lopsided.
oanim.ops.layer.new()
head = oanim.data.layers.active
head.add_curve([(0.0, 1.0), (1.0, 0.0), (0.0, -1.0), (-1.0, 0.0)], closed=True)
oanim.ops.mesh.select_all()
oanim.ops.sculpt.stroke(brush="GRAB", path=path((0.15, 1.0), (0.35, 1.25)), radius=0.3, strength=1.0,
                        falloff="SMOOTH", mirror=1)
shown_points, shown_curves = head.shown()
top = next(cp for cp in shown_curves[0][0] if cp[0] == 0)
tp = shown_points[0]
h_in = (top[1][0] - tp[0], top[1][1] - tp[1])
h_out = (top[2][0] - tp[0], top[2][1] - tp[1])
assert abs(tp[0]) < 1e-5 and tp[1] > 1.0, ("on the axis, moved up", tp)
assert close(h_in[0], -h_out[0]) and close(h_in[1], h_out[1]), ("its handles mirror each other", h_in, h_out)
p = points(head)
assert close(p[1][0], -p[3][0]) and close(p[1][1], p[3][1]), ("the sides still mirror", p)


# Both sides are sculpted at once (each dab also made mirrored, as Blender's symmetry): a circle of 12 points,
# pushed on its right, twisted, and grabbed right at the middle, stays its own mirror image every time.
import math
oanim.ops.layer.new()
ring = oanim.data.layers.active
ring.add_curve([(math.sin(2 * math.pi * k / 12), math.cos(2 * math.pi * k / 12)) for k in range(12)], closed=True)
oanim.ops.mesh.select_all()


def mirrored(layer):
    pts = points(layer)
    for x, y in pts:
        if not any(abs(x + u) < 1e-4 and abs(y - v) < 1e-4 for u, v in pts):
            return False
    return True


oanim.ops.sculpt.stroke(brush="PUSH", path=path((0.9, 0.3), (1.1, 0.3), (1.3, 0.35)), radius=0.4, strength=1.0,
                        falloff="SMOOTH", mirror=1)
assert mirrored(ring) and min(x for x, y in points(ring)) < -1.01, ("pushed out on both sides", points(ring))
oanim.ops.sculpt.stroke(brush="TWIST", path=path((0.8, -0.6), (0.8, -0.6)), radius=0.5, strength=1.0,
                        falloff="SMOOTH", mirror=1)
assert mirrored(ring), ("twisted the other way on the other side", points(ring))
top = points(ring)[0]
oanim.ops.sculpt.stroke(brush="GRAB", path=path(top, (top[0] + 0.3, top[1] + 0.3)), radius=0.3, strength=1.0,
                        falloff="SMOOTH", mirror=1)
p = points(ring)
assert mirrored(ring) and abs(p[0][0]) < 1e-5 and p[0][1] > top[1] + 0.2, ("grabbed at the middle: up, on the axis", p[0])

# A pose that isn't symmetric (a turned head) stays as it is: what mirrors is the change.
doc.frame_current = 20
oanim.ops.mesh.select_none()
before = points(ring)
oanim.ops.sculpt.stroke(brush="GRAB", path=path(before[2], (before[2][0] + 0.2, before[2][1])), radius=0.15,
                        strength=1.0, falloff="CONSTANT", mirror=0)
posed = points(ring)
assert not mirrored(ring), "posed one-sided"
oanim.ops.sculpt.stroke(brush="PUSH", path=path((-0.9, 0.3), (-1.1, 0.3), (-1.3, 0.35)), radius=0.4, strength=1.0,
                        falloff="SMOOTH", mirror=1)
after = points(ring)
d10 = (after[10][0] - posed[10][0], after[10][1] - posed[10][1])
d2 = (after[2][0] - posed[2][0], after[2][1] - posed[2][1])
assert abs(d10[0]) > 1e-3 and close(d2[0], -d10[0]) and close(d2[1], d10[1]), ("the change mirrors", d10, d2)
assert close(after[2][0] + after[10][0], posed[2][0] + posed[10][0]), "the pose's own lean is kept"
doc.frame_current = 0

# Into a partner: a layer named for a side (Blob_L) mirrors into the other (Blob_R).
oanim.ops.layer.new()
left = oanim.data.layers.active
left.name = "Blob_L"
left.add_curve([(1.0, 0.0), (2.0, 0.0), (3.0, 0.0)])
oanim.ops.layer.new()
right = oanim.data.layers.active
right.name = "Blob_R"
right.add_curve([(-1.0, 0.0), (-2.0, 0.0), (-3.0, 0.0)])
oanim.data.layers.active = left
for l in oanim.data.layers.all():
    l.select = l.id == left.id
oanim.ops.mesh.select_none()
oanim.ops.sculpt.stroke(brush="GRAB", path=path((2.0, 0.0), (2.0, 0.4)), radius=0.3, strength=1.0,
                        falloff="CONSTANT", mirror=1)
assert close(points(left)[1][1], 0.4), points(left)
assert close(points(right)[1][0], -2.0) and close(points(right)[1][1], 0.4), ("the partner follows", points(right))
before = right.radii
oanim.ops.sculpt.stroke(brush="THICKNESS", path=path((2.0, 0.4), (2.0, 0.4)), radius=0.3, strength=1.0,
                        falloff="CONSTANT", mirror=1)
assert abs(right.radii[1] - left.radii[1]) < 1e-5 and right.radii[1] > before[1], ("partner widths", right.radii)
# Off: nothing mirrors.
oanim.ops.sculpt.stroke(brush="GRAB", path=path((2.0, 0.4), (2.0, 0.0)), radius=0.3, strength=1.0,
                        falloff="CONSTANT", mirror=0)
assert close(points(left)[1][1], 0.0) and close(points(right)[1][1], 0.4), "mirror off"

print("sculpt: ok")
