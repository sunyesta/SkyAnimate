"""Subdivide (right-click on points, Blender's): each segment between two selected points is cut into
number_cuts + 1 pieces, keeping the curve's shape exactly; the cuts are spaced evenly along the segment's length;
the new points join the selection; at a shape's detail level they are detail of that level.

Run with:  OpenAnimate --background --python tests/test_subdivide_points.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm
doc.frame_current = 0


def outline(layer, steps=64):
    return layer.shape_outline(0, steps)[0]


def off_by(a, b):
    """How far the points of outline `a` stray from outline `b` (a closed polyline)."""
    worst = 0.0
    for p in a:
        best = 1e9
        for i in range(len(b)):
            q, r = b[i], b[(i + 1) % len(b)]
            dx, dy = r[0] - q[0], r[1] - q[1]
            t = max(0.0, min(1.0, ((p[0] - q[0]) * dx + (p[1] - q[1]) * dy) / max(dx * dx + dy * dy, 1e-12)))
            best = min(best, math.hypot(p[0] - q[0] - t * dx, p[1] - q[1] - t * dy))
        worst = max(worst, best)
    return worst


# --- A square: one side cut in the middle ---------------------------------------------------------------------
ops.layer.new()
box = layers.active
box.add_primitive("BOX", [(0, 0), (2, 2)], fill=True)  # straight sides
before = outline(box)
ops.mesh.select_none()
box.select_points([0, 1])
assert ops.mesh.subdivide() == {'FINISHED'}
assert box.point_count == 5 and box.curve_count == 1 and box.shape_count == 1
assert box.detail["wires"][0] == [0, 4, 1, 2, 3], box.detail["wires"]
assert all(abs(a - b) < 1e-5 for a, b in zip(box.points[4][:2], (1.0, 0.0))), box.points[4]
assert [p[2] for p in box.points] == [True, True, False, False, True], "the new point joins the selection"
assert off_by(outline(box), before) < 1e-4 and box.shape_outline(0, 4)[1], "the shape as it was, still closed"
# The ViewportActionPanel offers Number of Cuts to change afterwards; three cuts, evenly along it.
last = wm.last_operation()
assert last and last[0] == "mesh.subdivide" and last[1]["number_cuts"] == "1", last
ops.ed.undo()
ops.mesh.select_none()
box.select_points([0, 1])
assert ops.mesh.subdivide(number_cuts="3") == {'FINISHED'}
assert box.point_count == 7
xs = sorted(round(x, 4) for x, y, _ in box.points if abs(y) < 1e-6)
assert xs == [0.0, 0.5, 1.0, 1.5, 2.0], xs
ops.ed.undo()
assert box.point_count == 4

# The segment closing the loop (last point to first) too, and one point alone or two apart cut nothing.
ops.mesh.select_none()
box.select_points([3, 0])
assert ops.mesh.subdivide() == {'FINISHED'} and box.point_count == 5
assert all(abs(a - b) < 1e-5 for a, b in zip(box.points[4][:2], (0.0, 1.0))), box.points[4]
ops.ed.undo()
ops.mesh.select_none()
box.select_points([0, 2])
assert ops.mesh.subdivide() == {'CANCELLED'}, "not neighbours"
assert box.point_count == 4

# --- A circle, all of it, two cuts a segment: the shape exactly, the cuts even along the arcs ---------------------
ops.layer.new()
ring = layers.active
assert ring.add_primitive("CIRCLE", [(-1.0, -1.0), (1.0, 1.0)], fill=True)
before = outline(ring)
ops.mesh.select_all()
assert ops.mesh.subdivide(number_cuts="2") == {'FINISHED'}
assert ring.point_count == 12 and all(p[2] for p in ring.points)
assert off_by(outline(ring), before) < 1e-4, off_by(outline(ring), before)
wire = ring.detail["wires"][0]
pts = [ring.points[i][:2] for i in wire]
gaps = [math.hypot(pts[i][0] - pts[(i + 1) % 12][0], pts[i][1] - pts[(i + 1) % 12][1]) for i in range(12)]
assert max(gaps) - min(gaps) < 0.01 * max(gaps), ("evenly spaced", gaps)

# --- Uneven handles: even along the length, not the parameter ---------------------------------------------------
ops.layer.new()
lop = layers.active
lop.add_curve([(0, 0), (3, 0)])
ops.mesh.select_none()
lop.select_handle(0, 0, "OUT")
lop.move_points((2.0, 0.0))  # the first point's handle reaches far along the line
ops.mesh.select_none()
lop.select_points([0, 1])
assert ops.mesh.subdivide(number_cuts="2") == {'FINISHED'}
xs = sorted(round(p[0], 2) for p in lop.points)
assert xs == [0.0, 1.0, 2.0, 3.0], xs

# --- At a shape's detail level the cuts are detail of that level -------------------------------------------------
layers.active = box
ops.mesh.select_all()
assert ops.mesh.detail_level_set(level="1") == {'FINISHED'}
ops.mesh.select_none()
box.select_points([1, 2])
assert ops.mesh.subdivide() == {'FINISHED'}
assert box.detail["levels"][-1] == 1, box.detail["levels"]

# --- Mirror editing: the mirror image's segment is cut too -----------------------------------------------------
ops.layer.new()
pair = layers.active
pair.add_primitive("BOX", [(0.5, 0.0), (1.5, 1.0)], fill=True)
pair.add_primitive("BOX", [(-1.5, 0.0), (-0.5, 1.0)], fill=True)
wm.set_mirror_editing(x=True)
ops.mesh.select_none()
pair.select_points([0, 1])
assert ops.mesh.subdivide() == {'FINISHED'} and pair.point_count == 10
assert sorted(round(p[0], 4) for p in pair.points[8:]) == [-1.0, 1.0], pair.points[8:]
wm.set_mirror_editing(x=False)

print("subdivide points: ok")
