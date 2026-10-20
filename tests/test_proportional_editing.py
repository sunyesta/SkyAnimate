"""Proportional editing (Blender's, O): moving points drags the points around them too, by a
falloff out to a size; Connected Only measures along the curves. Points only: O does nothing
without editable points.

Run with:  SkyAnimate --background --python tests/test_proportional_editing.py
"""

import _sky
import sky


def close(a, b, eps=1e-4):
    return abs(a - b) < eps


layer = sky.data.layers.active
# A line along x (points 0..4, 1 apart) and, apart from it, a second curve near point 0.
line = layer.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0), (3.0, 0.0), (4.0, 0.0)])
other = layer.add_curve([(0.0, 0.5), (0.0, 3.0)])
sky.ops.mesh.select_none()
layer.select_points([0])


def weights():
    return _sky.layer_proportional_weights(layer.id)


# Off: only the selection moves.
assert sky.wm.proportional_editing()[:3] == (False, "SMOOTH", False) and abs(sky.wm.proportional_editing()[3] - 0.2) < 1e-6
assert weights() == [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]

# Smooth, size 2: point 1 (half way) gets half; point 2 (at the edge) and beyond, none.
sky.wm.set_proportional_editing(True, "SMOOTH", size=2.0)
w = weights()
assert w[0] == 1.0 and close(w[1], 0.5) and w[2] == 0.0 and w[3] == 0.0, w
# The other curve's first point, 0.5 away straight across, comes along.
assert close(w[5], 3 * 0.75**2 - 2 * 0.75**3), w

# Each falloff, half way out (Blender's formulas, d = 1 - distance / size).
d = 0.5
expected = {"SMOOTH": 3 * d * d - 2 * d**3, "SPHERE": (2 * d - d * d) ** 0.5, "ROOT": d**0.5,
            "INVERSE_SQUARE": d * (2 - d), "SHARP": d * d, "LINEAR": d, "CONSTANT": 1.0}
for falloff, value in expected.items():
    sky.wm.set_proportional_editing(True, falloff, size=2.0)
    assert close(weights()[1], value), (falloff, weights()[1], value)
sky.wm.set_proportional_editing(True, "RANDOM", size=2.0)
assert weights() == weights() and 0.0 <= weights()[1] <= 0.5, "Random: the same each time, up to Linear's"

# Connected Only: only along the curves, so the other curve stays put.
sky.wm.set_proportional_editing(True, "LINEAR", connected=True, size=2.5)
w = weights()
assert close(w[1], 0.6) and close(w[2], 0.2) and w[5] == 0.0 and w[6] == 0.0, w
# Measured along the curve: a bend makes it further than straight across.
bent = layer.add_curve([(10.0, 0.0), (10.0, 2.0), (11.0, 2.0), (11.0, 0.0)])
sky.ops.mesh.select_none()
layer.select_points([7])
w = weights()
assert w[10] == 0.0, ("5 along the curve, beyond the size, though 1 across", w[7:])
sky.wm.set_proportional_editing(True, "LINEAR", connected=False, size=2.5)
assert close(weights()[10], 0.6), "straight across: 1 away"

# O turns it on and off over the viewport, with points to edit.
sky.wm.set_proportional_editing(False)
assert sky.wm.event_simulate("O", area="Viewport")
assert sky.wm.proportional_editing()[0]
assert sky.wm.event_simulate("O", area="Viewport")
assert not sky.wm.proportional_editing()[0]
# Shift O: the next falloff; Alt O: Connected Only.
assert sky.wm.event_simulate("O", shift=True, area="Viewport")
assert sky.wm.proportional_editing()[1] == "SPHERE"
assert sky.wm.event_simulate("O", alt=True, area="Viewport")
assert sky.wm.proportional_editing()[2]
# Only for points: a layer without any, and O does nothing.
assert sky.ops.layer.new() == {'FINISHED'}
assert sky.data.layers.active.point_count == 0
assert sky.ops.transform.proportional_toggle() == {'CANCELLED'}
assert not sky.wm.proportional_editing()[0], "nothing to edit: stays off"

print("Proportional editing tests passed")
