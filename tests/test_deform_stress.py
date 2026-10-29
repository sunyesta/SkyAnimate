"""The deform modifiers under stress, every kind of shape and setting (Grid, a curved cell, a hand-made mesh with a
three-sided cell, a wire, Curve Deform stretching or not, Cage Deform by Mean Value, Rigid and Envelope on a box and a
curvy cage, Envelope at Blend 0 and 1; each with Precision, without it, and with Resample Self), on a drawing of
small squares, lines crossing the outline, thin bands and far points:

- a shape keyed where it rests bends nothing, and one moved or turned whole carries the drawing exactly with it;
- dragging the shape's points by fine steps never makes the drawing jump (no point, no shape's area);
- collapsed, flipped, huge or flattened shapes, a point dragged far off or onto another: never NaN.

Run with:  OpenAnimate --background --python tests/test_deform_stress.py
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



# How far a point may stray from its smooth path in one step (second difference, in steps) and how much a shape's
# area may (second difference, of the area): curving fast is fine, a jump is not. Each kind's own measure, with room.
LIMITS = {"grid": (0.05, 0.01), "cell": (0.05, 0.01), "mesh": (0.05, 0.01), "wire": (0.15, 0.02),
          "curve_stretch": (0.2, 0.05), "curve_keep": (0.2, 0.05), "box_mv": (0.05, 0.01), "box_rigid": (0.05, 0.01),
          "box_env": (0.05, 0.01), "curvy_mv": (0.05, 0.01), "curvy_rigid": (0.05, 0.01), "curvy_env": (0.6, 0.03),
          "curvy_env_b0": (0.6, 0.03), "curvy_env_b1": (0.6, 0.03)}


def finite(pts):
    return all(math.isfinite(v) for p in pts for v in p)


def handles_of(layer):
    return [[(tuple(i), tuple(o)) for _, i, o in layer.curve_handles(c)] for c in range(layer.curve_count)]


def key_all(target, rest_pts, rest_h, fn):
    """Key every point and handle of `target` at frame 10 where fn puts them from rest."""
    doc.frame_current = 10
    for i, p in enumerate(rest_pts):
        target.set_point(i, fn(p))
    for c, hs in enumerate(rest_h):
        for k, (hi, ho) in enumerate(hs):
            target.place_handle(c, k, "IN", fn(hi), keep_other=True)
            target.place_handle(c, k, "OUT", fn(ho), keep_other=True)


turn = math.radians(30)
rot = lambda p: (math.cos(turn) * p[0] - math.sin(turn) * p[1], math.sin(turn) * p[0] + math.cos(turn) * p[1])
for scn, make_target, mtype, settings, kind in SCENARIOS:
    doc.frame_current = 0
    target = make_target()
    art = make_art("Art_" + scn)
    layers.active = art
    assert ops.object.modifier_add(type=mtype) == {'FINISHED'}
    art.modifier_set(0, layer=target.name, shape=target.shapes[0][0], **settings)
    assert art.modifiers[0]["kind"] == kind, (scn, art.modifiers[0]["kind"])
    rest_pts = [tuple(p[:2]) for p in target.points]
    rest_h = handles_of(target)
    art_rest = [tuple(p[:2]) for p in art.points]
    n0 = len(art_rest)
    size = max(p[0] for p in rest_pts) - min(p[0] for p in rest_pts)
    step = size * 0.0025
    most_step, most_area = LIMITS[scn]
    for cname, cset in COMBOS:
        art.modifier_set(0, **cset)
        for what, fn, tolerance in (("keyed at rest", lambda p: p, 1e-4),
                                    ("moved whole", lambda p: (p[0] + 0.3, p[1] - 0.2), 2e-3),
                                    ("turned whole", rot, 5e-3)):
            key_all(target, rest_pts, rest_h, fn)
            pts = art.evaluated()[0]
            assert finite(pts), (scn, cname, what)
            err = max(math.dist(p, fn(q)) for p, q in zip(pts[:n0], art_rest))
            assert err < tolerance, (scn, cname, what, err)
        key_all(target, rest_pts, rest_h, lambda p: p)
        for which in sorted({0, len(rest_pts) // 2}):
            for d in ((1, 0), (0, 1)):
                series, areas = [], []
                for k in range(-10, 11):
                    p0 = rest_pts[which]
                    target.set_point(which, (p0[0] + d[0] * step * k, p0[1] + d[1] * step * k))
                    ev = art.evaluated()
                    assert finite(ev[0]), (scn, cname, which, k)
                    series.append(ev[0][:n0])
                    areas.append([sh[4] for sh in ev[2]])
                target.set_point(which, rest_pts[which])
                for i in range(1, len(series) - 1):
                    jump = max(math.dist((a[0] - 2 * b[0] + c[0], a[1] - 2 * b[1] + c[1]), (0, 0))
                               for a, b, c in zip(series[i - 1], series[i], series[i + 1]))
                    assert jump < most_step * step, ("a point jumped", scn, cname, which, d, i, jump / step)
                    for sh in range(len(areas[i])):
                        if areas[i][sh] > 1e-5:
                            r = abs(areas[i - 1][sh] - 2 * areas[i][sh] + areas[i + 1][sh]) / areas[i][sh]
                            assert r < most_area, ("an area jumped", scn, cname, which, d, i, sh, r)
        for what, fn in (("collapsed", lambda p: (0.01 * p[0], 0.01 * p[1])), ("flipped", lambda p: (-p[0], p[1])),
                         ("huge", lambda p: (p[0] * 50.0, p[1] * 50.0)), ("flat", lambda p: (p[0], 0.0))):
            key_all(target, rest_pts, rest_h, fn)
            assert finite(art.evaluated()[0]), (scn, cname, what)
        key_all(target, rest_pts, rest_h, lambda p: p)
        target.set_point(0, (rest_pts[0][0] + 8.0, rest_pts[0][1] - 6.0))
        assert finite(art.evaluated()[0]), (scn, cname, "dragged far")
        target.set_point(0, rest_pts[1])
        assert finite(art.evaluated()[0]), (scn, cname, "onto another")
        key_all(target, rest_pts, rest_h, lambda p: p)
    doc.frame_current = 0

print("Deform stress tests passed")
