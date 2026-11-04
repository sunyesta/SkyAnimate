"""Envelope Deform bends a line by its edges, as Toon Boom Harmony 24 does (2026-11-03). Harmony moves each edge of a
stroke on its own, so where a stroke crosses a tear line (two stretches of the envelope about as near) the edges jump at
different places: Harmony fills between them as a wide band, and where they twist it leaves a gap. Three of 17 open
envelopes rendered in Harmony with 4 open pencil strokes each (scaled, handles turned, a fold): spots on Harmony's ink,
some in those bands, and spots where Harmony has no ink although the bent middle of the line runs through them. Open
Animate's line edges (Shape.line_edges, filled Nonzero) must cover the first and miss the second.
Then a line bent by an Envelope Deform and then by another deform after it, as a lash in an eye group with an Envelope
Deform, inside one with a Mesh Deform, is (2026-11-03): the second bends the edges too, so they stay round the line's
bent middle, where the wire shows it, and don't stay behind where the Envelope Deform left them. Likewise a Cage Deform
after an Envelope Deform in a drawing's own stack.

Each envelope is 4 points in Harmony's field units with its segments as Harmony's Curve nodes keep them (the start
handle's length and angle, the end handle's length and angle, degrees); each stroke is 3 points and its handles. Harmony's
fields are 4:3, so here x is scaled by 4/3.

Run with:  OpenAnimate --background --python tests/test_envelope_edges.py
"""
import math

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
K, AX = 0.1, 4.0 / 3.0


def to_layer(p):
    return (p[0] * AX * K, p[1] * K)


CASES = [
    {"kind": "scale", "rest": {"pts": [[2.576923, -1.180769], [2.059796, 1.996815], [-1.383995, 1.737209], [-2.140345, -1.607933]], "segs": [[1.073129, 99.243405, 1.073129, 99.243405], [1.151187, -175.688984, 1.151187, -175.688984], [1.143194, -102.740568, 1.143194, -102.740568]]}, "now": {"pts": [[3.797659, -1.740121], [3.03556, 2.942744], [-2.03962, 2.560158], [-3.154266, -2.36964]], "segs": [[1.581491, 99.243405, 1.581491, 99.243405], [1.696526, -175.688984, 1.696526, -175.688984], [1.684747, -102.740568, 1.684747, -102.740568]]}, "strokes": [{"pts": [[0.745202, -0.656216], [-0.06639, -1.393146], [-1.221008, -3.256386]], "ctrl": [[[0.778505, -0.719294], [0.11489, -0.959473]], [[-0.3865, -2.061808], [-0.56544, -2.96828]]]}, {"pts": [[1.523913, -2.455863], [0.469347, -3.174713], [-0.400092, -4.513941]], "ctrl": [[[1.263993, -2.359108], [0.625533, -3.326051]], [[0.02036, -3.478613], [-0.348219, -4.331846]]]}, {"pts": [[2.490769, 0.76115], [1.218636, 0.195178], [0.312994, -0.649197]], "ctrl": [[[2.011441, 0.817283], [1.974057, 0.68805]], [[0.82429, -0.019795], [0.468064, -0.658798]]]}, {"pts": [[-0.053676, 1.66523], [1.794802, 0.58693], [2.470368, -0.408414]], "ctrl": [[[0.523003, 1.125927], [0.949907, 0.877551]], [[2.120577, 0.250249], [2.097477, 0.194662]]]}], "ink": [[1.3042, 0.5278], [1.7292, 0.0833], [1.6792, 0.1722], [1.2708, 0.5611], [1.2625, 0.5611], [1.5708, 0.6278], [1.8625, 0.3278], [1.6625, 0.1611], [-1.8292, -3.7278], [-1.8042, -3.7056], [-0.9958, -1.7833], [3.4458, -0.1611], [0.2792, 2.3278]], "empty": [[2.0708, 0.0278], [2.0208, -0.0167], [2.1292, -0.0833], [2.1208, -0.05], [2.0375, -0.0056], [2.1042, -0.0944]]},
    {"kind": "handles", "rest": {"pts": [[0.211347, 2.949397], [-3.356553, -0.530582], [-0.22641, -3.646487], [3.245649, -1.004929]], "segs": [[1.754004, -107.633211, 1.200232, -122.469321], [1.998925, -33.175475, 1.015693, -15.403911], [2.000596, 10.02033, 1.834966, 7.817056]]}, "now": {"pts": [[0.211347, 2.949397], [-3.356553, -0.530582], [-0.22641, -3.646487], [3.245649, -1.004929]], "segs": [[1.090884, -81.049494, 1.896215, -81.322474], [2.89423, -32.122653, 0.922354, -8.846921], [3.97856, 23.220231, 3.327377, -19.412608]]}, "strokes": [{"pts": [[-0.130816, 1.544511], [-1.320679, 3.349545], [-2.594871, 4.304613]], "ctrl": [[[-0.497565, 2.322937], [-0.757546, 3.079848]], [[-1.816925, 3.928887], [-2.036751, 4.26906]]]}, {"pts": [[2.17307, 2.104085], [3.900337, 2.482945], [5.688449, 2.965586]], "ctrl": [[[2.990628, 2.167661], [3.260946, 2.07361]], [[4.68936, 3.036634], [4.992829, 2.53883]]]}, {"pts": [[-2.700749, -1.20618], [-3.700386, -0.821641], [-4.53262, -0.045807]], "ctrl": [[[-2.915577, -0.857253], [-3.623627, -1.299945]], [[-4.01079, -0.495777], [-3.927773, -0.675319]]]}, {"pts": [[-3.476039, -3.152472], [-2.229334, -1.771203], [-1.571953, -0.68604]], "ctrl": [[[-3.312481, -2.867199], [-2.906892, -2.025623]], [[-2.160823, -1.370894], [-1.537377, -1.064168]]]}], "ink": [[-0.2208, 2.5056], [4.1625, 1.9722], [6.2625, 1.55], [4.1875, 1.95], [-1.6542, 4.9611]], "empty": [[3.6125, 1.95], [3.5042, 2.0944], [3.6375, 1.9278], [3.6542, 1.85], [3.5875, 1.9722], [3.6042, 1.9167]]},
    {"kind": "fold", "rest": {"pts": [[-1.800039, 2.028534], [-1.382435, -2.178198], [1.747617, -1.220642], [2.565663, 1.40493]], "segs": [[1.535336, -61.068467, 1.079861, -63.037352], [1.349119, -7.985144, 0.776701, 22.840208], [1.265484, 80.655791, 0.863851, 73.175238]]}, "now": {"pts": [[-1.800039, 2.028534], [0.131032, -0.190534], [1.747617, -1.220642], [2.565663, 1.40493]], "segs": [[1.535336, -61.068467, 1.079861, -63.037352], [1.349119, -7.985144, 0.776701, 22.840208], [1.265484, 80.655791, 0.863851, 73.175238]]}, "strokes": [{"pts": [[-0.973411, -1.40485], [0.581072, -1.266005], [1.823932, -0.555424]], "ctrl": [[[-0.305575, -1.673967], [-0.0217, -1.34288]], [[1.368885, -0.765352], [1.532794, -1.182406]]]}, {"pts": [[-0.587926, 0.873617], [-0.664688, 2.42142], [1.09017, 3.731516]], "ctrl": [[[-0.373953, 1.154936], [-0.546247, 1.737801]], [[-0.178944, 2.889933], [0.343782, 3.164729]]]}, {"pts": [[-0.49951, 0.626416], [0.497286, 2.217729], [2.170013, 3.542922]], "ctrl": [[[-0.116998, 1.335888], [0.030348, 1.9488]], [[0.728681, 2.372118], [1.287954, 3.243347]]]}, {"pts": [[1.354572, -2.151724], [-0.353652, -2.254068], [-1.275607, -1.377867]], "ctrl": [[[0.510833, -2.486552], [0.141229, -2.561807]], [[-0.324478, -2.02039], [-0.959036, -1.552136]]]}], "ink": [[1.3875, 3.0722], [1.6625, 3.2833], [1.0042, 0.1611], [1.3208, -0.1722], [1.1208, -1.8056]], "empty": [[0.6125, 1.9278], [0.6042, 2.1278], [0.6208, 2.0167], [0.6458, 1.9611], [0.2375, 3.8167], [0.0042, 3.5611]]},
]


def handles(state):
    """Each point's (in, out) handle, from its segments' lengths and angles."""
    pts, segs = state["pts"], state["segs"]
    h = [[None, None] for _ in pts]
    for i, (l0, a0, l1, a1) in enumerate(segs):
        a, b = pts[i], pts[i + 1]
        h[i][1] = (a[0] + l0 * math.cos(math.radians(a0)), a[1] + l0 * math.sin(math.radians(a0)))
        h[i + 1][0] = (b[0] - l1 * math.cos(math.radians(a1)), b[1] - l1 * math.sin(math.radians(a1)))
    return h


def pose(layer, state):
    for i, p in enumerate(state["pts"]):
        layer.set_point(i, to_layer(p))
    for i, (h_in, h_out) in enumerate(handles(state)):
        for side, h in (("IN", h_in), ("OUT", h_out)):
            if h is not None:
                layer.place_handle(0, i, side, to_layer(h), keep_other=True)


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


def winding(rings, p):
    """Nonzero winding number of the rings around p."""
    w = 0
    for ring in rings:
        for k in range(len(ring)):
            (x0, y0), (x1, y1) = ring[k], ring[(k + 1) % len(ring)]
            if (y0 <= p[1]) != (y1 <= p[1]):
                x = x0 + (p[1] - y0) / (y1 - y0) * (x1 - x0)
                if x > p[0]:
                    w += 1 if y1 > y0 else -1
    return w


for n, case in enumerate(CASES):
    what = "%s (%d)" % (case["kind"], n)
    ops.wm.read_homefile(confirmed="1")
    doc.frame_current = 0
    cage = new_layer("Envelope")
    cage.add_curve([to_layer(p) for p in case["rest"]["pts"]])
    pose(cage, case["rest"])
    art = new_layer("Art")
    for k, s in enumerate(case["strokes"]):
        art.add_curve([to_layer(p) for p in s["pts"]])
        for g in range(2):
            art.place_handle(k, g, "OUT", to_layer(s["ctrl"][g][0]), keep_other=True)
            art.place_handle(k, g + 1, "IN", to_layer(s["ctrl"][g][1]), keep_other=True)
    assert ops.object.modifier_add(type="ENVELOPE_DEFORM") == {'FINISHED'}
    art.modifier_set(0, layer="Envelope", shape=cage.shapes[0][0])
    doc.frame_current = 8
    pose(cage, case["now"])
    doc.frame_current = 9
    doc.frame_current = 8
    rings = []
    for k in range(len(case["strokes"])):
        edges = art.evaluated_line_edges(k, render=True)
        assert edges, (what, "stroke %d has no edges" % k)
        rings += edges
    for p in case["ink"]:
        assert winding(rings, to_layer(p)) != 0, (what, "Harmony has ink here", p)
    for p in case["empty"]:
        assert winding(rings, to_layer(p)) == 0, (what, "Harmony has no ink here", p)


# --- A deform after the Envelope Deform -------------------------------------------------------------------------
def edges_of(layer):
    return [layer.evaluated_line_edges(k, render=True) for k in range(layer.shape_count)]


def carried(layer, holder, index, move, what):
    """The edges with the deform after the Envelope Deform on, against them with it off moved by `move` (the cage
    moved whole): each place the same."""
    with_it = edges_of(layer)
    holder.modifier_set(index, show_viewport=False, show_render=False)
    doc.frame_current = 9
    doc.frame_current = 8
    without = edges_of(layer)
    holder.modifier_set(index, show_viewport=True, show_render=True)
    doc.frame_current = 9
    doc.frame_current = 8
    assert all(without) and len(with_it) == len(without), (what, "drawn by its edges")
    for rings_on, rings_off in zip(with_it, without):
        assert len(rings_on) == len(rings_off), (what, "rings")
        for ring_on, ring_off in zip(rings_on, rings_off):
            assert len(ring_on) == len(ring_off), (what, "places")
            worst = max(math.dist(p, (q[0] + move[0], q[1] + move[1])) for p, q in zip(ring_on, ring_off))
            assert worst < 1e-4, (what, "the edges went with the line", round(worst, 5))


ops.wm.read_homefile(confirmed="1")
doc.frame_current = 0
anchor = layers.active


def made(name, points=None):
    layers.active = anchor
    ops.layer.new()
    layer = layers.active
    layer.name = name
    if points:
        layer.add_curve(points)
        layer.set_shape_style(0, has_outline=True, line_width=0.02)
    return layer


def grouped(name, *children):
    layers.active = anchor
    assert ops.layer.new_group() == {'FINISHED'}
    group = layers.active
    group.name = name
    for child in children:
        layers.active = child
        assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
    return group


def deform(on, kind, shape_layer, **values):
    layers.active = on
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    index = len(on.modifiers) - 1
    on.modifier_set(index, layer=shape_layer.name, shape=shape_layer.shapes[0][0], **values)


env = made("Env", [(0.0, -0.4), (0.0, 0.4)])
box = made("Box")
assert box.add_primitive("BOX", [(-0.8, -0.8), (0.8, 0.8)])
lash = made("Lash", [(-0.3, 0.1), (0.3, 0.2)])
own = made("Own", [(-0.3, -0.2), (0.3, -0.1)])
inner = grouped("Inner", lash)
outer = grouped("Outer", inner)
deform(inner, "ENVELOPE_DEFORM", env)
deform(outer, "CAGE_DEFORM", box, cage_method="MEAN_VALUE", resample_self=False)
deform(own, "ENVELOPE_DEFORM", env)
deform(own, "CAGE_DEFORM", box, cage_method="MEAN_VALUE", resample_self=False)
doc.frame_current = 8
env.set_point(1, (0.25, 0.35))
# The cage moved whole: the line keeps its width, and edges left where the Envelope Deform put them would be 0.3 off.
for i, p in enumerate(box.points):
    box.set_point(i, (p[0] + 0.3, p[1] + 0.05))
doc.frame_current = 9
doc.frame_current = 8
carried(lash, outer, 0, (0.3, 0.05), "an Envelope Deform group inside a Cage Deform group")
carried(own, own, 1, (0.3, 0.05), "an Envelope Deform then a Cage Deform in its own stack")

print("test_envelope_edges: OK")
