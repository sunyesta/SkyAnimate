"""Move to Lower Level (right-click on detail points): mesh.detail_lower moves the selected detail points
one detail level down, keeping the curve's look; from level 1 they become base points. The finer
detail around them then rides them. Base points can't go lower; the setup frame only; one undo step.

Run with:  OpenAnimate --background --python tests/test_detail_lower.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
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


def levels():
    return layer.detail["levels"]


def level(n):
    ops.mesh.select_all()
    ops.mesh.detail_level_set(level=str(n))
    ops.mesh.select_none()


layer = layers.active
ring = [(math.cos(a * math.pi / 4) * (1.15 if a % 2 else 1.0), math.sin(a * math.pi / 4) * (1.15 if a % 2 else 1.0))
        for a in range(8)]
layer.add_curve(ring, closed=True, fill=True)
before = outline()
# Points 1 and 2 to level 1, then 1 on to level 2.
ops.mesh.select_none()
layer.select_points([1, 2])
assert ops.mesh.detail_raise() == {'FINISHED'}
level(1)
layer.select_points([1])
assert ops.mesh.detail_raise() == {'FINISHED'}
assert levels()[:3] == [0, 2, 1], levels()
level(2)

# A base point can't go lower.
layer.select_points([0])
assert ops.mesh.detail_lower() == {'CANCELLED'}

# Point 1 down to level 1: the look stays.
ops.mesh.select_none()
layer.select_points([1])
assert ops.mesh.detail_lower() == {'FINISHED'}
assert levels()[:3] == [0, 1, 1], levels()
assert same_outline(before, outline()), "the curve keeps its look"
assert oanim.wm.undo_history()[0][-1] == "Move to Lower Level", oanim.wm.undo_history()[0][-1]

# Point 2 down to level 0: a base point now, and point 1 (level 1) rides it.
ops.mesh.select_none()
layer.select_points([2])
assert ops.mesh.detail_lower() == {'FINISHED'}
assert levels()[:3] == [0, 1, 0], levels()
assert same_outline(before, outline())
level(0)
assert layer.detail["hidden"][1] and not layer.detail["hidden"][2], layer.detail
was1 = layer.points[1][:2]
layer.select_points([2])
assert ops.mesh.transform_selected(move_x="0.3") == {'FINISHED'}
assert abs(layer.points[1][0] - was1[0]) > 0.02, "carried along by its new parent"
ops.ed.undo()

# Both together, from level 1: base points, and nothing moves.
level(1)
layer.select_points([1])
assert ops.mesh.detail_lower() == {'FINISHED'}
assert levels()[:3] == [0, 0, 0], levels()
assert same_outline(before, outline())

# Undo takes it back a step at a time.
ops.ed.undo()
assert levels()[:3] == [0, 1, 0], levels()

# Not away from the setup frame.
doc.frame_current = 5
level(1)
layer.select_points([1])
assert ops.mesh.detail_lower() == {'CANCELLED'}
doc.frame_current = 0

print("detail_lower: ok")
