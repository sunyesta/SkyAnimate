"""Editing points on several layers at once: the active layer plus the layers picked with Ctrl or
Shift click in the Layers editor.

Run with:  SkyAnimate --background --python tests/test_multi_layer.py
"""

import sky

layers = sky.data.layers
a = layers.active
a.add_curve([(-1.0, 0.0), (-0.5, 0.5), (0.0, 0.0)])
sky.ops.layer.new()
b = layers.active
assert b != a
b.add_curve([(1.0, 0.0), (1.5, 0.5), (2.0, 0.0)])

# Only the active layer is edited until another is picked.
sky.ops.mesh.select_all()
assert all(p[2] for p in b.points) and not any(p[2] for p in a.points)

a.select = True
sky.ops.mesh.select_all()
assert all(p[2] for p in a.points), "select all reaches every picked layer"
sky.ops.mesh.select_none()
assert not any(p[2] for p in a.points + b.points)
sky.ops.mesh.select_invert()
assert all(p[2] for p in a.points + b.points)

# Delete removes the selected points on both layers.
sky.ops.mesh.select_none()
a.select_points([1])
b.select_points([1])
assert sky.ops.mesh.delete() == {'FINISHED'}
assert a.point_count == 2 and b.point_count == 2

# One undo step covers both layers.
sky.ops.ed.undo()
assert a.point_count == 3 and b.point_count == 3

# A picked layer that is hidden or locked is left alone.
a.locked = True
sky.ops.mesh.select_none()
sky.ops.mesh.select_all()
assert not any(p[2] for p in a.points) and all(p[2] for p in b.points)
a.locked = False

# With only a picked layer having a selection, operators still run.
sky.ops.mesh.select_none()
a.select_points([0, 1, 2])
assert sky.ops.mesh.peak() == {'FINISHED'}
assert sky.ops.mesh.duplicate_move() in ({'FINISHED'}, {'RUNNING_MODAL'})

# Unpicking goes back to the active layer only.
a.select = False
sky.ops.mesh.select_none()
sky.ops.mesh.select_all()
assert all(p[2] for p in b.points)

print("Multi-layer tests passed")
