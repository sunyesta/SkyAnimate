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

# Part of a shape: a selected point's handle leading to a point left out keeps its length (it only rides along with
# its point), so the part not picked keeps its shape. A circle's top, right and bottom points scaled twice as wide
# from the left of their box: the left half stays round, exactly as it was.
import math
doc.frame_current = 0
oanim.ops.layer.new()
ring = oanim.data.layers.active
ring.add_primitive("CIRCLE", [(-1.0, -1.0), (1.0, 1.0)], fill=True)


def ring_handles():
    return [(tuple(round(v, 4) for v in i), tuple(round(v, 4) for v in o)) for _, i, o in ring.curve_handles(0)]


def left_half():
    pts, _ = ring.shape_outline(0, 16)
    return sorted((round(x, 4), round(y, 4)) for x, y in pts if x < -1e-3)


left_before, handles_before = left_half(), ring_handles()
places = [(round(x, 4), round(y, 4)) for x, y, _ in ring.points]
left = places.index((-1.0, 0.0))
top, right_i = places.index((0.0, 1.0)), places.index((1.0, 0.0))
oanim.ops.mesh.select_none()
ring.select_points([i for i in range(len(places)) if i != left])
assert oanim.ops.mesh.transform_selected(scale_x="2", scale_y="1", pivot_x="0", pivot_y="0") == {'FINISHED'}
assert left_half() == left_before, "the left half keeps its shape"
assert ring_handles()[left] == handles_before[left]
h_in, h_out = ring_handles()[top]
assert abs(h_in[0] - 1.1046) < 1e-3 and abs(h_out[0] + 0.5523) < 1e-3, ("toward the right: stretched; left: kept", h_in, h_out)
oanim.ops.ed.undo()
# Turned, a smooth point's handle toward the part left out keeps its length and stays in line: no corner.
oanim.ops.mesh.select_none()
ring.select_points([i for i in range(len(places)) if i != left])
assert oanim.ops.mesh.transform_selected(angle="30", pivot_x="0", pivot_y="0") == {'FINISHED'}
(ix, iy), (ox, oy) = ring_handles()[top]
px, py = ring.points[top][:2]
a, b = (ix - px, iy - py), (ox - px, oy - py)
assert abs(math.hypot(*b) - 0.5523) < 1e-3, ("its length kept", b)
assert abs(a[0] * b[1] - a[1] * b[0]) < 1e-4 and a[0] * b[0] + a[1] * b[1] < 0, ("in line", a, b)
oanim.ops.ed.undo()
# Everything picked: every handle scales, as before.
oanim.ops.mesh.select_all()
assert oanim.ops.mesh.transform_selected(scale_x="2", scale_y="1", pivot_x="0", pivot_y="0") == {'FINISHED'}
assert abs(ring_handles()[top][1][0] + 1.1046) < 1e-3, ring_handles()[top]
oanim.ops.ed.undo()

print("Transform Points tests passed")
