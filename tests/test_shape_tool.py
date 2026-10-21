"""The Shape tool's boxes and circles, dragged out on Moho curves, and the Draw tool's modes.

Run with:  OpenAnimate --background --python tests/test_shape_tool.py
"""

import math

import oanim

layer = oanim.data.layers.active


def near(a, b, eps=1e-4):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def new_points(before):
    return [(x, y) for x, y, _ in layer.points[before:]]


# --- The tool and its flyout's choice -------------------------------------------------------
for shape in ("BOX", "CIRCLE"):
    assert oanim.ops.wm.tool_shape(shape=shape) == {'FINISHED'}
for other in ("CURVE", "LINE", "HEXAGON"):
    assert oanim.ops.wm.tool_shape(shape=other) == {'CANCELLED'}, f"{other} is not a shape"
# The pen is a mode of the Draw tool.
assert oanim.ops.wm.tool_draw(mode="PEN") == {'FINISHED'}
assert oanim.ops.wm.tool_draw(mode="FREEHAND") == {'FINISHED'}
assert oanim.ops.wm.tool_draw(mode="SPRAY") == {'CANCELLED'}
assert oanim.ops.wm.tool_pen() == {'FINISHED'}, "the old pen operator still works"

# --- Box: four peaked corners, closed, filled on request --------------------------------------
n = layer.point_count
assert layer.add_primitive("BOX", [(-1, -0.5), (1, 0.5)], fill=True)
assert new_points(n) == [(-1, -0.5), (1, -0.5), (1, 0.5), (-1, 0.5)]
assert all(sel for _, _, sel in layer.points[n:]) and not any(sel for _, _, sel in layer.points[:n]), \
    "the new shape's points become the selection"
outline, closed = layer.shape_outline(layer.shape_count - 1, steps=4)
assert closed and all(near(p, q) for p, q in zip(outline[::4], new_points(n))), "peaked corners keep straight sides"
assert all(abs(abs(x) - 1) < 1e-5 or abs(abs(y) - 0.5) < 1e-5 for x, y in outline), "sides don't bulge"
assert not layer.add_primitive("BOX", [(0, 0), (1, 0)]), "a flat box draws nothing"

# --- Circle and ellipse: four points whose curve stays on the ellipse --------------------------
for rx, ry in ((0.5, 0.5), (1.0, 0.4)):
    n = layer.point_count
    assert layer.add_primitive("CIRCLE", [(-rx, -ry), (rx, ry)])
    outline, closed = layer.shape_outline(layer.shape_count - 1, steps=32)
    assert closed and layer.point_count - n == 4
    worst = max(abs(math.hypot(x / rx, y / ry) - 1.0) for x, y in outline)
    assert worst < 0.003, f"ellipse {rx}x{ry} strays {worst:.4f} from the true ellipse"

# --- Curves are drawn with the pen, not as shapes --------------------------------------------------
try:
    layer.add_primitive("CURVE", [(0, 0), (1, 1)])
    raise AssertionError("curves are drawn with the pen")
except ValueError:
    pass

assert layer.shape_count == 3, "one box and two circles"
print("Shape tool tests passed")
