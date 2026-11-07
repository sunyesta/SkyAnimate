"""Align keeps a point's two handles in line on every frame, not just the one it is done on: keys that turn them apart (one
handle keyed alone, or an action turning them by different amounts) are made to agree. The handle turned into line takes
the other's turn wherever either is keyed, in the timeline and in every action; each keeps its own length. A point
picked turns both, halfway each.

Run with:  OpenAnimate --background --python tests/test_handles_align_keys.py
"""

import math

import oanim

ops, doc, L = oanim.ops, oanim.data.document, oanim.data.layers


def turn_of(v):
    return math.atan2(v[1], v[0])


def wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def handles(layer, f):
    doc.frame_current = f
    _, i, o = layer.curve_handles(0)[1]
    q = layer.points[1][:2]
    return (i[0] - q[0], i[1] - q[1]), (o[0] - q[0], o[1] - q[1])


def bend(layer, f):
    """How far from in line the point's handles are on frame f, in degrees."""
    i, o = handles(layer, f)
    return abs(math.degrees(wrap(turn_of(i) - turn_of((-o[0], -o[1])))))


def turned(v, degrees):
    a = turn_of(v) + math.radians(degrees)
    r = math.hypot(*v)
    return (math.cos(a) * r, math.sin(a) * r)


def setup():
    """A curve whose middle point's in handle alone turns 40 degrees by frame 20 (keyed unaligned)."""
    ops.wm.read_homefile()
    doc.auto_key = True
    doc.frame_current = 0
    layer = L.active
    layer.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
    ops.mesh.select_none()
    layer.select_points([1])
    assert ops.mesh.handles_unalign() == {'FINISHED'}
    doc.frame_current = 20
    i, _ = handles(layer, 20)
    q = layer.points[1][:2]
    t = turned(i, 40)
    layer.move_handle(0, 1, "IN", (q[0] + t[0], q[1] + t[1]))
    ops.mesh.select_none()
    return layer


def align_at_0(layer, pick):
    doc.frame_current = 0
    ops.mesh.select_none()
    if pick == "POINT":
        layer.select_points([1])
    else:
        layer.select_handle(0, 1, pick)
    assert ops.mesh.handles_align() == {'FINISHED'}


# --- The in handle keyed alone: aligning it (picking the in) undoes its own turn, as it follows the out ---------------
layer = setup()
assert bend(layer, 20) > 30, ("set up: turned apart", bend(layer, 20))
len_in = math.hypot(*handles(layer, 20)[0])
align_at_0(layer, "IN")
for f in (0, 5, 10, 20, 30):
    assert bend(layer, f) < 0.05, ("the in handle still turns by itself", f, bend(layer, f))
assert abs(math.hypot(*handles(layer, 20)[0]) - len_in) < 1e-4, "it keeps its own length"

# --- Picking the out: the out takes the in's turn ------------------------------------------------------------------------
layer = setup()
was_in = handles(layer, 20)[0]
align_at_0(layer, "OUT")
for f in (0, 5, 10, 20, 30):
    assert bend(layer, f) < 0.05, ("the out handle doesn't follow", f, bend(layer, f))
assert abs(math.degrees(wrap(turn_of(handles(layer, 20)[0]) - turn_of(was_in)))) < 0.05, "the in keeps its turn"

# --- The point picked: both turn, halfway each -----------------------------------------------------------------------------
layer = setup()
align_at_0(layer, "POINT")
assert bend(layer, 20) < 0.05, bend(layer, 20)
half = math.degrees(wrap(turn_of(handles(layer, 20)[0]) - turn_of(handles(layer, 0)[0])))
assert abs(half - 20) < 0.5, ("halfway each", half)

# --- Aligned on a later frame, the setup bent too: in line on frame 0 as well ---------------------------------------------
layer = setup()
doc.frame_current = 0
i, _ = handles(layer, 0)
q = layer.points[1][:2]
t = turned(i, 30)
layer.move_handle(0, 1, "IN", (q[0] + t[0], q[1] + t[1]))
ops.mesh.select_none()
assert bend(layer, 0) > 20, ("set up: bent on frame 0", bend(layer, 0))
doc.frame_current = 20
ops.mesh.select_none()
layer.select_handle(0, 1, "IN")
assert ops.mesh.handles_align() == {'FINISHED'}
for f in (0, 10, 20):
    assert bend(layer, f) < 0.05, ("aligned on 20, still bent", f, bend(layer, f))

# --- An action that keys the in alone: aligned on the mainline, the action plays them in line ------------------------------
ops.wm.read_homefile()
doc.auto_key = True
doc.frame_current = 0
ops.layer.new_bone()
rig = L.active
ops.layer.new()
layer = L.active
ops.layer.move_to_group(group=str(rig.id))
layer = next(x for x in L.all() if x.id == layer.id)
layer.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
ops.mesh.select_none()
layer.select_points([1])
assert ops.mesh.handles_unalign() == {'FINISHED'}
ops.mesh.select_none()
L.active = layer
assert ops.anim.action_new(name="turn") == {'FINISHED'}
doc.frame_current = 30
i, _ = handles(layer, 30)
q = layer.points[1][:2]
t = turned(i, -50)
layer.move_handle(0, 1, "IN", (q[0] + t[0], q[1] + t[1]))
doc.frame_current = 31
assert bend(layer, 30) > 40, ("set up: the action turns the in alone", bend(layer, 30))
doc.action = ""
align_at_0(layer, "IN")
doc.action = "turn"
for f in (10, 30):
    assert bend(layer, f) < 0.05, ("the action still turns the in by itself", f, bend(layer, f))
doc.action = ""

# Setup handles with no length that grow out in the keys (in line on frame 20): aligned, they stay in line on every frame,
# frame 20 showing the handles it did. The point picked, or only the out handle (the in has nothing to follow).
for pick in ("POINT", "OUT"):
    ops.wm.read_homefile()
    doc.auto_key = True
    doc.frame_current = 0
    layer = L.active
    layer.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
    ops.mesh.select_none()
    layer.select_points([1])
    assert ops.mesh.handles_unalign() == {'FINISHED'}
    q = layer.points[1][:2]
    layer.move_handle(0, 1, "IN", q)
    layer.move_handle(0, 1, "OUT", q)
    doc.frame_current = 20
    layer.move_handle(0, 1, "IN", (q[0] - 0.3, q[1] + 0.1))
    layer.move_handle(0, 1, "OUT", (q[0] + 0.3, q[1] - 0.1))
    doc.frame_current = 21
    was = handles(layer, 20)
    assert bend(layer, 20) < 0.05, ("set up: in line on frame 20", bend(layer, 20))
    doc.frame_current = 0
    ops.mesh.select_none()
    if pick == "POINT":
        layer.select_points([1])
    else:
        layer.select_handle(0, 1, "OUT")
    assert ops.mesh.handles_align() == {'FINISHED'}
    for f in (5, 10, 20, 30):
        assert bend(layer, f) < 0.05, ("zero setups: aligned on every frame", pick, f, bend(layer, f))
    now = handles(layer, 20)
    assert max(math.dist(a, b) for a, b in zip(was, now)) < 1e-4, ("frame 20 kept its handles", pick, was, now)
    i0, o0 = handles(layer, 0)
    assert math.hypot(*i0) < 1e-4 and math.hypot(*o0) < 1e-4, ("frame 0 still shows no handles", i0, o0)

print("test_handles_align_keys: all passed")
