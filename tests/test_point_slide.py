"""Slide (G G): selected points slide along their curves toward a neighbour, and the curve keeps its
shape (the segments beside them are refit to the old line).

Run with:  OpenAnimate --background --python tests/test_point_slide.py
"""

import math

import oanim

layers = oanim.data.layers
doc = oanim.data.document
ops = oanim.ops
doc.frame_current = 0


def near_line(p, line):
    """How far p is from the polyline `line`."""
    best = 1e9
    for a, b in zip(line, line[1:]):
        ex, ey = b[0] - a[0], b[1] - a[1]
        l2 = ex * ex + ey * ey
        t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((p[0] - a[0]) * ex + (p[1] - a[1]) * ey) / l2))
        best = min(best, math.hypot(p[0] - a[0] - ex * t, p[1] - a[1] - ey * t))
    return best


def stays_on(layer, was, eps):
    now, _ = layer.shape_outline(0, steps=64)
    worst = max(near_line(p, was) for p in now)
    assert worst < eps, ("the curve keeps its shape", worst)
    return worst


def make_arc():
    """A smooth arc of 5 points round a unit circle, on a layer of its own."""
    ops.layer.new()
    layer = layers.active
    layer.add_curve([(math.cos(a), math.sin(a)) for a in (0.0, 0.4, 0.8, 1.2, 1.6)])
    layer.select_points(range(layer.point_count), False)
    return layer


arc = make_arc()
was, _ = arc.shape_outline(0, steps=64)
start = arc.points

# The middle point halfway toward the next one: it lands on the old line, the ends don't move.
arc.select_points(range(arc.point_count), False)
arc.select_points([2])
assert ops.mesh.slide(factor="0.5") == {'FINISHED'}
p = arc.points[2]
assert near_line(p, was) < 1e-3, p
assert abs(math.atan2(p[1], p[0]) - 1.0) < 0.05, ("halfway from 0.8 to 1.2 rad", math.atan2(p[1], p[0]))
assert arc.points[0][:2] == start[0][:2] and arc.points[4][:2] == start[4][:2]
stays_on(arc, was, 3e-3)
arc = make_arc()

# Toward the one before, and two points next to each other at once.
arc.select_points([1, 2])
assert ops.mesh.slide(factor="-0.7") == {'FINISHED'}
for i in (1, 2):
    assert near_line(arc.points[i], was) < 1e-3
stays_on(arc, was, 3e-3)
arc = make_arc()

# An open curve's end only slides inward.
arc.select_points(range(arc.point_count), False)
arc.select_points([0])
assert ops.mesh.slide(factor="-0.5") == {'CANCELLED'}
assert ops.mesh.slide(factor="0.5") == {'FINISHED'}
assert near_line(arc.points[0], was) < 1e-3 and arc.points[0][:2] != start[0][:2]
stays_on(arc, was, 3e-3)
arc = make_arc()

# A closed curve: point 0 of a ring of 6 slides back round the end, toward point 5.
ops.layer.new()
box = layers.active
box.add_curve([(math.cos(i * math.pi / 3), math.sin(i * math.pi / 3)) for i in range(6)], closed=True, fill=True)
box_was, _ = box.shape_outline(0, steps=64)
box.select_points(range(box.point_count), False)
box.select_points([0])
assert ops.mesh.slide(factor="-0.25") == {'FINISHED'}, "toward point 5, round the end"
assert box.points[0][1] < -0.1, box.points[0]
p = box.points[0]
assert near_line(p, box_was) < 1e-3, p
stays_on(box, box_was, 1.2e-2)

# Posed on frame 10: the slide is a key there; frame 0 keeps its shape.
layers.active = arc
doc.frame_current = 10
arc.select_points(range(arc.point_count), False)
arc.select_points([2])
assert ops.mesh.slide(factor="0.5") == {'FINISHED'}
assert abs(math.atan2(arc.points[2][1], arc.points[2][0]) - 1.0) < 0.05
doc.frame_current = 0
assert arc.points[2][:2] == start[2][:2], "the setup frame is unchanged"

print("point_slide: ok")
