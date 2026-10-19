"""Curve handles: Blender's handle types (Automatic, Vector, Aligned, Free), dragging handles,
and saving them.

Run with:  SkyAnimate --background --python tests/test_handles.py
"""

import math
import pathlib
import tempfile

import sky


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1])


def length(v):
    return math.hypot(v[0], v[1])


def collinear_opposite(a, b):
    """True when vectors a and b point exactly away from each other."""
    cross = a[0] * b[1] - a[1] * b[0]
    dot = a[0] * b[0] + a[1] * b[1]
    return abs(cross) < 1e-4 and dot < 0


layer = sky.data.layers.active
pts = [(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)]
curve = layer.add_curve(pts)
handles = layer.curve_handles(curve)
assert [h[0] for h in handles] == ["AUTO", "AUTO", "AUTO"], "points start Automatic"
# An Automatic middle point's handles are smooth: in a line through the point.
t, h_in, h_out = handles[1]
assert collinear_opposite(sub(h_in, pts[1]), sub(h_out, pts[1]))

# --- Set Handle Type --------------------------------------------------------------------------
sky.ops.mesh.select_none()
assert sky.ops.mesh.handle_type_set(type="VECTOR") == {'CANCELLED'}, "nothing selected"
layer.select_points([1])
assert sky.ops.mesh.handle_type_set(type="SIDEWAYS") == {'CANCELLED'}
assert sky.ops.mesh.handle_type_set(type="VECTOR") == {'FINISHED'}
t, h_in, h_out = layer.curve_handles(curve)[1]
assert t == "VECTOR"
assert close(h_in, (1.0 - 1 / 3, 0.5 - 0.5 / 3)) and close(h_out, (1.0 + 1 / 3, 0.5 - 0.5 / 3)), \
    "Vector handles point a third of the way to the neighbours"
assert [h[0] for h in layer.curve_handles(curve)] == ["AUTO", "VECTOR", "AUTO"], "only selected points change"

# Vector -> Aligned keeps the handles where they were but straightens them into a line.
assert sky.ops.mesh.handle_type_set(type="ALIGNED") == {'FINISHED'}
t, a_in, a_out = layer.curve_handles(curve)[1]
assert t == "ALIGNED" and close(a_out, h_out), "the out handle stays"
assert collinear_opposite(sub(a_in, pts[1]), sub(a_out, pts[1]))
assert abs(length(sub(a_in, pts[1])) - length(sub(h_in, pts[1]))) < 1e-4, "the in handle keeps its length"

sky.ops.ed.undo()
assert layer.curve_handles(curve)[1][0] == "VECTOR"
sky.ops.ed.redo()
assert layer.curve_handles(curve)[1][0] == "ALIGNED"

# Peak and Smooth are Vector and Automatic.
assert sky.ops.mesh.peak() == {'FINISHED'} and layer.curve_handles(curve)[1][0] == "VECTOR"
assert sky.ops.mesh.smooth() == {'FINISHED'} and layer.curve_handles(curve)[1][0] == "AUTO"

# --- Dragging handles -------------------------------------------------------------------------
# An Automatic handle dragged becomes Aligned: the other handle turns to stay in line and keeps
# its length.
_, old_in, old_out = layer.curve_handles(curve)[1]
layer.move_handle(curve, 1, "OUT", (1.5, 1.0))
t, n_in, n_out = layer.curve_handles(curve)[1]
assert t == "ALIGNED" and close(n_out, (1.5, 1.0))
assert collinear_opposite(sub(n_in, pts[1]), sub(n_out, pts[1]))
assert abs(length(sub(n_in, pts[1])) - length(sub(old_in, pts[1]))) < 1e-4

# Free handles move on their own.
sky.ops.mesh.handle_type_set(type="FREE")
layer.move_handle(curve, 1, "IN", (0.8, 1.2))
t, f_in, f_out = layer.curve_handles(curve)[1]
assert t == "FREE" and close(f_in, (0.8, 1.2)) and close(f_out, n_out), "the other Free handle stays put"

# A Vector handle dragged becomes Free; its partner stays where the Vector put it.
sky.ops.mesh.handle_type_set(type="VECTOR")
_, v_in, _ = layer.curve_handles(curve)[1]
layer.move_handle(curve, 1, "OUT", (1.2, 0.9))
t, g_in, g_out = layer.curve_handles(curve)[1]
assert t == "FREE" and close(g_in, v_in) and close(g_out, (1.2, 0.9))

# The handles shape the curve: the outline passes near the dragged handle side.
outline, _ = layer.shape_outline(0, 32)
assert max(p[1] for p in outline) > 0.5, "pulling the out handle up bends the curve above the point"

# --- Box corners --------------------------------------------------------------------------------
assert layer.add_primitive("BOX", [(3.0, 0.0), (4.0, 1.0)])
box_curve = layer.curve_count - 1
assert [h[0] for h in layer.curve_handles(box_curve)] == ["VECTOR"] * 4, "box corners have Vector handles"
outline, closed = layer.shape_outline(layer.shape_count - 1, 8)
assert closed
for x, y in outline:
    on_side = min(abs(x - 3.0), abs(x - 4.0), abs(y - 0.0), abs(y - 1.0)) < 1e-4
    assert on_side, f"box sides stay straight ({x}, {y})"

# --- Saved with the document ------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_handles_")) / "handles.sky")
saved = layer.curve_handles(curve)
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = sky.data.layers.active
loaded = layer.curve_handles(curve)
assert [h[0] for h in loaded] == [h[0] for h in saved]
for a, b in zip(loaded, saved):
    assert close(a[1], b[1]) and close(a[2], b[2]), (a, b)
assert [h[0] for h in layer.curve_handles(box_curve)] == ["VECTOR"] * 4

print("Handle tests passed")
