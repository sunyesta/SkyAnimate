"""Align Handles on points already aligned and in line changes nothing: no handle moves, no key is added on the frame
shown (with auto-key on), and it is CANCELLED with no undo step. Pressed twice, the second time does nothing.

Run with:  OpenAnimate --background --python tests/test_align_twice.py
"""

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm


def handle_keys(layer):
    return [len(layer.keyframes("curves[0].points[%d].handle_in" % i)) +
            len(layer.keyframes("curves[0].points[%d].handle_out" % i)) for i in range(3)]


def offsets(layer):
    pts = layer.points
    return [(i[0] - pts[p][0], i[1] - pts[p][1], o[0] - pts[p][0], o[1] - pts[p][1])
            for (t, i, o), p in zip(layer.curve_handles(0), layer.curve_points(0))]


ops.wm.read_homefile(confirmed="1")
doc.auto_key = True
doc.frame_current = 0
layer = layers.active
layer.key_whole_drawing = False
layer.key_together = False
layer.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
ops.mesh.select_none()
# Keyed on frame 10 (a handle turned), so the handles are animated; then posed on 30.
doc.frame_current = 10
q = layer.points[1][:2]
layer.move_handle(0, 1, "OUT", (q[0] + 0.4, q[1] + 0.13))
ops.mesh.select_none()
doc.frame_current = 30
layer.move_handle(0, 1, "IN", (q[0] - 0.3, q[1] + 0.21))
ops.mesh.select_none()
doc.frame_current = 20
doc.frame_current = 30
layer.select_points([0, 1, 2])
assert ops.mesh.handles_align() == {'FINISHED'}, "the first time lines them up"
doc.frame_current = 20
doc.frame_current = 30
layer.select_points([0, 1, 2])
keys = handle_keys(layer)
was = offsets(layer)
assert ops.mesh.handles_align() == {'CANCELLED'}, "in line already: nothing to do"
now = offsets(layer)
assert now == was, (now, was)
assert handle_keys(layer) == keys, ("no key added on the frame shown", keys, handle_keys(layer))

print("Align twice tests passed")
