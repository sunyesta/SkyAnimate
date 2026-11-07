"""Sculpting where the points show on a posed frame (anim/sculpt.hh):

- inside a bone layer, Dyntopo's resample doesn't move the line: each new point is weighted between its neighbours and
  stored where its own skin (by those weights) shows it on the line;
- on a layer a Cage Deform bends, Grab puts a point under the mouse, as G does (each dab's points settled on until they
  show where the brush put them).

Run with:  OpenAnimate --background --python tests/test_sculpt_posed.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops


def path(*pts):
    return ";".join("%r,%r" % (float(p[0]), float(p[1])) for p in pts)


def bez(c, t):
    u = 1 - t
    return tuple(u * u * u * c[0][i] + 3 * u * u * t * c[1][i] + 3 * u * t * t * c[2][i] + t * t * t * c[3][i]
                 for i in range(2))


def shown_line(l, steps=32):
    pts, curves = l.shown()
    cps, closed = curves[0]
    out = []
    for s in range(len(cps) - 1):
        a, b = cps[s], cps[s + 1]
        c = (pts[a[0]], a[2], b[1], pts[b[0]])
        out += [bez(c, k / steps) for k in range(steps + (1 if s == len(cps) - 2 else 0))]
    return out


def off(p, poly):
    best = 1e9
    for a, b in zip(poly, poly[1:]):
        ax, ay = b[0] - a[0], b[1] - a[1]
        t = max(0.0, min(1.0, ((p[0] - a[0]) * ax + (p[1] - a[1]) * ay) / max(ax * ax + ay * ay, 1e-24)))
        best = min(best, math.dist(p, (a[0] + ax * t, a[1] + ay * t)))
    return best


def select_only(l):
    for other in layers.all():
        other.selected = other.id == l.id
    layers.active = l


# --- Dyntopo inside a bone layer, on a posed frame -------------------------------------------------------------------
# A line weighted to bone a at its left end and b at its right, b turned on frame 5: a tiny Grab with Subdivide leaves
# the shown line where it was, but for the 0.0001 itself (a new point stored by an end's skin, or by one between its
# neighbours' skins, would show off the line once its own blended weights pose it).
doc.auto_key = False
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="b", head_x="3", head_y="0", tail_x="4", tail_y="0", parent="")
ops.layer.new()
aid = layers.active.id
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
art = next(l for l in layers.all() if l.id == aid)
art.key_whole_drawing = False
art.add_curve([(0.5, 0.0), (3.5, 0.0)])
layers.active = art
# Weights that don't add up to 1 (each point's are shared out to 1 when posed), so a new point's skin by its own blended
# weights is not the one between its neighbours' skins.
for i, bone, weight in ((0, "a", "0.3"), (1, "b", "1")):
    ops.mesh.select_none()
    art.select_points([i])
    rig.select_bone(bone)
    assert ops.weight.assign(weight=weight) == {'FINISHED'}
ops.mesh.select_none()
doc.frame_current = 5
rig.bone_set("b", angle=0.8)
doc.frame_current = 6
doc.frame_current = 5
select_only(art)
before = shown_line(art, 2000)
mid = before[len(before) // 2]
assert ops.sculpt.stroke(brush="GRAB", path=path(mid, (mid[0], mid[1] + 1e-4)), radius=0.6, strength=1.0,
                         falloff="SMOOTH", mirror="0", resolution="20", refine="SUBDIVIDE") == {'FINISHED'}
assert len(art.points) > 10, len(art.points)
after = shown_line(art, 40)
moved = max(max(off(p, after) for p in before), max(off(p, before) for p in after))
print("bones: %d points, the shown line moved %.6f" % (len(art.points), moved))
assert moved < 2e-4, ("Dyntopo's resample on a posed frame moved the shown line", moved)

# --- Cage Deform on a posed frame: Grab lands the point under the mouse ------------------------------------------------
ops.wm.read_homefile(confirmed="1")
doc.auto_key = True
doc.frame_current = 0
ops.layer.new()
cage = layers.active
cage.name = "Cage"
assert cage.add_primitive("BOX", [(-1, -1), (1, 1)])
ops.layer.new()
art = layers.active
art.name = "Art"
art.add_curve([(-0.55, -0.3), (-0.2, 0.35), (0.25, -0.15), (0.6, 0.3)])
assert ops.object.modifier_add(type="CAGE_DEFORM") == {'FINISHED'}
art.modifier_set(len(art.modifiers) - 1, layer=cage.name, shape=cage.shapes[0][0], cage_method="MEAN_VALUE")
doc.frame_current = 10
for i, p in enumerate(cage.points):
    cage.set_point(i, (p[0] * 1.3 + 0.2 * p[1] + 0.1, p[1] * 0.9 + (0.12 if p[0] > 0.5 else 0.0)))
doc.frame_current = 11
doc.frame_current = 10
select_only(art)
ops.mesh.select_none()
for i, d in ((1, (0.06, 0.1)), (2, (-0.12, 0.04))):
    p0 = art.shown()[0][i]
    assert ops.sculpt.stroke(brush="GRAB", path=path(p0, (p0[0] + d[0], p0[1] + d[1])), radius=0.02, strength=1.0,
                             falloff="CONSTANT", mirror="0") == {'FINISHED'}
    got = art.shown()[0][i]
    miss = math.dist(got, (p0[0] + d[0], p0[1] + d[1]))
    print("cage: point %d lands %.7f off the mouse (moved %.3f)" % (i, miss, math.hypot(*d)))
    # (settled, 1e-7; by the stretch where it started alone, about 4e-4 for these moves)
    assert miss < 1e-5, ("Grab on a bent layer left the point off the mouse", i, miss)

print("Sculpt posed: ok")
