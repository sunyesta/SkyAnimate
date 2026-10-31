"""Merge (M): the selected points into one at their middle, or those within a distance of one another; curves run
through the kept point, a segment between two merged points goes, and shapes that met only there become one.

Run with:  OpenAnimate --background --python tests/test_merge.py
"""

import oanim

ops = oanim.ops
layers = oanim.data.layers


def close(a, b, eps=1e-5):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


# M on a drawing opens the Merge menu (on a bone layer Move to Bone Collection has the key).
strokes = new_layer("Strokes")
strokes.add_curve([(0.0, 0.0), (1.0, 0.0)])
strokes.add_curve([(1.2, 0.0), (2.0, 0.5)])
assert strokes.shape_count == 2
strokes.select_points([1])
assert oanim.wm.event_simulate("M", area="Viewport"), "M is taken"
ops.mesh.select_none()

# Two strokes' ends: one point at their middle, both curves through it, their two lines one shape.
strokes.select_points([1, 2])
assert ops.mesh.merge() == {'FINISHED'}
assert strokes.point_count == 3 and strokes.curve_count == 2, (strokes.point_count, strokes.curve_count)
assert any(close(p[:2], (1.1, 0.0)) for p in strokes.points), strokes.points
assert strokes.shape_count == 1, strokes.shapes
assert len(strokes.shapes[0][1]) == 2, ("the one shape has both strokes", strokes.shapes)
ops.ed.undo()
assert strokes.point_count == 4 and strokes.shape_count == 2, "undo brings both back"
ops.mesh.select_none()

# Two neighbours on one curve: the segment between them goes, the curve and its shape carry on across.
line = new_layer("Line")
line.add_curve([(0.0, 0.0), (1.0, 0.0), (1.05, 0.0), (2.0, 0.0)])
line.select_points([1, 2])
assert ops.mesh.merge() == {'FINISHED'}
assert line.point_count == 3 and line.curve_count == 1 and line.shape_count == 1
assert close(line.points[1][:2], (1.025, 0.0)), line.points
assert len(line.shapes[0][1]) == 1, ("one run of segments", line.shapes)
ops.mesh.select_none()

# Merge by Distance: only the points within the distance of one another (a closed square's stacked corner), the
# fill and its outline (both on all of them) staying as they are.
square = new_layer("Square")
square.add_curve([(0, 0), (1, 0), (1, 1), (1, 1.0005), (0, 1)], closed=True, fill=True)
shapes_before = square.shape_count
ops.mesh.select_all()
assert ops.mesh.merge_by_distance(distance="0.01") == {'FINISHED'}
assert square.point_count == 4, square.points
assert square.shape_count == shapes_before, square.shapes
assert any(close(p[:2], (1.0, 1.00025)) for p in square.points), square.points
assert square.shape_style(0)["has_fill"]
# Nothing that close: nothing changes.
before = square.point_count
assert ops.mesh.merge_by_distance(distance="0.01") == {'FINISHED'}
assert square.point_count == before
ops.mesh.select_none()

# One point alone can't merge.
square.select_points([0])
assert ops.mesh.merge() == {'CANCELLED'}
print("Merge tests passed")

# --- With X mirror editing each side merges by itself: a line's two ends don't meet --------------------------------------
# A mouth: two lines whose ends nearly touch, each corner's two points far closer together than the lookup's reach.
ops.wm.read_homefile(confirmed="1")
layers = oanim.data.layers
ops.layer.new()
lips = layers.active
lips.add_curve([(0.0431, 0.4249), (0.0, 0.413), (-0.0431, 0.4249)])
lips.add_curve([(0.0431, 0.4248), (0.0, 0.4157), (-0.0431, 0.4248)])
oanim.wm.set_mirror_editing(True, False)
lips.select_points([0, 3])
assert ops.mesh.merge() == {'FINISHED'}
assert oanim.wm.banner_report()[3] == "Removed 2 points", oanim.wm.banner_report()
assert lips.point_count == 4, lips.point_count
where = [tuple(p[:2]) for p in lips.points]
assert abs(where[0][0] - 0.0431) < 1e-4, ("the right corner stays on the right", where)
assert abs(where[2][0] + 0.0431) < 1e-4 or abs(where[3][0] + 0.0431) < 1e-4, ("and the left on the left", where)
assert sorted(p[2] for p in lips.points) == [False, False, False, True], "the images aren't left selected"
assert lips.curve_count == 2 and len({tuple(w) for w in lips.detail["wires"]}) == 2, lips.detail["wires"]

# Picked on both sides by hand: they meet, as asked.
ops.layer.new()
both = layers.active
both.add_curve([(1.0, 0.0), (0.0, 1.0), (-1.0, 0.0)])
both.select_points([0, 2])
assert ops.mesh.merge() == {'FINISHED'}
assert both.point_count == 2 and abs(both.points[0][0]) < 1e-4, [p[:2] for p in both.points]

# One point picked with its image nowhere to merge with: nothing to do on either side.
ops.layer.new()
one = layers.active
one.add_curve([(1.0, 0.0), (0.0, 1.0), (-1.0, 0.0)])
one.select_points([0])
assert ops.mesh.merge() == {'CANCELLED'}
assert one.point_count == 3 and [p[2] for p in one.points] == [True, False, False], "the selection is as it was"
oanim.wm.set_mirror_editing(False, False)
