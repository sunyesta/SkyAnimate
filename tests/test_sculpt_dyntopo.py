"""Dyntopo for the Sculpt tool (anim/sculpt.hh): a stroke adds points where the line can't follow the shape sculpted
(Subdivide) and, once over, takes out the points the line keeps its shape without (Collapse); Simplify does the
second on its own. Only on drawings without animation, and the stroke makes no keys.

Run with:  OpenAnimate --background --python tests/test_sculpt_dyntopo.py
"""

import math
import time

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def path(*pts):
    return ";".join("%r,%r" % p for p in pts)


def line(pts):
    ops.layer.new()
    l = layers.active
    l.key_whole_drawing = False
    for other in layers:
        other.selected = other.id == l.id
    layers.active = l
    l.add_curve(pts)
    ops.mesh.select_none()
    return l


def outline(l):
    return l.shape_outline(0, 48)[0]


def off(p, poly):
    """How far p is from the polyline."""
    best = 1e9
    for a, b in zip(poly, poly[1:]):
        ax, ay = b[0] - a[0], b[1] - a[1]
        t = max(0.0, min(1.0, ((p[0] - a[0]) * ax + (p[1] - a[1]) * ay) / max(ax * ax + ay * ay, 1e-12)))
        best = min(best, math.dist(p, (a[0] + ax * t, a[1] + ay * t)))
    return best


def bump(l, **kw):
    """A narrow Grab up from the middle of the line: the line has to rise to 1 there and stay flat a little way off."""
    return ops.sculpt.stroke(brush="GRAB", path=path((2.0, 0.0), (2.0, 0.5), (2.0, 1.0)), radius=0.5, strength=1.0,
                             falloff="SMOOTH", mirror="0", **kw)


doc.auto_key = False
ops.wm.tool_sculpt()
s = wm.sculpt_settings()
assert s["dyntopo"] is False and s["refine"] == "SUBDIVIDE_COLLAPSE" and s["detail_size"] > 0, s
wm.set_sculpt(dyntopo=True, detail_size=3.0, refine="COLLAPSE")
s = wm.sculpt_settings()
assert s["dyntopo"] and s["refine"] == "COLLAPSE" and abs(s["detail_size"] - 3.0) < 1e-6, s
wm.set_sculpt(dyntopo=False, refine="SUBDIVIDE_COLLAPSE")

# --- Without it, two points can't make a narrow bump ---------------------------------------------------------------
plain = line([(0.0, 0.0), (4.0, 0.0)])
assert bump(plain) == {'FINISHED'}
assert len(plain.points) == 2, "no Dyntopo: no points added"
o = outline(plain)
plain_peak = max(y for _, y in o)
plain_side = max(abs(y) for x, y in o if abs(x - 2.0) > 0.7)
assert plain_peak < 0.9 or plain_side > 0.05, ("a single segment can't", plain_peak, plain_side)

# --- Subdivide: points go in where the line needs them, and it follows the bump ----------------------------------------
detail = 0.01
sub = line([(0.0, 0.0), (4.0, 0.0)])
assert bump(sub, detail=str(detail), refine="SUBDIVIDE") == {'FINISHED'}
assert len(sub.points) > 2, len(sub.points)
o = outline(sub)
assert max(y for _, y in o) > 0.95, ("up to the brush", max(y for _, y in o))
assert max(abs(y) for x, y in o if abs(x - 2.0) > 0.7) < 0.03, "flat outside the brush"
ends = [p for p in sub.points if abs(p[1]) < 1e-6 and p[0] in (0.0, 4.0)]
assert len(ends) == 2, ("its ends stay", list(sub.points))
subdivided = len(sub.points)
sub_outline = o

# --- Subdivide Collapse: the same shape with fewer points ---------------------------------------------------------
both = line([(0.0, 0.0), (4.0, 0.0)])
assert bump(both, detail=str(detail)) == {'FINISHED'}
assert 2 < len(both.points) < subdivided, (len(both.points), subdivided)
for p in outline(both):
    # (the outline's chords between samples cut a bend a touch)
    assert off(p, sub_outline) < detail * 3, ("the same shape", p, off(p, sub_outline))
# One undo step, back to two points.
ops.ed.undo()
assert len(both.points) == 2, len(both.points)

# --- Collapse alone: nothing added, and a line of many points along the way loses those it doesn't need --------------
many = line([(x * 0.25, 0.0) for x in range(17)])
assert ops.sculpt.stroke(brush="PUSH", path=path((2.0, 0.0), (2.0, 0.001)), radius=3.0, strength=0.0, mirror="0",
                         detail=str(detail), refine="COLLAPSE") in ({'FINISHED'}, {'CANCELLED'})
# Push with no strength moves nothing, so nothing was touched: all 17 stay.
assert len(many.points) == 17

# --- Simplify: a straight line of 17 points is two; a bend keeps what it needs ----------------------------------------
assert ops.sculpt.simplify(detail=str(detail)) == {'FINISHED'}
assert len(many.points) == 2 and sorted(p[0] for p in many.points) == [0.0, 4.0], list(many.points)
arc = line([(math.cos(a / 16 * math.pi) * 2.0, math.sin(a / 16 * math.pi) * 2.0) for a in range(17)])
before = outline(arc)
assert ops.sculpt.simplify(detail=str(detail)) == {'FINISHED'}
assert 2 < len(arc.points) < 17, len(arc.points)
assert all(off(p, before) < detail * 3 for p in outline(arc)), "the arc kept"
assert ops.sculpt.simplify(detail="0") == {'CANCELLED'}

# A real stroke in the middle: the points it reached that the line is straight without go; those it didn't stay.
reached = line([(x * 0.25, 0.0) for x in range(17)])
assert ops.sculpt.stroke(brush="PUSH", path=path((2.0, 0.0), (2.0, 0.0005)), radius=0.6, strength=1.0, mirror="0",
                         detail=str(detail), refine="COLLAPSE") == {'FINISHED'}
xs = sorted(round(p[0], 3) for p in reached.points)
assert 2 < len(xs) < 17 and 0.25 in xs and 3.75 in xs and 2.0 not in xs, xs
ops.ed.undo()
assert len(reached.points) == 17

# --- Never on a drawing with animation, nor with X Mirror -----------------------------------------------------------
keyed = line([(0.0, 0.0), (4.0, 0.0)])
assert wm.sculpt_dyntopo_blocked(keyed.id) == ""
keyed.keyframe_insert("points[0].pos", 10)
why = wm.sculpt_dyntopo_blocked(keyed.id)
assert why == "its points are keyed", why
assert bump(keyed, detail=str(detail)) == {'FINISHED'}
assert len(keyed.points) == 2, "keyed: no points added"
ops.ed.undo()
# X Mirror pairs points by where they are: not with Dyntopo (yet).
mirrored = line([(0.0, 0.0), (4.0, 0.0)])
assert bump(mirrored, detail=str(detail)) == {'FINISHED'} and len(mirrored.points) > 2
ops.ed.undo()
assert ops.sculpt.stroke(brush="GRAB", path=path((2.0, 0.0), (2.0, 0.5), (2.0, 1.0)), radius=0.5, strength=1.0,
                         mirror="1", detail=str(detail)) == {'FINISHED'}
assert len(mirrored.points) == 2, "X Mirror: no points added"
fresh = line([(0.0, 0.0), (4.0, 0.0)])

# --- On another frame with auto-key: the new shape is the drawing's own, no keys --------------------------------------
doc.frame_current = 12
doc.auto_key = True
assert bump(fresh, detail=str(detail)) == {'FINISHED'}
added = len(fresh.points)
assert added > 2
assert wm.sculpt_dyntopo_blocked(fresh.id) == "", ("no keys made", wm.sculpt_dyntopo_blocked(fresh.id))
shape_now = outline(fresh)
doc.frame_current = 0
doc.auto_key = False
assert all(off(p, shape_now) < 1e-4 for p in outline(fresh)), "frame 0 shows it too: it is the setup"

# --- How long a dab takes: about as long with Dyntopo as without --------------------------------------------------------
long_path = path(*[(1.0 + i * 0.05, math.sin(i * 0.3) * 0.4) for i in range(60)])


def timed(**kw):
    l = line([(x * 0.5, 0.0) for x in range(9)])
    t0 = time.perf_counter()
    ops.sculpt.stroke(brush="PUSH", path=long_path, radius=0.4, strength=1.0, mirror="0", **kw)
    return time.perf_counter() - t0, len(l.points)


without, _ = timed()
with_it, count = timed(detail=str(detail))
print("Dyntopo stroke: %.1f ms without, %.1f ms with (%d points)" % (without * 1000, with_it * 1000, count))
assert with_it < without * 4 + 0.05, (without, with_it)

print("Sculpt Dyntopo: ok")
