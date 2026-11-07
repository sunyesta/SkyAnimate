"""Clear Handles (Alt R away from frame 0, mesh.clear_handles): the picked points' handles go back to their setup shape;
a handle picked on its own goes back by itself, and on an aligned point the other turns to stay in line with it, as a
handle dragged by itself turns it. Amount takes part of the way back, still in line.

Run with:  OpenAnimate --background --python tests/test_clear_handles.py
"""

import math

import oanim

ops, doc, L = oanim.ops, oanim.data.document, oanim.data.layers


def turn_of(v):
    return math.atan2(v[1], v[0])


def handles(layer, f):
    doc.frame_current = f
    t, i, o = layer.curve_handles(0)[1]
    q = layer.points[1][:2]
    return t, (i[0] - q[0], i[1] - q[1]), (o[0] - q[0], o[1] - q[1])


def bend(layer, f):
    _, i, o = handles(layer, f)
    d = turn_of(i) - turn_of((-o[0], -o[1]))
    return abs(math.degrees((d + math.pi) % (2 * math.pi) - math.pi))


def setup():
    """An aligned point whose in handle (and so its out) is turned 30 degrees and stretched on frame 20."""
    ops.wm.read_homefile(confirmed="1")
    doc.auto_key = True
    doc.frame_current = 0
    layer = L.active
    layer.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
    ops.mesh.select_none()
    t, i0, o0 = handles(layer, 0)
    assert t == "ALIGNED", t
    doc.frame_current = 20
    _, i, _ = handles(layer, 20)
    q = layer.points[1][:2]
    a, r = turn_of(i) + math.radians(30), math.hypot(*i) * 1.5
    layer.move_handle(0, 1, "IN", (q[0] + math.cos(a) * r, q[1] + math.sin(a) * r))
    doc.frame_current = 21
    assert bend(layer, 20) < 0.05, ("set up in line", bend(layer, 20))
    return layer, i0, o0


# The point picked: both handles back to their setup shape on that frame.
layer, i0, o0 = setup()
doc.frame_current = 20
ops.mesh.select_none()
layer.select_points([1])
assert ops.mesh.clear_handles() == {'FINISHED'}
_, i, o = handles(layer, 20)
assert math.dist(i, i0) < 1e-4 and math.dist(o, o0) < 1e-4, ("both back", i, i0, o, o0)

# The in handle alone: it goes back, and the out turns with it (keeping its own length), so the point stays in line.
layer, i0, o0 = setup()
_, i20, o20 = handles(layer, 20)
doc.frame_current = 20
ops.mesh.select_none()
layer.select_handle(0, 1, "IN")
assert ops.mesh.clear_handles() == {'FINISHED'}
_, i, o = handles(layer, 20)
assert math.dist(i, i0) < 1e-4, ("the in back", i, i0)
assert bend(layer, 20) < 0.05, ("in line after clearing one handle", bend(layer, 20))
assert abs(math.hypot(*o) - math.hypot(*o20)) < 1e-4, ("the out keeps its length", o, o20)

# Part of the way back (amount 0.5), the out handle alone: still in line.
layer, i0, o0 = setup()
doc.frame_current = 20
ops.mesh.select_none()
layer.select_handle(0, 1, "OUT")
assert ops.mesh.clear_handles(amount="0.5") == {'FINISHED'}
assert bend(layer, 20) < 0.05, ("in line after clearing half of one handle", bend(layer, 20))

# Unaligned, a handle cleared alone leaves the other where it is.
layer, i0, o0 = setup()
doc.frame_current = 20
ops.mesh.select_none()
layer.select_points([1])
assert ops.mesh.handles_unalign() == {'FINISHED'}
_, _, o20 = handles(layer, 20)
ops.mesh.select_none()
layer.select_handle(0, 1, "IN")
assert ops.mesh.clear_handles() == {'FINISHED'}
_, i, o = handles(layer, 20)
assert math.dist(i, i0) < 1e-4 and math.dist(o, o20) < 1e-4, ("unaligned: the out stays", o, o20)

print("test_clear_handles: all passed")
