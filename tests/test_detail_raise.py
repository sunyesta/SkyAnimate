"""Move to Higher Level (right-click on points): mesh.detail_raise moves the selected points one detail
level up, keeping the curve's look; they then hide below their level and ride the coarser points
around them. An open curve's ends can't go up; the setup frame only; one undo step.

Run with:  OpenAnimate --background --python tests/test_detail_raise.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0


def to_polyline(p, line):
    best = 1e9
    for (ax, ay), (bx, by) in zip(line, line[1:] + line[:1]):
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / max(dx * dx + dy * dy, 1e-12)))
        best = min(best, ((p[0] - ax - dx * t) ** 2 + (p[1] - ay - dy * t) ** 2) ** 0.5)
    return best


def same_outline(a, b, eps=3e-3):
    return all(to_polyline(p, b) < eps for p in a) and all(to_polyline(p, a) < eps for p in b)


def outline():
    return layer.shape_outline(0, 32)[0]


layer = layers.active
ring = [(math.cos(a * math.pi / 4), math.sin(a * math.pi / 4)) for a in range(8)]
layer.add_curve(ring, closed=True, fill=True)
oanim.ops.mesh.select_none()

assert oanim.ops.mesh.detail_raise() == {'CANCELLED'}, "nothing selected"
before = outline()
layer.select_points([1])
assert oanim.ops.mesh.detail_raise() == {'FINISHED'}
assert layer.detail["levels"][1] == 1, layer.detail
assert same_outline(before, outline()), "the curve keeps its look"
assert layer.detail["hidden"][1], "hidden at the shape's level (0)"
assert oanim.wm.undo_history()[0][-1] == "Move to Higher Level", oanim.wm.undo_history()[0][-1]

# It rides the coarser points around it: moving them moves it.
was = layer.points[1][:2]
layer.select_points([0, 2, 3, 4, 5, 6, 7])
assert oanim.ops.mesh.transform_selected(move_x="1") == {'FINISHED'}
assert abs(layer.points[1][0] - (was[0] + 1.0)) < 1e-3, ("carried along", layer.points[1], was)
oanim.ops.ed.undo()

# Up again: level 2 (shown once the shape is edited at 1).
oanim.ops.mesh.select_all()
assert oanim.ops.mesh.detail_level_set(level="1") == {'FINISHED'}
oanim.ops.mesh.select_none()
layer.select_points([1])
assert oanim.ops.mesh.detail_raise() == {'FINISHED'}
assert layer.detail["levels"][1] == 2, layer.detail
assert same_outline(before, outline())

# Undo takes it back a step at a time.
oanim.ops.ed.undo()
assert layer.detail["levels"][1] == 1, layer.detail

# Not away from the setup frame.
doc.frame_current = 5
layer.select_points([3])
assert oanim.ops.mesh.detail_raise() == {'CANCELLED'}
doc.frame_current = 0

# An open curve's ends stay base points; its middle can go up.
oanim.ops.layer.new()
layer = layers.active
layer.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
oanim.ops.mesh.select_none()
layer.select_points([0])
assert oanim.ops.mesh.detail_raise() == {'CANCELLED'}, "an end"
layer.select_points([0, 1])
assert oanim.ops.mesh.detail_raise() == {'FINISHED'}
assert layer.detail["levels"] == [0, 1, 0], layer.detail

print("detail_raise: ok")
