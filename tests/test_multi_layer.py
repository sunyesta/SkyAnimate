"""Editing points on several layers at once: the active layer plus the layers picked with Ctrl or
Shift click in the Layers editor.

Run with:  OpenAnimate --background --python tests/test_multi_layer.py
"""

import oanim

layers = oanim.data.layers
a = layers.active
a.add_curve([(-1.0, 0.0), (-0.5, 0.5), (0.0, 0.0)])
oanim.ops.layer.new()
b = layers.active
assert b != a
b.add_curve([(1.0, 0.0), (1.5, 0.5), (2.0, 0.0)])

# Only the active layer is edited until another is picked.
oanim.ops.mesh.select_all()
assert all(p[2] for p in b.points) and not any(p[2] for p in a.points)

a.select = True
oanim.ops.mesh.select_all()
assert all(p[2] for p in a.points), "select all reaches every picked layer"
oanim.ops.mesh.select_none()
assert not any(p[2] for p in a.points + b.points)
oanim.ops.mesh.select_invert()
assert all(p[2] for p in a.points + b.points)

# Delete removes the selected points on both layers.
oanim.ops.mesh.select_none()
a.select_points([1])
b.select_points([1])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert a.point_count == 2 and b.point_count == 2

# One undo step covers both layers.
oanim.ops.ed.undo()
assert a.point_count == 3 and b.point_count == 3

# A picked layer that is hidden or locked is left alone.
a.locked = True
oanim.ops.mesh.select_none()
oanim.ops.mesh.select_all()
assert not any(p[2] for p in a.points) and all(p[2] for p in b.points)
a.locked = False

# With only a picked layer having a selection, operators still run.
oanim.ops.mesh.select_none()
a.select_points([0, 1, 2])
assert oanim.ops.mesh.peak() == {'FINISHED'}
assert oanim.ops.mesh.duplicate_move() in ({'FINISHED'}, {'RUNNING_MODAL'})

# Unpicking goes back to the active layer only.
a.select = False
oanim.ops.mesh.select_none()
oanim.ops.mesh.select_all()
assert all(p[2] for p in b.points)

# Delete Layer removes the active layer and every layer picked with it.
def names():
    return [l.name for l in oanim.data.layers]


oanim.ops.layer.new()
c = layers.active
oanim.ops.layer.new()
d = layers.active
oanim.ops.layer.new()
keep = layers.active
layers.active = d
c.select = True
keep_name, c_name, d_name = keep.name, c.name, d.name
assert oanim.ops.layer.delete() == {'FINISHED'}
assert c_name not in names() and d_name not in names(), names()
assert keep_name in names() and layers.active.name in names(), "an unpicked layer stays, and becomes active"
# A picked layer inside a picked group goes with the group.
oanim.ops.layer.new()
inner = layers.active
assert oanim.ops.layer.group() == {'FINISHED'}
group = layers.active
group_name, inner_name = group.name, inner.name
inner.select = True
before = len(names())
assert oanim.ops.layer.delete() == {'FINISHED'}
assert group_name not in names() and len(names()) == before - 1, names()

print("Multi-layer tests passed")
