"""Editing points bent by deform modifiers, on every kind of deform, with bones, Mirror, Resample, Bevel and a group's:

- a point moved, near or far, lands where it was sent, though a skin holds only around where it was; nothing else moves;
- several moved, turned and scaled come out as asked, and each undoes exactly;
- a handle grabbed and not moved leaves the line as it was (what shows is bent, not the stored handle), and moved it
  lands where it was sent;
- a point grabbed, turned by nothing or scaled by one leaves the line too;
- the dots stay on the drawing, a Bevel's outline included;
- an edit on a bent frame leaves the setup alone, and Proportional and X mirror work as ever.

Run with:  OpenAnimate --background --python tests/test_deform_point_edit.py
"""

import math
import random

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
doc.auto_key = True
doc.frame_current = 0
anchor = layers.active
rng = random.Random(5)
made = [0]


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def new_layer(prefix="L"):
    layers.active = anchor
    ops.layer.new()
    layer = layers.active
    made[0] += 1
    layer.name = "%s%d" % (prefix, made[0])
    return layer


def drawing():
    a = new_layer("Art")
    a.add_curve([(-0.55, -0.3), (-0.2, 0.35), (0.25, -0.15), (0.6, 0.3)])
    a.add_curve([(-0.5, -0.6), (0.0, -0.45), (0.5, -0.6), (0.55, 0.55), (-0.45, 0.6)], closed=True)
    return a


def deform(a, kind, target, **settings):
    layers.active = a
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    i = len(a.modifiers) - 1
    a.modifier_set(i, layer=target.name, shape=target.shapes[0][0], **settings)


def box():
    t = new_layer("Cage")
    assert t.add_primitive("BOX", [(-1, -1), (1, 1)])
    return t


def bent(t):
    doc.frame_current = 10
    for i, p in enumerate(t.points):
        t.set_point(i, (p[0] * 1.3 + 0.2 * p[1] + 0.1, p[1] * 0.9 + (0.12 if p[0] > 0.5 else 0.0)))
    at(10)


def cage(method="MEAN_VALUE", **settings):
    def make():
        t = box()
        a = drawing()
        deform(a, "CAGE_DEFORM", t, cage_method=method, **settings)
        bent(t)
        return a
    return make


def mesh(method):
    def make():
        t = new_layer("Grid")
        assert t.add_primitive("GRID", [(-1, -1), (1, 1)], columns=3, rows=3)
        a = drawing()
        deform(a, "MESH_DEFORM", t, mesh_method=method)
        bent(t)
        return a
    return make


def curve():
    t = new_layer("Line")
    t.add_curve([(-1.2, 0.0), (-0.4, 0.05), (0.4, -0.05), (1.2, 0.0)])
    a = drawing()
    deform(a, "CURVE_DEFORM", t)
    doc.frame_current = 10
    for i, p in enumerate(t.points):
        t.set_point(i, (p[0] * 1.15, p[1] + 0.3 * math.sin(p[0] * 2)))
    at(10)
    return a


def with_modifier(kind):
    def make():
        t = box()
        a = drawing()
        layers.active = a
        assert ops.object.modifier_add(type=kind) == {'FINISHED'}
        if kind == "BEVEL":
            a.modifier_set(0, amount=0.05)
        deform(a, "CAGE_DEFORM", t)
        bent(t)
        return a
    return make


def bones(strength=1.0):
    def make():
        layers.active = anchor
        ops.layer.new_bone()
        rig = layers.active
        assert ops.armature.bone_add(name="b", head_x="0", head_y="-0.2", tail_x="1", tail_y="0", parent="") == {'FINISHED'}
        t = box()
        a = drawing()
        name = a.name
        layers.active = a
        assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
        a = next(l for l in layers.all() if l.name == name)
        layers.active = a
        a.select_points(list(range(a.point_count)))
        rig.select_bone("b")
        assert ops.weight.assign(weight="1.0") == {'FINISHED'}
        ops.mesh.select_none()
        deform(a, "CAGE_DEFORM", t, strength=strength)
        bent(t)
        doc.frame_current = 10
        rig.bone_set("b", angle=0.5)
        at(10)
        return a
    return make


def group():
    t = box()
    layers.active = anchor
    ops.layer.new_group()
    g = layers.active
    a = drawing()
    name = a.name
    assert ops.layer.move_to_group(group=str(g.id)) == {'FINISHED'}
    a = next(l for l in layers.all() if l.name == name)
    layers.active = g
    assert ops.object.modifier_add(type="CAGE_DEFORM") == {'FINISHED'}
    g.modifier_set(0, layer=t.name, shape=t.shapes[0][0])
    bent(t)
    return a


def shown(a):
    return [tuple(p) for p in a.shown()[0]]


def stored(a):
    return [tuple(p[:2]) for p in a.points]


def pick(a, *points):
    layers.active = a
    ops.mesh.select_none()
    a.select_points(list(points))


def outline(a):
    points = []
    for shape in range(len(a.evaluated()[2])):
        points.extend(a.evaluated_outline(shape, 24)[0])
    return points


def same_line(before, after):
    """How far the line moved; Precision may have cut it elsewhere, so samples needn't match one for one."""
    if len(before) == len(after):
        return max(math.dist(p, q) for p, q in zip(before, after))
    return max(min(math.dist(p, q) for q in before) for p in after)


def back(a, wanted):
    assert ops.ed.undo() == {'FINISHED'}
    at(10)
    now = shown(a)
    assert len(now) == len(wanted) and max(math.dist(p, q) for p, q in zip(now, wanted)) < 1e-5, "undone exactly"


rigs = [("cage", cage()), ("cage rigid", cage("RIGID")), ("cage envelope", cage("ENVELOPE")), ("cage at half", cage(strength=0.5)),
        ("custom weights", cage(weights="CUSTOM")), ("mesh", mesh("CELLS")), ("mesh envelope", mesh("ENVELOPE")), ("curve", curve),
        ("mirror", with_modifier("MIRROR")), ("resample", with_modifier("RESAMPLE")), ("bevel", with_modifier("BEVEL")),
        ("bones", bones()), ("bones at half", bones(0.5)), ("group", group)]

for name, make in rigs:
    doc.frame_current = 0
    a = make()
    at(10)
    n = a.point_count
    rest = stored(a)

    # Moved, near and far: lands, alone.
    for size in (0.02, 0.15, 0.3):
        for p in range(n):
            angle = rng.uniform(0, 6.28)
            d = (size * math.cos(angle), size * math.sin(angle))
            before = shown(a)
            pick(a, p)
            assert ops.mesh.translate(move_x=str(d[0]), move_y=str(d[1])) == {'FINISHED'}
            at(10)
            now = shown(a)
            miss = math.dist(now[p], (before[p][0] + d[0], before[p][1] + d[1]))
            assert miss < size * 0.04, (name, "point", p, "move", size, "missed by", miss)
            others = max((math.dist(q, w) for i, (q, w) in enumerate(zip(before, now)) if i != p), default=0.0)
            assert others < 1e-5, (name, "another moved", others)
            back(a, before)

    # Several together, turned and scaled.
    before = shown(a)
    points = [0, 2, 5, 7]
    pick(a, *points)
    assert ops.mesh.translate(move_x="0.08", move_y="-0.05") == {'FINISHED'}
    at(10)
    now = shown(a)
    assert max(math.dist(now[p], (before[p][0] + 0.08, before[p][1] - 0.05)) for p in points) < 0.006, (name, "several")
    back(a, before)
    pick(a, *points)
    assert ops.mesh.rotate(angle="25") == {'FINISHED'}
    at(10)
    back(a, before)
    pick(a, *points)
    assert ops.mesh.resize(scale_x="1.3", scale_y="0.8") == {'FINISHED'}
    at(10)
    back(a, before)

    # Grabbed and not moved, a handle: the line stays. Moved, it lands.
    for c in range(2):
        count = len(a.curve_handles(c))
        for i in range(count):
            for side in ("IN", "OUT"):
                if c == 0 and ((side == "IN" and i == 0) or (side == "OUT" and i == count - 1)):
                    continue
                line = outline(a)
                layers.active = a
                ops.mesh.select_none()
                a.select_handle(c, i, side)
                assert ops.mesh.translate(move_x="0.0", move_y="0.0") == {'FINISHED'}
                at(10)
                assert same_line(line, outline(a)) < 0.006, (name, "grabbing a handle", c, i, side, same_line(line, outline(a)))
                ops.ed.undo()
                at(10)
                ends = a.evaluated_handles(c, True)
                if len(ends) != count:
                    continue  # Bent with points of the modifiers', it shows as stored.
                end = ends[i][1 if side == "IN" else 2]
                ops.mesh.select_none()
                a.select_handle(c, i, side)
                assert ops.mesh.translate(move_x="0.04", move_y="0.03") == {'FINISHED'}
                at(10)
                now_end = a.evaluated_handles(c, True)[i][1 if side == "IN" else 2]
                assert math.dist(now_end, (end[0] + 0.04, end[1] + 0.03)) < 0.015, (name, "handle", c, i, side, now_end, end)
                ops.ed.undo()
                at(10)

    # Grabbed, turned by nothing, scaled by one: the line stays.
    for label, call, what in (("grabbed", lambda: ops.mesh.translate(move_x="0.0", move_y="0.0"), (1,)),
                              ("turned by nothing", lambda: ops.mesh.rotate(angle="0"), (0, 1, 2)),
                              ("scaled by one", lambda: ops.mesh.resize(scale_x="1", scale_y="1"), (0, 1, 2))):
        line = outline(a)
        pick(a, *what)
        assert call() == {'FINISHED'}
        at(10)
        assert same_line(line, outline(a)) < 0.006, (name, label, same_line(line, outline(a)))
        ops.ed.undo()
        at(10)

    # The dots are on the drawing.
    line = outline(a)
    assert max(min(math.dist(p, q) for q in line) for p in shown(a)) < 0.01, (name, "dots off the drawing")

    # An edit here leaves the setup alone.
    pick(a, 0)
    assert ops.mesh.translate(move_x="0.1", move_y="0.1") == {'FINISHED'}
    at(0)
    assert max(math.dist(p, q) for p, q in zip(stored(a), rest)) < 1e-5, (name, "the setup moved")
    print(name, "fine", flush=True)

# --- Proportional and X mirror ----------------------------------------------------------------------------------------
doc.frame_current = 0
a = mesh("CELLS")()
at(10)
before = shown(a)
oanim.wm.set_proportional_editing(True, "SMOOTH", size=0.6)
pick(a, 1)
assert ops.mesh.translate(move_x="0.15", move_y="0.1") == {'FINISHED'}
at(10)
now = shown(a)
oanim.wm.set_proportional_editing(False)
assert math.dist(now[1], (before[1][0] + 0.15, before[1][1] + 0.1)) < 1e-3, now[1]
assert 0.0 < math.dist(now[8], before[8]) < 0.15, "a neighbour comes part of the way"
back(a, before)

doc.frame_current = 0
t = box()
b = new_layer("Both")
b.add_curve([(-0.5, 0.2), (0.0, 0.5), (0.5, 0.2)])
deform(b, "CAGE_DEFORM", t)
bent(t)
oanim.wm.set_mirror_editing(True)
before, was = shown(b), stored(b)
pick(b, 0)
assert ops.mesh.translate(move_x="-0.1", move_y="0.05") == {'FINISHED'}
at(10)
oanim.wm.set_mirror_editing(False)
assert math.dist(shown(b)[0], (before[0][0] - 0.1, before[0][1] + 0.05)) < 1e-3
assert abs(stored(b)[2][0] + stored(b)[0][0]) < 1e-4 and stored(b)[2] != was[2], "its image moves too"

print("point editing on deformed drawings: all good")
