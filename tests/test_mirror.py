"""Mirror (right-click on points, Blender's): transform.mirror flips the selected points along X or Y:
GLOBAL in the document's axes through their layer's origin, LOCAL in the active layer's own axes
through the selection's middle; handles flip with them; on a posed frame it keys them.

Run with:  OpenAnimate --background --python tests/test_mirror.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0
doc.auto_key = True


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


layer = layers.active
layer.add_curve([(0.0, 0.0), (1.0, 0.0), (3.0, 1.0)])
oanim.ops.mesh.select_none()


def where(i):
    return tuple(layer.points[i][:2])


assert oanim.ops.transform.mirror(axis="X") == {'CANCELLED'}, "nothing selected"
layer.select_points([0, 1, 2])
assert oanim.ops.transform.mirror(axis="Z") == {'CANCELLED'}
assert oanim.ops.transform.mirror(axis="X", orientation="SIDEWAYS") == {'CANCELLED'}

# X Global: left and right swap about the layer's origin (x 0); y stays.
assert oanim.ops.transform.mirror(axis="X", orientation="GLOBAL") == {'FINISHED'}
assert close(where(0), (0.0, 0.0)) and close(where(1), (-1.0, 0.0)) and close(where(2), (-3.0, 1.0)), layer.points
assert oanim.wm.undo_history()[0][-1] == "Mirror X Global", oanim.wm.undo_history()[0][-1]
# Y Global: up and down about it (y 0).
assert oanim.ops.transform.mirror(axis="Y") == {'FINISHED'}
assert close(where(1), (-1.0, 0.0)) and close(where(2), (-3.0, -1.0)), layer.points
oanim.ops.ed.undo()
oanim.ops.ed.undo()
assert close(where(0), (0.0, 0.0)) and close(where(2), (3.0, 1.0)), "one undo step each"
layer.select_points([0, 1, 2])

# A moved layer: about its own origin, not the document's.
layer.location_x = 2.0
assert oanim.ops.transform.mirror(axis="X") == {'FINISHED'}
assert close(where(1), (-1.0, 0.0)) and close(where(2), (-3.0, 1.0)), ("about the layer's origin", layer.points)
oanim.ops.ed.undo()
layer.location_x = 0.0
layer.select_points([0, 1, 2])

# Local: through the selection's middle (x 1.5).
assert oanim.ops.transform.mirror(axis="X", orientation="LOCAL") == {'FINISHED'}
assert close(where(0), (3.0, 0.0)) and close(where(1), (2.0, 0.0)) and close(where(2), (0.0, 1.0)), layer.points
oanim.ops.ed.undo()
layer.select_points([0, 1, 2])

# A turned layer: Local flips along its own x, Global along the document's x (its y, turned 90).
layer.rotation = 90.0
assert oanim.ops.transform.mirror(axis="X", orientation="LOCAL") == {'FINISHED'}
assert close(where(0), (3.0, 0.0)) and close(where(2), (0.0, 1.0)), ("local", layer.points)
oanim.ops.ed.undo()
# (Undo takes back the turn and selection too: neither was an undo step of its own.)
layer.rotation = 90.0
layer.select_points([0, 1, 2])
assert oanim.ops.transform.mirror(axis="X", orientation="GLOBAL") == {'FINISHED'}
assert close(where(1), (1.0, 0.0)) and close(where(2), (3.0, -1.0)), ("global", layer.points)
oanim.ops.ed.undo()
layer.rotation = 0.0

# Only the selection moves.
oanim.ops.mesh.select_none()
layer.select_points([0, 1])
assert oanim.ops.transform.mirror(axis="X") == {'FINISHED'}
assert close(where(1), (-1.0, 0.0)) and close(where(2), (3.0, 1.0)), layer.points
oanim.ops.ed.undo()

# Posed: it keys the points, and frame 0 keeps the setup.
doc.frame_current = 10
layer.select_points([0, 1, 2])
assert oanim.ops.transform.mirror(axis="X") == {'FINISHED'}
assert close(where(2), (-3.0, 1.0)), layer.points
doc.frame_current = 0
assert close(where(2), (3.0, 1.0)), "the setup is untouched"
doc.frame_current = 10
assert close(where(2), (-3.0, 1.0)), "keyed"

print("mirror: ok")
