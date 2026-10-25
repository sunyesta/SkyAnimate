"""The Transform Points and Transform Bones tools' Distort, Warp and edge shear: mesh.cage_transform and
armature.cage_transform bend the selection by a cage, a box split into cells each way whose corners are moved
(an even grid changes nothing; one cell is a four-corner distort; each node is on the bent surface).

Run with:  OpenAnimate --background --python tests/test_cage_transform.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def nodes(points):
    return " ".join("%r %r" % p for p in points)


def even(cells, lo=(0.0, 0.0), hi=(2.0, 2.0)):
    return [(lo[0] + (hi[0] - lo[0]) * i / cells, lo[1] + (hi[1] - lo[1]) * j / cells)
            for j in range(cells + 1) for i in range(cells + 1)]


layer = layers.active
layer.add_curve([(0.0, 0.0), (2.0, 0.0), (2.0, 2.0), (0.0, 2.0), (1.0, 1.0), (0.5, 1.5)])
layer.select_points(list(range(6)))
before = [tuple(p[:2]) for p in layer.points]
box = dict(lo_x="0", lo_y="0", hi_x="2", hi_y="2")

# An even grid changes nothing, at any size.
for cells in (1, 3):
    assert oanim.ops.mesh.cage_transform(cells=str(cells), nodes=nodes(even(cells)), **box) == {'FINISHED'}
    assert all(close(tuple(p[:2]), b) for p, b in zip(layer.points, before)), (cells, layer.points)

# A shear as a one-cell cage: the top slid right by 1 (x + y/2), exactly.
sheared = [(0.0, 0.0), (2.0, 0.0), (1.0, 2.0), (3.0, 2.0)]
assert oanim.ops.mesh.cage_transform(cells="1", nodes=nodes(sheared), name="Shear Points", **box) == {'FINISHED'}
for p, (x, y) in zip(layer.points, before):
    assert close(p[:2], (x + y * 0.5, y)), (p, x, y)
oanim.ops.ed.undo()
assert all(close(tuple(p[:2]), b) for p, b in zip(layer.points, before)), "one undo step"

# Distort: one corner moved, the others stay; the middle goes a quarter of the way (bilinear).
distort = [(0.0, 0.0), (2.0, 0.0), (0.0, 2.0), (3.0, 3.0)]
assert oanim.ops.mesh.cage_transform(cells="1", nodes=nodes(distort), **box) == {'FINISHED'}
assert close(layer.points[2][:2], (3.0, 3.0)) and close(layer.points[0][:2], (0.0, 0.0))
assert close(layer.points[4][:2], (1.25, 1.25)), layer.points[4]
oanim.ops.ed.undo()

# Warp: a 2x2 grid with its middle node pulled; a point on that node goes exactly there, corners stay.
grid = even(2)
grid[4] = (1.3, 0.8)
assert oanim.ops.mesh.cage_transform(cells="2", nodes=nodes(grid), **box) == {'FINISHED'}
assert close(layer.points[4][:2], (1.3, 0.8)), layer.points[4]
assert close(layer.points[0][:2], (0.0, 0.0)) and close(layer.points[2][:2], (2.0, 2.0))
moved = layer.points[5][:2]
assert 0.5 < moved[0] < 0.8 and 1.2 < moved[1] < 1.5, ("pulled along a little", moved)
oanim.ops.ed.undo()

# A turned box (box_angle 90): its x is the layer's y, so a cage slid along its x moves the points up.
turned = [(1.0, -2.0), (3.0, -2.0), (1.0, 0.0), (3.0, 0.0)]
assert oanim.ops.mesh.cage_transform(cells="1", nodes=nodes(turned), box_angle="90", lo_x="0", lo_y="-2", hi_x="2",
                                     hi_y="0") == {'FINISHED'}
for p, (x, y) in zip(layer.points, before):
    assert close(p[:2], (x, y + 1.0)), (p, x, y)
oanim.ops.ed.undo()

# Bad arguments are refused.
assert oanim.ops.mesh.cage_transform(cells="2", nodes="1 2 3", **box) == {'CANCELLED'}

# Bones: their bases and tips go through the cage.
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
oanim.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="2", tail_y="0", parent="")
oanim.ops.armature.select_all()
assert oanim.ops.armature.cage_transform(cells="1", nodes=nodes(sheared), name="Shear Bones",
                                         lo_x="0", lo_y="-1", hi_x="2", hi_y="1") == {'FINISHED'}
bone = rig.bones[0]
# The box (0, -1)..(2, 1) onto the sheared nodes: x + (y + 1) / 2, y + 1.
assert close(bone["head"], (0.5, 1.0)) and close(bone["tail"], (2.5, 1.0)), (bone["head"], bone["tail"])
# The tools' box mode, from Python.
assert oanim.wm.gizmo_mode() == ('TRANSFORM', 3)
assert oanim.wm.gizmo_mode('warp', 4) == ('WARP', 4)
assert oanim.wm.gizmo_mode(cells=20) == ('WARP', 8), "at most 8 each way"
oanim.wm.gizmo_mode('TRANSFORM')
print("cage transform: ok")
