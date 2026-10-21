"""The Transform Points tool: a box gizmo around the selected points, as Transform Bones has for
bones; its header's fields run mesh.transform_selected (move, scale and turn about a pivot, in the
active layer's space), on any frame.

Run with:  OpenAnimate --background --python tests/test_transform_points.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0
doc.auto_key = True


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


assert oanim.ops.wm.tool_transform_points() == {'FINISHED'}
layer = layers.active
layer.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
oanim.ops.mesh.select_none()


def where(i):
    return tuple(layer.points[i][:2])


assert oanim.ops.mesh.transform_selected(move_x="1") == {'CANCELLED'}, "nothing selected"
layer.select_points([0, 1])
# Move.
assert oanim.ops.mesh.transform_selected(move_x="1", move_y="0.5") == {'FINISHED'}
assert close(where(0), (1.0, 0.5)) and close(where(1), (2.0, 0.5)) and close(where(2), (2.0, 0.0))
# Scale about the middle of the selection (the default pivot).
assert oanim.ops.mesh.transform_selected(scale_x="2") == {'FINISHED'}
assert close(where(0), (0.5, 0.5)) and close(where(1), (2.5, 0.5)), (where(0), where(1))
# Turn a quarter about a given pivot.
assert oanim.ops.mesh.transform_selected(pivot_x="0.5", pivot_y="0.5", angle="90") == {'FINISHED'}
assert close(where(0), (0.5, 0.5)) and close(where(1), (0.5, 2.5)), where(1)
# Flip across the pivot.
assert oanim.ops.mesh.transform_selected(pivot_x="0.5", pivot_y="0.5", scale_x="-1") == {'FINISHED'}
assert close(where(1), (0.5, 2.5)), "on the pivot's vertical line: stays"
# One undo step each.
oanim.ops.ed.undo()
oanim.ops.ed.undo()
assert close(where(0), (0.5, 0.5)) and close(where(1), (2.5, 0.5))

# In the active layer's space: a turned layer moves its points along its own x.
layer.rotation = 90.0
assert oanim.ops.mesh.transform_selected(move_x="1") == {'FINISHED'}
assert close(where(0), (1.5, 0.5)), ("along the layer's x", where(0))
layer.rotation = 0.0

# Posed: it keys the points.
doc.frame_current = 10
assert oanim.ops.mesh.transform_selected(move_y="1") == {'FINISHED'}
doc.frame_current = 11
assert [k["frame"] for k in layer.keyframes("points[0].pos")] == [0, 10]
doc.frame_current = 0
assert close(where(0), (1.5, 0.5)), "the setup stays"

print("Transform Points tests passed")
