"""Influence (Capsule) on Envelope Deform, and Curve Deform's Envelope method, against Toon Boom Harmony 24's Curve
deformer with Elliptic influence (measured 2026-11-03 through createVertexTransform; reports/Harmony curve deformer.md).

tests/data/curve_influence.json holds 8 cases, 1,340 of Harmony's points, most of them where the influence decides
(the fade round a capsule, and the seam where two segments' capsules meet):
- one Curve moved up a field whole, under six influences: symmetric, narrow and long, a short fade, uneven sides and
  ends, an arched curve and a diagonal one;
- a chain of two Curves, its joint turned 30 degrees and moved, where the second segment's capsule leads where they
  overlap and they blend half and half where they meet.
Each case: its segments (Bezier controls in Harmony's field units), how they move (`move`, or `bent` segments), the
influence (left, right, begin, end, fade, symmetric; across in fields high, along and fade in fields wide, as Harmony
measures them) and points (x, y, Harmony's x, Harmony's y).

Also: Curve Deform with Method Envelope bends exactly as Envelope Deform does, with Standard and Weighted and either
Influence; Everywhere leaves Envelope Deform as it was; and the settings save, open and reach Python.

Run with:  OpenAnimate --background --python tests/test_curve_influence.py
"""
import json
import math
import os
import tempfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
HERE = os.path.dirname(os.path.abspath(__file__))
K, AX = 0.1, 4.0 / 3.0
CASES = json.load(open(os.path.join(HERE, "data", "curve_influence.json")))


def to_layer(p):
    return (p[0] * AX * K, p[1] * K)


def to_field(p):
    return (p[0] / (AX * K), p[1] / K)


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


def pose(layer, segments):
    """The line through the segments' ends, each handle where its control is."""
    ends = [segments[0][0]] + [s[3] for s in segments]
    for i, p in enumerate(ends):
        layer.set_point(i, to_layer(p))
    for i, s in enumerate(segments):
        layer.place_handle(0, i, "OUT", to_layer(s[1]), keep_other=True)
        layer.place_handle(0, i + 1, "IN", to_layer(s[2]), keep_other=True)


def influence_args(infl):
    left, right, begin, end, fade, symmetric = infl
    return dict(influence="CAPSULE", influence_symmetric=symmetric, influence_left=left * K,
                influence_right=right * K, influence_begin=begin * AX * K, influence_end=end * AX * K,
                influence_fade=fade * AX * K)


def bent_of(case):
    if "bent" in case:
        return case["bent"]
    dx, dy = case["move"]
    return [[[x + dx, y + dy] for x, y in s] for s in case["segments"]]


def run(case, extra=None, kinds=("ENVELOPE_DEFORM", "CURVE_DEFORM")):
    """Each kind's bent points, at frame 8."""
    ops.wm.read_homefile(confirmed="1")
    doc.frame_current = 0
    segments = case["segments"]
    line = new_layer("Line")
    line.add_curve([to_layer(p) for p in [segments[0][0]] + [s[3] for s in segments]])
    pose(line, segments)
    arts = []
    for kind in kinds:
        art = new_layer("Art " + kind)
        art.add_curve([to_layer(q[:2]) for q in case["points"]])
        assert ops.object.modifier_add(type=kind) == {'FINISHED'}
        args = dict(layer="Line", shape=line.shapes[0][0], precision=0.0)
        if kind == "CURVE_DEFORM":
            args["curve_method"] = "ENVELOPE"
        args.update(extra if extra is not None else influence_args(case["influence"]))
        art.modifier_set(0, **args)
        arts.append(art)
    for art in arts:
        got = art.evaluated()[0]
        assert all(math.dist(g, to_layer(q[:2])) < 1e-6 for g, q in zip(got, case["points"])), (case["name"], "rest")
    doc.frame_current = 8
    pose(line, bent_of(case))
    doc.frame_current = 9
    doc.frame_current = 8
    return [art.evaluated()[0] for art in arts]


def error(g, q):
    f = to_field(g)
    return math.hypot((f[0] - q[2]) * AX, f[1] - q[3])


# Against Harmony. A point's error is in fields (high), across at 4:3; each case moves its points up to a field, so an
# error is about the influence's error. These points are the hard ones (fades and seams): over Harmony's whole grids the
# influence is off 0.0007 on average, but just round an end cap's tip Harmony's capsule is a little flatter than a half
# ellipse (no polygon or sampling of one fits it better), so a few points there are off up to about 0.15.
summary = []
for case in CASES:
    envelope, curve = run(case)
    errors = sorted(error(g, q) for g, q in zip(envelope, case["points"]))
    mean = sum(errors) / len(errors)
    near = sum(e < 0.02 for e in errors) / len(errors)
    summary.append("%s: mean %.4f, %d%% within 0.02, worst %.3f" % (case["name"], mean, round(near * 100), errors[-1]))
    assert mean < 0.012 and near > 0.85 and errors[-1] < 0.2, (case["name"], "as Harmony puts it", summary[-1])
    worst = max(math.dist(a, b) for a, b in zip(envelope, curve))
    assert worst < 1e-6, (case["name"], "Curve Deform's Envelope is Envelope Deform", worst)
print("\n".join(summary))

# Curve Deform's Envelope is Envelope Deform with every setting: Standard and Weighted, Everywhere and Capsule.
arch = CASES[4]
for extra in (dict(influence="EVERYWHERE"), dict(envelope_method="BLENDED"),
              dict(envelope_method="BLENDED", pre_blend=0.5, post_blend=1.0, influence="CAPSULE"),
              dict(influence_args(arch["influence"]), envelope_method="STANDARD")):
    envelope, curve = run(arch, extra)
    assert max(math.dist(a, b) for a, b in zip(envelope, curve)) < 1e-6, ("the same as Envelope Deform", extra)

# Everywhere is Envelope Deform as it was: a point far outside any capsule still follows.
far = dict(CASES[0], points=[[0.0, 2.5, 0.0, 3.5], [-4.6, 0.0, -4.6, 1.0]])
everywhere, = run(far, dict(influence="EVERYWHERE"), kinds=("ENVELOPE_DEFORM",))
capsule, = run(far, None, kinds=("ENVELOPE_DEFORM",))
for g, c, q in zip(everywhere, capsule, far["points"]):
    assert error(g, q) < 0.001, ("Everywhere carries it", q, to_field(g))
    assert math.dist(c, to_layer(q[:2])) < 1e-6, ("past the fade it stays", q, to_field(c))

# Saved, opened and in Python.
run(CASES[3], kinds=("CURVE_DEFORM",))
art = layers.active
path = os.path.join(tempfile.mkdtemp(), "influence.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
ops.wm.read_homefile(confirmed="1")
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
m = [l for l in layers if l.name.startswith("Art")][0].modifiers[0]
assert m["curve_method"] == "ENVELOPE" and m["influence"] == "CAPSULE" and m["influence_symmetric"] is False, m
assert abs(m["influence_right"] - 1.5 * K) < 1e-6 and abs(m["influence_begin"] - 0.3 * AX * K) < 1e-6, m
assert abs(m["influence_fade"] - 0.5 * AX * K) < 1e-6, m

print("test_curve_influence: OK")
