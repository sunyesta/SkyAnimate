"""Layer.shown() says where points and handles show (bones and parents included, in document space), and
place_point / place_handle put them there: what matching a drawing to a reference over it works with.

Run with:  OpenAnimate --background --python tests/test_shown_place.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
assert oanim.ops.armature.bone_primitive_add() == {'FINISHED'}
bone = rig.bones[0]["name"]
assert oanim.ops.layer.new() == {'FINISHED'}
art = layers.active
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
art = next(l for l in layers.all() if l.id == art.id)
art.add_curve([(0.2, 0.1), (0.4, 0.3), (0.1, 0.5)])
assert oanim.ops.layer.parent_bone_set(bone=bone, deform="POINTS") == {'FINISHED'}
art.location_x = 0.05  # a layer offset too

points, curves = art.shown()
assert len(points) == 3 and len(curves) == 1
cps, closed = curves[0]
assert not closed and [c[0] for c in cps] == [0, 1, 2]
assert close(points[0], (0.25, 0.1)), points[0]

# Posed: the bone turns and squashes what it carries.
doc.frame_current = 10
rig.bone_set(bone, angle=0.6, scale=(2.0, 0.7))
evaluated, _, _ = art.evaluated()
points, curves = art.shown()
for p, q in zip(points, evaluated):
    assert close(p, art.to_document(q)), ("shown is the evaluated drawing in document space", p, q)

# Place a point where it should show: it shows there, and the edit keys frame 10.
target = (0.3, -0.2)
before = points[1]
art.place_point(1, target)
points, curves = art.shown()
assert close(points[1], target, 1e-3), points[1]
assert not close(before, target, 1e-2)
doc.frame_current = 0  # a frame change records the edit as a key
setup = art.shown()[0][1]
assert not close(setup, target, 1e-2), "frame 0 keeps its setup"
doc.frame_current = 10
assert close(art.shown()[0][1], target, 1e-3), "keyed at frame 10"

# Place a handle's end: it shows there; an aligned point's other handle stays in line unless keep_other.
pos = art.shown()[0][1]
handle_target = (pos[0] + 0.04, pos[1] + 0.07)
art.place_handle(0, 1, "OUT", handle_target)
_, curves = art.shown()
point, hin, hout, aligned, hidden, level, index = curves[0][0][1]
assert point == 1 and close(hout, handle_target, 1e-3), hout
if aligned:
    a = (pos[0] - hin[0], pos[1] - hin[1])
    b = (hout[0] - pos[0], hout[1] - pos[1])
    assert abs(a[0] * b[1] - a[1] * b[0]) < 1e-4, "in line"
in_before = hin
art.place_handle(0, 1, "OUT", (pos[0] + 0.02, pos[1] - 0.09), keep_other=True)
_, curves = art.shown()
assert close(curves[0][0][1][1], in_before, 1e-4), "keep_other leaves the in handle"

for bad in (lambda: art.place_handle(0, 9, "OUT", (0, 0)), lambda: art.place_handle(3, 0, "OUT", (0, 0))):
    try:
        bad()
    except IndexError:
        pass
    else:
        raise AssertionError("no such curve point")
try:
    art.place_handle(0, 1, "UP", (0, 0))
except ValueError:
    pass
else:
    raise AssertionError("side must be IN or OUT")

# Detail levels: shown() as drawn has every point with the handles the render uses; shown(level=0) the
# coarse curve alone, with its own handles, each point's place in the whole curve beside it.
doc.frame_current = 0
assert oanim.ops.layer.new() == {'FINISHED'}
sq = layers.active
sq.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True, fill=True)
oanim.ops.mesh.select_all()
assert oanim.ops.mesh.detail_level_set(level="1") == {'FINISHED'}
d = sq.split_segment(0, 0, 0.5)
assert sq.detail["levels"][d] == 1


def bezier(p0, p1, p2, p3, t):
    u = 1 - t
    return tuple(u ** 3 * a + 3 * u * u * t * b + 3 * u * t * t * c + t ** 3 * e for a, b, c, e in zip(p0, p1, p2, p3))


def drawn_samples(points, cps, steps=8):
    out = []
    for k in range(len(cps)):
        a, b = cps[k], cps[(k + 1) % len(cps)]
        out += [bezier(points[a[0]], a[2], b[1], points[b[0]], t / steps) for t in range(steps)]
    return out


points, curves = sq.shown()
cps = curves[0][0]
assert [c[5] for c in cps] == [0, 1, 0, 0, 0] and [c[6] for c in cps] == [0, 1, 2, 3, 4], cps
outline = sq.shape_outline(0, 64)[0]
for p in drawn_samples(points, cps):
    assert min((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 for q in outline) < 1e-5, ("as drawn", p)
_, coarse = sq.shown(level=0)
coarse = coarse[0][0]
assert [c[0] for c in coarse] == [0, 1, 2, 3] and [c[6] for c in coarse] == [0, 2, 3, 4], coarse
assert not close(coarse[0][2], cps[0][2]), "the coarse handle facing the detail is longer than the drawn one"

# Editing at level 0 carries the detail along; at level 1 it stays where it is.
before = sq.shown()[0][d]
sq.set_detail_level(0)
assert sq.detail["shape_levels"] == [0] and sq.detail["hidden"][d]
sq.place_point(1, (1.5, 0.0))
sq.detail_sync()
moved = sq.shown()[0][d]
assert not close(moved, before), "the detail rides its coarse curve"
sq.set_detail_level(1)
sq.place_point(1, (1.2, 0.0))
sq.detail_sync()
assert close(sq.shown()[0][d], moved), "edited at its own level, the detail keeps its place"

print("shown and place: ok")
