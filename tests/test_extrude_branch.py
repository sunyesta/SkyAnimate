"""Extrude (E) from any point: an open end grows its curve, as before; any other point (a box's corner, a point
inside a curve, a grid's crossing) starts a new open curve welded to it, as Moho's Add Point and Blender's Grease
Pencil extrude do. The new segment shows: in an outline-only shape through the point, else as a new outline shape
in the look of the shape there. A grid is one shape, drawn line by line (never a stroke folded back on itself
where lines cross).

Run with:  OpenAnimate --background --python tests/test_extrude_branch.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0
layer = layers.active


def close(a, b, eps=1e-5):
    return all(abs(x - y) < eps for x, y in zip(a, b))


# A grid is one shape: its border one line, each inner row and column one line.
assert layer.add_primitive("GRID", [(0.0, 0.0), (2.0, 2.0)], columns=2, rows=2)
assert layer.shape_count == 1 and layer.curve_count == 6
runs = layer.shape_runs(0)
assert sorted(len(r) for r in runs) == [2, 2, 8], runs

# From its crossing: a new curve welded there, part of the grid's shape, drawn as a line of its own.
nodes = layer.grids[0][3]
oanim.ops.mesh.select_none()
layer.select_points([nodes[4]])
added = layer.extrude_selected()
assert len(added) == 1 and layer.curve_count == 7 and layer.shape_count == 1
assert close(layer.points[added[0]][:2], (1.0, 1.0)), "at its point, for the move"
assert len(layer.shape_runs(0)) == 4 and [len(r) for r in layer.shape_runs(0)].count(1) == 1
assert layer.grids, "the grid stays a grid"
# Again from the new end: that curve grows.
added2 = layer.extrude_selected()
assert layer.curve_count == 7 and layer.point_count == 11, (layer.curve_count, layer.point_count)

# A filled box's corner: a new outline-only shape in its look (the fill untouched).
oanim.ops.layer.new()
box = layers.active
assert box.add_primitive("BOX", [(0.0, 0.0), (1.0, 1.0)], fill=True)
style = box.shape_style(0)
oanim.ops.mesh.select_none()
box.select_points([0])
assert len(box.extrude_selected()) == 1
assert box.curve_count == 2 and box.shape_count == 2
line = box.shape_style(1)
assert not line['has_fill'] and line['has_outline'] and line['line'] == style['line'], line
assert box.shape_style(0)['has_fill'], "the box keeps its fill"

# An outline-only box: its own shape takes the new segment.
oanim.ops.layer.new()
ring = layers.active
assert ring.add_primitive("CIRCLE", [(0.0, 0.0), (1.0, 1.0)], fill=False)
oanim.ops.mesh.select_none()
ring.select_points([1])
assert len(ring.extrude_selected()) == 1
assert ring.shape_count == 1 and ring.curve_count == 2
assert sorted(len(r) for r in ring.shape_runs(0)) == [1, 4], ring.shape_runs(0)

# An open end extruded leads on to the new point: its handle out, which had no length (nothing to lead to), now
# carries on the way the curve came in, as long as its handle in, so the curve doesn't turn there.
oanim.ops.layer.new()
arc = layers.active
arc.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
oanim.ops.mesh.select_none()
arc.select_points([2])
assert arc.extrude_selected() == [3]
arc.move_points((1.0, 0.0))
_, h_in, h_out = arc.curve_handles(0)[2]
d_in = (h_in[0] - 2.0, h_in[1] - 0.0)
d_out = (h_out[0] - 2.0, h_out[1] - 0.0)
assert close(d_out, (-d_in[0], -d_in[1])) and abs(d_out[0]) > 0.1, (d_in, d_out)
# From the curve's start (turned round to grow there) the same.
oanim.ops.mesh.select_none()
arc.select_points([0])
assert len(arc.extrude_selected()) == 1
arc.move_points((-1.0, 0.0))
handles = arc.curve_handles(0)
for kind, h_in, h_out in handles[1:-1]:
    assert not close(h_in, h_out), ("every inner point has handles of length", handles)

# As the new point moves, the segment's handles follow it (E's drag): the end it grew from goes on in line with its
# handle in, the new end arrives in one even bend, and both stay in line from then on.
import math


def unit(v):
    n = math.hypot(*v)
    return (v[0] / n, v[1] / n)


oanim.ops.layer.new()
bend = layers.active
bend.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
oanim.ops.mesh.select_none()
bend.select_points([2])
tip = bend.extrude_selected()[0]
bend.move_points((1.0, -0.5))
bend.extrude_align(tip)
kind, h_in, h_out = bend.curve_handles(0)[2]
d_in, d_out = (h_in[0] - 2.0, h_in[1]), (h_out[0] - 2.0, h_out[1])
assert kind == "ALIGNED" and close(unit(d_out), (-unit(d_in)[0], -unit(d_in)[1])), (d_in, d_out)
chord = math.hypot(1.0, 0.5)
reach = math.hypot(*d_out)
assert 0.0 < reach <= chord / 3 + 1e-5, "as long as the new segment asks, not as the old one was"
# The new end mirrors that across the chord, pointing back along the bend; nothing past it.
kind, end_in, end_out = bend.curve_handles(0)[3]
to_in = (end_in[0] - 3.0, end_in[1] + 0.5)
along = (1.0 / chord, -0.5 / chord)
out_way = unit(d_out)
arrive = (2 * (out_way[0] * along[0] + out_way[1] * along[1]) * along[0] - out_way[0],
          2 * (out_way[0] * along[0] + out_way[1] * along[1]) * along[1] - out_way[1])
assert kind == "ALIGNED" and close(unit(to_in), (-arrive[0], -arrive[1]), 1e-4), (to_in, arrive)
assert close(end_out, (3.0, -0.5)), "no handle past the end"
# It follows the point: moved further on, the handle grows with the segment and stays in line.
bend.move_points((1.0, 0.0))
bend.extrude_align(tip)
_, h_in, h_out = bend.curve_handles(0)[2]
d_out2 = (h_out[0] - 2.0, h_out[1])
assert math.hypot(*d_out2) > reach and close(unit(d_out2), out_way), (d_out2, reach)
# Turned straight back: no loop, the handle has no length.
bend.move_points((-4.0, 1.0))
bend.extrude_align(tip)
assert all(math.isfinite(c) for h in bend.curve_handles(0) for p in h[1:] for c in p)

# A straight run goes on straight along it, though its end had no handle in of any length.
oanim.ops.layer.new()
run = layers.active
run.add_curve([(0.0, 0.0), (1.0, 0.0)])
oanim.ops.mesh.select_none()
run.select_points([1])
next_tip = run.extrude_selected()[0]
run.move_points((1.0, 1.0))
run.extrude_align(next_tip)
_, _, h_out = run.curve_handles(0)[1]
assert h_out[0] > 1.05 and abs(h_out[1]) < 1e-5, h_out

# A corner stays one: the handle leaves toward the new point, not on past the corner.
oanim.ops.layer.new()
corner = layers.active
corner.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
oanim.ops.mesh.select_none()
corner.select_points([2])
assert oanim.ops.mesh.peak() == {'FINISHED'}
corner_tip = corner.extrude_selected()[0]
corner.move_points((0.0, 3.0))
corner.extrude_align(corner_tip)
kind, _, h_out = corner.curve_handles(0)[2]
assert kind == "FREE" and close(h_out, (2.0, 1.0)), (kind, h_out)

# A branch has no way it was going: its segment stays straight.
oanim.ops.layer.new()
fork = layers.active
fork.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
oanim.ops.mesh.select_none()
fork.select_points([1])
branch_tip = fork.extrude_selected()[0]
fork.move_points((0.0, 1.0))
before = fork.curve_handles(1)
fork.extrude_align(branch_tip)
assert fork.curve_handles(1) == before

print("extrude branch: ok")
