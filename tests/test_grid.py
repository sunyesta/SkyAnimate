"""Grids: the Shape tool's Grid makes ordinary points and curves in a vector layer (a line along each row and up
each column, sharing the nodes) with a note of which points are its nodes. Each cell bends what is in it as the
Coons patch of the four curve segments round it: an unchanged grid changes nothing, a point on a grid line
follows that line, and past the grid's edge places go on straight. Saved with the file; a grid losing a node is
ordinary drawing from then on.

Run with:  OpenAnimate --background --python tests/test_grid.py
"""

import os
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0
layer = layers.active


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


# A 2 x 3 grid over (0, 0)..(2, 3): 3 x 4 nodes, 4 row lines and 3 column lines.
assert layer.add_primitive("GRID", [(0.0, 0.0), (2.0, 3.0)], columns=2, rows=3)
(name, columns, rows, nodes), = layer.grids
assert (name, columns, rows, len(nodes)) == ("Grid", 2, 3, 12), layer.grids
assert layer.point_count == 12 and layer.curve_count == 7, (layer.point_count, layer.curve_count)
assert close(layer.points[nodes[4]][:2], (1.0, 1.0)), "row by row from the bottom"
doc.frame_current = 1
doc.frame_current = 0

# Unchanged: every place stays, inside or out.
for p in [(0.3, 0.4), (1.7, 2.9), (1.0, 1.5), (-0.5, 1.0), (2.5, 3.5)]:
    assert close(layer.grid_map(p), p), (p, layer.grid_map(p))

# Posed on frame 10: the middle node (1, 1) moved right by 0.5. A place on it goes with it; one on the row
# line between it and (0, 1) goes half as far; a far cell's corner stays.
doc.auto_key = True
doc.frame_current = 10
oanim.ops.mesh.select_none()
layer.select_points([nodes[4]])
assert oanim.ops.mesh.translate(move_x="0.5") == {'FINISHED'}
assert close(layer.grid_map((1.0, 1.0)), (1.5, 1.0))
assert close(layer.grid_map((0.5, 1.0)), (0.75, 1.0)), layer.grid_map((0.5, 1.0))
assert close(layer.grid_map((2.0, 3.0)), (2.0, 3.0))
assert close(layer.grid_map((1.0, 3.0)), (1.0, 3.0)), "the top row stays"
inside = layer.grid_map((0.5, 0.5))
assert 0.5 < inside[0] < 0.75 and abs(inside[1] - 0.5) < 0.05, inside
# Outside: on straight from the edge.
outside = layer.grid_map((-0.5, 0.5))
assert outside[0] < 0.0, outside

# Saved and opened again, the grid comes back.
doc.frame_current = 0
path = os.path.join(tempfile.mkdtemp(), "grid.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = layers.active
assert [(g[0], g[1], g[2]) for g in layer.grids] == [("Grid", 2, 3)], layer.grids
doc.frame_current = 10
assert close(layer.grid_map((1.0, 1.0)), (1.5, 1.0))

# A node deleted: no grid any more, the rest of the drawing stays.
doc.frame_current = 0
nodes = layer.grids[0][3]
oanim.ops.mesh.select_none()
layer.select_points([nodes[0]])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert layer.grids == [] and layer.point_count == 11, (layer.grids, layer.point_count)

# A node dissolved, as deleted: no grid any more (none left naming a point that is gone), and saving and opening the
# file gives back just that.
oanim.ops.layer.new()
diss = layers.active
assert diss.add_primitive("GRID", [(0.0, 0.0), (3.0, 3.0)], columns=3, rows=3)
nodes = diss.grids[0][3]
oanim.ops.mesh.select_none()
diss.select_points([nodes[0], nodes[2], nodes[5], nodes[13]])
assert oanim.ops.mesh.dissolve_points() == {'FINISHED'}
assert diss.grids == [], diss.grids
path = os.path.join(tempfile.mkdtemp(), "dissolved.oanim")
assert oanim.ops.wm.save_copy(filepath=path) == {'FINISHED'}
kept = (diss.id, diss.point_count, diss.curve_count)
assert oanim.ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
diss = next(l for l in layers.all() if l.id == kept[0])
assert diss.grids == [] and (diss.id, diss.point_count, diss.curve_count) == kept, (diss.grids, diss.point_count)
layers.active = diss
layer = diss

# The Shape tool's Grid, its cells set by the operator.
assert oanim.ops.wm.tool_shape(shape="GRID", columns="4", rows="2") == {'FINISHED'}

# What its drag makes: Add Grid, which the ViewportActionPanel runs again with other options (one undo step,
# made again from before), as Blender's Add Mesh panel does.
oanim.ops.layer.new()
layer = layers.active
assert oanim.ops.mesh.primitive_grid_add(x="1", y="1", width="2", height="2", columns="2", rows="2") == {'FINISHED'}
assert [(g[1], g[2]) for g in layer.grids] == [(2, 2)] and layer.point_count == 9
assert oanim.wm.last_operation()[0] == "mesh.primitive_grid_add", oanim.wm.last_operation()
assert oanim.wm.redo_last(columns="5", width="4") == 'FINISHED'
assert [(g[1], g[2]) for g in layer.grids] == [(5, 2)] and layer.point_count == 18, (layer.grids, layer.point_count)
xs = [p[0] for p in layer.points]
assert abs(min(xs) + 1.0) < 1e-5 and abs(max(xs) - 3.0) < 1e-5, (min(xs), max(xs))
# The next grid starts with those cells.
assert oanim.ops.mesh.primitive_grid_add(x="0", y="0", width="1", height="1") == {'FINISHED'}
assert [(g[1], g[2]) for g in layer.grids] == [(5, 2), (5, 2)]
# A box or circle the same way, Fill in the panel.
assert oanim.ops.mesh.primitive_box_add(x="0", y="0", width="1", height="1", fill="1") == {'FINISHED'}
assert oanim.wm.redo_last(fill="0") == 'FINISHED'
assert not layer.shape_style(layer.shape_count - 1)['has_fill']
print("grid: ok")
