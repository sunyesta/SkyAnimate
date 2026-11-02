"""The Resample modifier above the deform modifiers, and Resample Self, which does the same work inside the deform: on
every kind of deform shape (but Envelope's Blend extremes and Curve Deform's Stretch off, tested elsewhere), the drawing
split into 4 pieces a segment, or By Length, and bent comes out as true to the bend (the truth: split into 24 pieces a
segment) as fitting each whole segment back to its bent samples did, or truer, the two the same with Resample Self, both
together truer still; Resample alone, and the deform on frame 0, change nothing; and dragging the shape's points never
makes the Resampled drawing jump.

Run with:  OpenAnimate --background --python tests/test_resample_deform.py
"""
import math

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


def pick(layer, *points):
    ops.mesh.select_none()
    layer.select_points(list(points))


# --- Targets --------------------------------------------------------------------------------------------------
def t_grid():
    g = new_layer("T_grid")
    assert g.add_primitive("GRID", [(-1, -1), (1, 1)], columns=2, rows=2)
    return g


def t_cell():
    c = new_layer("T_cell")
    c.add_curve([(-1, -0.9), (1.0, -1.0), (0.9, 1.0), (-1.0, 0.95)], closed=True, fill=True)
    return c


def t_mesh():
    h = new_layer("T_mesh")
    h.add_curve([(-1.0, 0.8), (0.0, 0.8), (1.0, 0.8)])
    pick(h, 1)
    h.extrude_selected()
    h.set_point(3, (0.0, -0.8))
    pick(h, 0)
    h.extrude_selected()
    h.set_point(4, (-1.0, -0.8))
    pick(h, 4, 3)
    ops.mesh.make_segment()
    pick(h, 2, 3)
    ops.mesh.make_segment()
    ops.mesh.select_none()
    return h


def t_line():
    l = new_layer("T_line")
    l.add_curve([(-1.2, -0.3), (-0.4, 0.2), (0.4, -0.2), (1.2, 0.3)])
    return l


def t_box():
    b = new_layer("T_box")
    assert b.add_primitive("BOX", [(-1, -1), (1, 1)])
    return b


def t_curvy():
    c = new_layer("T_curvy")
    c.add_curve([(-1.0, -0.6), (0.0, -1.1), (1.1, -0.5), (0.8, 0.9), (-0.9, 1.0)], closed=True, fill=True)
    return c


SCENARIOS = [
    ("grid", t_grid, "MESH_DEFORM", {}, "Grid"),
    ("cell", t_cell, "MESH_DEFORM", {}, "Grid"),
    ("mesh", t_mesh, "MESH_DEFORM", {}, "Mesh"),
    ("wire", t_line, "MESH_DEFORM", {}, "Wire"),
    ("curve_stretch", t_line, "CURVE_DEFORM", {"stretch": True}, "Line"),
    ("curve_keep", t_line, "CURVE_DEFORM", {"stretch": False}, "Line"),
    ("box_mv", t_box, "CAGE_DEFORM", {"cage_method": "MEAN_VALUE"}, "Cage"),
    ("box_rigid", t_box, "CAGE_DEFORM", {"cage_method": "RIGID"}, "Cage"),
    ("box_env", t_box, "CAGE_DEFORM", {"cage_method": "ENVELOPE"}, "Cage"),
    ("curvy_mv", t_curvy, "CAGE_DEFORM", {"cage_method": "MEAN_VALUE"}, "Cage"),
    ("curvy_rigid", t_curvy, "CAGE_DEFORM", {"cage_method": "RIGID"}, "Cage"),
    ("curvy_env", t_curvy, "CAGE_DEFORM", {"cage_method": "ENVELOPE"}, "Cage"),
    ("curvy_env_b0", t_curvy, "CAGE_DEFORM", {"cage_method": "ENVELOPE", "blend": 0.0}, "Cage"),
    ("curvy_env_b1", t_curvy, "CAGE_DEFORM", {"cage_method": "ENVELOPE", "blend": 1.0}, "Cage"),
]
COMBOS = [("prec", {"precision": 0.002, "resample_self": False}), ("plain", {"precision": 0.0, "resample_self": False}),
          ("resample", {"resample_self": True})]


def make_art(name):
    a = new_layer(name)
    # Small squares inside and around.
    for i in range(-2, 3):
        for j in range(-2, 3):
            x, y = i * 0.5, j * 0.5
            a.add_curve([(x - 0.08, y - 0.08), (x + 0.08, y - 0.08), (x + 0.08, y + 0.08), (x - 0.08, y + 0.08)],
                        closed=True, fill=True)
    # Lines crossing the outline.
    a.add_curve([(-1.5, 0.1), (-0.5, 0.15), (0.5, 0.05), (1.5, 0.1)])
    a.add_curve([(0.1, -1.5), (0.15, 0.0), (0.1, 1.5)])
    # A thin band just inside the top and one just outside the right.
    a.add_curve([(-0.8, 0.9), (0.0, 0.92), (0.8, 0.9), (0.8, 0.906), (0.0, 0.926), (-0.8, 0.906)], closed=True, fill=True)
    a.add_curve([(1.03, -0.7), (1.05, 0.0), (1.03, 0.7), (1.036, 0.7), (1.056, 0.0), (1.036, -0.7)], closed=True, fill=True)
    # Far points.
    a.add_curve([(3.0, 3.0), (3.5, 2.5), (-3.0, -2.5)])
    return a


def make_art2(name):
    a = new_layer(name)
    for i in range(-1, 2):
        for j in range(-1, 2):
            x, y = i * 0.6, j * 0.6
            a.add_curve([(x - 0.15, y - 0.15), (x + 0.15, y - 0.15), (x + 0.15, y + 0.15), (x - 0.15, y + 0.15)],
                        closed=True, fill=True)
    a.add_curve([(-1.4, 0.3), (-0.3, 0.35), (0.7, 0.25), (1.4, 0.3)])
    a.add_curve([(0.3, -1.4), (0.35, 0.0), (0.3, 1.4)])
    a.add_curve([(-0.8, 0.85), (0.0, 0.87), (0.8, 0.85), (0.8, 0.86), (0.0, 0.88), (-0.8, 0.86)], closed=True, fill=True)
    return a


def outlines(layer, steps):
    shapes = layer.evaluated()[2]
    return [layer.evaluated_outline(k, steps)[0] for k in range(len(shapes))]


def seg_dist(p, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    dd = dx * dx + dy * dy
    t = 0.0 if dd < 1e-20 else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / dd))
    return math.hypot(p[0] - a[0] - dx * t, p[1] - a[1] - dy * t)


def to_line(p, line, closed):
    n = len(line)
    m = n if closed else n - 1
    return min(seg_dist(p, line[i], line[(i + 1) % n]) for i in range(m))


def hausdorff(a, b, closed):
    return max(max(to_line(p, b, closed) for p in a), max(to_line(p, a, closed) for p in b))



VARIANTS = [
    ("resample_self", None, {"resample_self": True, "precision": 0.0, "split": "COUNT", "pieces": 4}),
    ("sub4", {"split": "COUNT", "pieces": 4}, {"resample_self": False, "precision": 0.0}),
    ("sublen", {"split": "LENGTH", "spacing": 0.15}, {"resample_self": False, "precision": 0.0}),
    ("sub4_rs", {"split": "COUNT", "pieces": 4}, {"resample_self": True, "precision": 0.0, "split": "COUNT", "pieces": 4}),
]
# What fitting each whole segment back to its bent samples made of the worst shape, when Resample Self did that.
FIT = {"grid": 0.0296, "cell": 0.0114, "mesh": 0.0200, "wire": 0.0633, "curve_stretch": 0.0854, "box_mv": 0.0320,
       "box_rigid": 0.0268, "box_env": 0.0081, "curvy_mv": 0.0182, "curvy_rigid": 0.0141, "curvy_env": 0.0130}
# How much worse than that it may come out (Curve Deform: where the art crosses between two parts of a wiggly line its
# bend turns sharply, and 4 pieces a segment follow it about as well as a fit).
ROOM = {"curve_stretch": 1.2}
for scn, make_target, mtype, settings, kind in SCENARIOS:
    if scn in ("curvy_env_b0", "curvy_env_b1", "curve_keep"):
        continue
    doc.frame_current = 0
    target = make_target()
    rest_pts = [tuple(p[:2]) for p in target.points]

    def art_with(vname, sub, dset):
        a = make_art2("A_%s_%s" % (scn, vname))
        layers.active = a
        if sub is not None:
            assert ops.object.modifier_add(type="RESAMPLE") == {'FINISHED'}
            a.modifier_set(len(a.modifiers) - 1, **sub)
        assert ops.object.modifier_add(type=mtype) == {'FINISHED'}
        k = len(a.modifiers) - 1
        a.modifier_set(k, layer=target.name, shape=target.shapes[0][0], **settings)
        a.modifier_set(k, **dset)
        return a

    truth = art_with("truth", {"split": "COUNT", "pieces": 24}, {"resample_self": False, "precision": 0.0})
    made = {v: art_with(v, sub, dset) for v, sub, dset in VARIANTS}
    # Frame 0: each draws the art as it is.
    plain = art_with("plain", None, {"resample_self": False, "precision": 0.0})
    reference = outlines(plain, 48)
    closed = [plain.evaluated_outline(k, 4)[1] for k in range(len(reference))]
    for v in made:
        o = outlines(made[v], 6)
        err = max(max(to_line(q, b, c) for q in a) for a, b, c in zip(o, reference, closed))
        # (Measured against the outline drawn in fine straight pieces: what is left is theirs.)
        assert err < 5e-4, ("frame 0 changed", scn, v, err)
    # A bend: two of the shape's points moved.
    doc.frame_current = 10
    a0, m0 = 0, len(rest_pts) // 2
    bent_to = {a0: (rest_pts[a0][0] - 0.25, rest_pts[a0][1] + 0.2), m0: (rest_pts[m0][0] + 0.35, rest_pts[m0][1] + 0.3)}
    for i, q in bent_to.items():
        target.set_point(i, q)
    want = outlines(truth, 4)
    worst = {}
    for v in made:
        o = outlines(made[v], 12)
        worst[v] = max(hausdorff(a, b, c) for a, b, c in zip(o, want, closed))
    room = ROOM.get(scn, 1.05)
    assert abs(worst["resample_self"] - worst["sub4"]) < 1e-5, ("Resample Self is Resample", scn, worst)
    for v in ("sub4", "sublen"):
        assert worst[v] <= FIT[scn] * room + 1e-4, (scn, v, worst)
    assert worst["sub4_rs"] <= worst["sub4"] * 1.05 + 1e-4, (scn, worst)
    # Dragging the shape's points with Resample: no jumps, no points coming or going.
    size = max(p[0] for p in rest_pts) - min(p[0] for p in rest_pts)
    step = size * 0.0025
    for v in ("sub4", "sublen"):
        for which, base in bent_to.items():
            series = []
            for k in range(-6, 7):
                target.set_point(which, (base[0] + step * k, base[1]))
                series.append(made[v].evaluated()[0])
            target.set_point(which, base)
            assert len({len(s) for s in series}) == 1, (scn, v, "points came or went")
            for i in range(1, len(series) - 1):
                jump = max(math.hypot(a[0] - 2 * b[0] + c[0], a[1] - 2 * b[1] + c[1])
                           for a, b, c in zip(series[i - 1], series[i], series[i + 1]))
                assert jump < 0.8 * step, (scn, v, which, i, jump / step)
    doc.frame_current = 0

# Resample alone: the drawing exactly as it was (the split is exact).
lone = make_art2("Lone")
before = outlines(lone, 64)
layers.active = lone
ops.object.modifier_add(type="RESAMPLE")
for sub in ({"split": "COUNT", "pieces": 4}, {"split": "LENGTH", "spacing": 0.07}):
    lone.modifier_set(0, **sub)
    after = outlines(lone, 4)
    cl = [lone.evaluated_outline(k, 4)[1] for k in range(len(after))]
    err = max(max(to_line(q, b, c) for q in a) for a, b, c in zip(after, before, cl))
    assert err < 5e-4, (sub, err)

print("Resample with deform tests passed")
