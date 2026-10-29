"""Proportional editing (Blender's, O): moving points drags the points around them too, by a
falloff out to a size; Connected Only measures along the curves. Points only: O does nothing
without editable points.

Run with:  OpenAnimate --background --python tests/test_proportional_editing.py
"""

import _oanim
import oanim


def close(a, b, eps=1e-4):
    return abs(a - b) < eps


layer = oanim.data.layers.active
# A line along x (points 0..4, 1 apart) and, apart from it, a second curve near point 0.
line = layer.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0), (3.0, 0.0), (4.0, 0.0)])
other = layer.add_curve([(0.0, 0.5), (0.0, 3.0)])
oanim.ops.mesh.select_none()
layer.select_points([0])


def weights():
    return _oanim.layer_proportional_weights(layer.id)


# Off: only the selection moves.
assert oanim.wm.proportional_editing()[:3] == (False, "SMOOTH", False) and abs(oanim.wm.proportional_editing()[3] - 0.2) < 1e-6
assert weights() == [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]

# Smooth, size 2: point 1 (half way) gets half; point 2 (at the edge) and beyond, none.
oanim.wm.set_proportional_editing(True, "SMOOTH", size=2.0)
w = weights()
assert w[0] == 1.0 and close(w[1], 0.5) and w[2] == 0.0 and w[3] == 0.0, w
# The other curve's first point, 0.5 away straight across, comes along.
assert close(w[5], 3 * 0.75**2 - 2 * 0.75**3), w

# Each falloff, half way out (Blender's formulas, d = 1 - distance / size).
d = 0.5
expected = {"SMOOTH": 3 * d * d - 2 * d**3, "SPHERE": (2 * d - d * d) ** 0.5, "ROOT": d**0.5,
            "INVERSE_SQUARE": d * (2 - d), "SHARP": d * d, "LINEAR": d, "CONSTANT": 1.0}
for falloff, value in expected.items():
    oanim.wm.set_proportional_editing(True, falloff, size=2.0)
    assert close(weights()[1], value), (falloff, weights()[1], value)
oanim.wm.set_proportional_editing(True, "RANDOM", size=2.0)
assert weights() == weights() and 0.0 <= weights()[1] <= 0.5, "Random: the same each time, up to Linear's"

# Connected Only: only along the curves, so the other curve stays put.
oanim.wm.set_proportional_editing(True, "LINEAR", connected=True, size=2.5)
w = weights()
assert close(w[1], 0.6) and close(w[2], 0.2) and w[5] == 0.0 and w[6] == 0.0, w
# Measured along the curve: a bend makes it further than straight across.
bent = layer.add_curve([(10.0, 0.0), (10.0, 2.0), (11.0, 2.0), (11.0, 0.0)])
oanim.ops.mesh.select_none()
layer.select_points([7])
w = weights()
assert w[10] == 0.0, ("5 along the curve, beyond the size, though 1 across", w[7:])
oanim.wm.set_proportional_editing(True, "LINEAR", connected=False, size=2.5)
assert close(weights()[10], 0.6), "straight across: 1 away"

# Handles come along by their own distance, as Blender's (each Bezier handle a point of its own): a pulled
# point's handle nearer the selection moves further than the point, the one beyond it less, so the curve
# bends into the falloff.
oanim.wm.set_proportional_editing(True, "LINEAR", connected=False, size=2.5)
oanim.ops.mesh.select_none()
layer.select_points([0])
before = layer.curve_handles(line)[1]
assert oanim.ops.mesh.translate(move_x="0", move_y="1") == {'FINISHED'}
after = layer.curve_handles(line)[1]
point_up = layer.points[1][1]
in_up, out_up = after[1][1] - before[1][1], after[2][1] - before[2][1]
assert close(point_up, 0.6), point_up
assert close(in_up, 1.0 - abs(before[1][0]) / 2.5, 1e-3) and in_up > point_up > out_up, (in_up, point_up, out_up)
oanim.ops.ed.undo()
# Connected Only: along the curve, the handle on the selection's side from the neighbour that way.
oanim.wm.set_proportional_editing(True, "LINEAR", connected=True, size=2.5)
oanim.ops.mesh.select_none()
layer.select_points([0])
assert oanim.ops.mesh.translate(move_x="0", move_y="1") == {'FINISHED'}
after = layer.curve_handles(line)[1]
in_up, out_up = after[1][1] - before[1][1], after[2][1] - before[2][1]
assert in_up > layer.points[1][1] > out_up, (in_up, layer.points[1][1], out_up)
oanim.ops.ed.undo()

# O turns it on and off over the viewport, with points to edit.
oanim.wm.set_proportional_editing(False)
assert oanim.wm.event_simulate("O", area="Viewport")
assert oanim.wm.proportional_editing()[0]
assert oanim.wm.event_simulate("O", area="Viewport")
assert not oanim.wm.proportional_editing()[0]
# Shift O: the next falloff; Alt O: Connected Only.
assert oanim.wm.event_simulate("O", shift=True, area="Viewport")
assert oanim.wm.proportional_editing()[1] == "SPHERE"
assert oanim.wm.event_simulate("O", alt=True, area="Viewport")
assert oanim.wm.proportional_editing()[2]
# Only for points: a layer without any, and O does nothing.
assert oanim.ops.layer.new() == {'FINISHED'}
assert oanim.data.layers.active.point_count == 0
assert oanim.ops.transform.proportional_toggle() == {'CANCELLED'}
assert not oanim.wm.proportional_editing()[0], "nothing to edit: stays off"

print("Proportional editing tests passed")
