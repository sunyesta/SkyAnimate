"""The deform modifiers keep their shape's build and what they bent between evaluations (so a frame where nothing
they use has changed costs nothing). Whatever does change must still show: after each change, a layer that has bent
before draws exactly as a twin bending for the first time."""
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


cage = new_layer("Cage")
cage.add_curve([(-1, -0.9), (1.0, -1.0), (0.9, 1.0), (-1.0, 0.95)], closed=True, fill=True)
rest = [tuple(p[:2]) for p in cage.points]


def make_art(name):
    a = new_layer(name)
    a.add_curve([(-0.6, -0.5), (0.5, -0.4), (0.4, 0.6), (-0.5, 0.5)], closed=True, fill=True)
    a.add_curve([(-1.4, 0.1), (0.0, 0.2), (1.4, 0.1)])
    layers.active = a
    assert ops.object.modifier_add(type="CAGE_DEFORM") == {'FINISHED'}
    a.modifier_set(0, layer=cage.name, shape=cage.shapes[0][0], cage_method="OUTLINE", precision=0.002,
                   resample_self=False)
    return a


art = make_art("Art")
doc.frame_current = 10
cage.set_point(0, (rest[0][0] - 0.3, rest[0][1] + 0.2))
art.evaluated()

checks = 0


def same_as_fresh(what):
    """`art` (which has bent before) against a new twin with the same settings."""
    global checks
    twin = make_art("Twin%d" % checks)
    twin.modifier_set(0, **{k: v for k, v in art.modifiers[0].items()
                            if k in ("cage_method", "blend", "precision", "strength", "resample_self")})
    for i, p in enumerate(art.points):
        twin.set_point(i, tuple(p[:2]))
    a, b = art.evaluated()[0], twin.evaluated()[0]
    assert len(a) == len(b), (what, len(a), len(b))
    worst = max(math.dist(p, q) for p, q in zip(a, b))
    assert worst < 1e-6, (what, worst)
    checks += 1
    return a


before = same_as_fresh("start")
# The cage moves.
cage.set_point(2, (rest[2][0] + 0.25, rest[2][1] + 0.3))
moved = same_as_fresh("cage moved")
assert max(math.dist(p, q) for p, q in zip(before, moved)) > 1e-3, "the cage's move didn't show"
# Settings.
for setting in ({"blend": 0.1}, {"cage_method": "RIGID"}, {"cage_method": "MEAN_VALUE"}, {"strength": 0.5},
                {"resample_self": True}, {"resample_self": False, "precision": 0.0}):
    was = art.evaluated()[0]
    art.modifier_set(0, **setting)
    now = same_as_fresh(str(setting))
    if "precision" not in setting:
        # (Precision off fits handles to the bend as Resample Self does: the same drawing.)
        assert len(now) != len(was) or max(math.dist(p, q) for p, q in zip(was, now)) > 1e-5, setting
# The art itself moves.
was = art.evaluated()[0]
art.set_point(0, (-0.7, -0.6))
now = same_as_fresh("art point moved")
assert max(math.dist(p, q) for p, q in zip(was, now)) > 1e-3
# Back to an earlier pose: the same as it was then.
cage.set_point(2, rest[2])
same_as_fresh("cage back")
# Frame 0: nothing bends.
doc.frame_current = 0
at_rest = art.evaluated()[0]
assert max(math.dist(p, tuple(q[:2])) for p, q in zip(at_rest, art.points)) < 1e-6
print("deform kept: %d checks" % checks)
