"""Dyntopo's Decimate (Collapse after a stroke) and Simplify (anim/sculpt.hh), and what the resample counts as moved:

- a brush far from the line moves none of it, so Dyntopo resamples and decimates nothing (Twist turned by nothing about
  a far centre still rounds);
- Decimate keeps the line's width as well as its place: a Thickness bump stays, after a stroke or Simplify;
- on a layer squashed and turned (its own or a group's), the line Decimate leaves stays within its tolerance of the line
  as it was: the spans are fitted where the line shows;
- Decimate, Simplify and the resample take about as long per point however many there are (not the square of it).

Run with:  OpenAnimate --background --python tests/test_sculpt_collapse.py
"""

import math
import time

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def path(*pts):
    return ";".join("%r,%r" % (float(p[0]), float(p[1])) for p in pts)


def line(pts):
    ops.layer.new()
    l = layers.active
    l.key_whole_drawing = False
    for other in layers.all():
        other.selected = other.id == l.id
    layers.active = l
    l.add_curve(pts)
    ops.mesh.select_none()
    return l


def bez(c, t):
    u = 1 - t
    return tuple(u * u * u * c[0][i] + 3 * u * u * t * c[1][i] + 3 * u * t * t * c[2][i] + t * t * t * c[3][i]
                 for i in range(2))


def dense(l, h=0.004):
    """The line as it shows (document space), sampled about every h."""
    pts, curves = l.shown()
    cps, closed = curves[0]
    out = []
    for s in range(len(cps) - 1):
        a, b = cps[s], cps[s + 1]
        c = (pts[a[0]], a[2], b[1], pts[b[0]])
        L = math.dist(c[0], c[1]) + math.dist(c[1], c[2]) + math.dist(c[2], c[3])
        k = max(2, int(math.ceil(L / h)))
        out += [bez(c, i / k) for i in range(k)]
    out.append(pts[cps[-1][0]])
    return out


def seg_dist(p, a, b):
    ax, ay = b[0] - a[0], b[1] - a[1]
    t = max(0.0, min(1.0, ((p[0] - a[0]) * ax + (p[1] - a[1]) * ay) / max(ax * ax + ay * ay, 1e-24)))
    return math.dist(p, (a[0] + ax * t, a[1] + ay * t))


def grid_off(pa, pb, cell=0.05):
    """How far the furthest of pa is from the polyline pb."""
    g = {}
    for a, b in zip(pb, pb[1:]):
        for q in (a, b):
            g.setdefault((int(q[0] // cell), int(q[1] // cell)), []).append((a, b))
    worst = 0.0
    for p in pa:
        cx, cy = int(p[0] // cell), int(p[1] // cell)
        best = 1e9
        for r in range(0, 8):
            for dx in range(-r, r + 1):
                for dy in range(-r, r + 1):
                    if max(abs(dx), abs(dy)) == r:
                        for a, b in g.get((cx + dx, cy + dy), ()):
                            best = min(best, seg_dist(p, a, b))
            if best < r * cell:
                break
        worst = max(worst, best)
    return worst


def apart(a, b):
    return max(grid_off(a, b), grid_off(b, a))


doc.auto_key = False

# --- A brush far from the line moves none of it ------------------------------------------------------------------------
for brush in ("TWIST", "PINCH", "PUSH"):
    for decimate in ("0", "1"):
        l = line([(x * 0.5, 0.0) for x in range(9)])
        ops.sculpt.stroke(brush=brush, path=path((20.0, 5.0), (20.0, 5.1)), radius=1.0, strength=1.0, mirror="0",
                          resolution="40", decimate=decimate)
        assert len(l.points) == 9, ("a brush 20 units off changed the line's points", brush, decimate, len(l.points))

# --- Decimate keeps the width -------------------------------------------------------------------------------------------


def width_at(l, x):
    pts = sorted((p[0], r) for p, r in zip(l.points, l.radii))
    for (x0, r0), (x1, r1) in zip(pts, pts[1:]):
        if x0 <= x <= x1:
            return r0 + (r1 - r0) * (x - x0) / max(x1 - x0, 1e-9)
    return None


thick = dict(brush="THICKNESS", path=path(*[(2.0, 0.0)] * 6), radius=0.6, strength=1.0, mirror="0")
plain = line([(x * 0.25, 0.0) for x in range(17)])
assert ops.sculpt.stroke(**thick) == {'FINISHED'}
peak, side = width_at(plain, 2.0), width_at(plain, 0.5)
assert peak > side + 1.0, (peak, side)
bump = peak - side
profile = [width_at(plain, x * 0.05) for x in range(81)]


def keeps_width(l, what, fewer=True):
    got = [width_at(l, x * 0.05) for x in range(81)]
    worst = max(abs(a - b) for a, b in zip(got, profile))
    print("%s: %d points, the width strays %.4f (bump %.3f)" % (what, len(l.points), worst, bump))
    assert worst < 0.1 * bump, (what, "the width bump flattened", worst, bump)
    assert not fewer or len(l.points) < 17, (what, "the flat stretches still go", len(l.points))


dyn = line([(x * 0.25, 0.0) for x in range(17)])
assert ops.sculpt.stroke(resolution="12", decimate="1", **thick) == {'FINISHED'}
# (Thickness moves no point, so it touches none for Decimate to take out)
keeps_width(dyn, "Dyntopo Thickness", fewer=False)
simp = line([(x * 0.25, 0.0) for x in range(17)])
assert ops.sculpt.stroke(**thick) == {'FINISHED'}
assert ops.sculpt.simplify(resolution="12") == {'FINISHED'}
keeps_width(simp, "Simplify")
push = line([(x * 0.25, 0.0) for x in range(17)])
assert ops.sculpt.stroke(**thick) == {'FINISHED'}
assert ops.sculpt.stroke(brush="PUSH", path=path((2.0, 0.0), (2.0, 0.0005)), radius=3.0, strength=1.0, mirror="0",
                         resolution="12", decimate="1") == {'FINISHED'}
keeps_width(push, "Push with Decimate")

# --- Squashed and turned: Decimate stays within its tolerance of the line ----------------------------------------------
PTS = [(-1.5, 0.0), (-0.5, 0.3), (0.5, -0.2), (1.5, 0.1), (2.0, 0.8)]
RES = 40.0


def squashed(decimate, place, simplify=False):
    ops.wm.read_homefile(confirmed="1")
    doc.auto_key = False
    l = line(PTS)
    place(l)
    poly = dense(l)
    m = poly[len(poly) // 3]
    assert ops.sculpt.stroke(brush="TWIST", path=path(*[m] * 8), radius=0.5, strength=0.6, falloff="SMOOTH",
                             mirror="0", resolution=str(RES), decimate=decimate) == {'FINISHED'}
    before = dense(l)
    if simplify:
        assert ops.sculpt.simplify(resolution=str(RES)) == {'FINISHED'}
    cx, cy = wm.sculpt_dyntopo_cell(l.id, RES)
    return before, dense(l), 0.1 * math.sqrt(abs(cx[0] * cy[1] - cx[1] * cy[0])), len(l.points)


def own_scale(l):
    l.scale_x, l.scale_y = 3.0, 0.4


def in_group(l):
    lid = l.id
    ops.layer.new_group()
    g = layers.active
    g.scale_x, g.scale_y, g.rotation = 1.5, 0.8, 0.6
    layers.active = next(x for x in layers.all() if x.id == lid)
    assert ops.layer.move_to_group(group=str(g.id)) == {'FINISHED'}
    l = next(x for x in layers.all() if x.id == lid)
    for other in layers.all():
        other.selected = other.id == lid
    layers.active = l


for name, place in (("scaled 3 x 0.4", own_scale), ("in a group scaled 1.5 x 0.8 and turned", in_group)):
    sub, _, tol, many = squashed("0", place)
    _, both, _, few = squashed("1", place)
    dev = apart(sub, both)
    print("%s: %d points, decimated %d, strays %.5f (tolerance %.5f)" % (name, many, few, dev, tol))
    assert few < many and dev < 1.5 * tol, (name, "Decimate strayed", dev, tol)
    before, after, tol, left = squashed("0", place, simplify=True)
    dev = apart(before, after)
    print("%s: Simplify to %d points, strays %.5f" % (name, left, dev))
    assert left < many and dev < 1.5 * tol, (name, "Simplify strayed", dev, tol)

# --- About as long per point however many: the time for 8 times the points is nowhere near 64 times -------------------


def best_of(fn, runs=2):
    return min(fn() for _ in range(runs))


def simplify_time(n):
    def run():
        ops.wm.read_homefile(confirmed="1")
        doc.auto_key = False
        l = line([(x * 4.0 / n, 0.0) for x in range(n + 1)])
        t0 = time.perf_counter()
        assert ops.sculpt.simplify(resolution="50") == {'FINISHED'}
        took = time.perf_counter() - t0
        assert len(l.points) == 2, len(l.points)
        return took
    return best_of(run)


def stroke_time(radius, decimate):
    """A Grab on a line of one segment, all of it resampled at the brush's spacing: a brush 8 times smaller makes 8
    times the points."""
    def run():
        ops.wm.read_homefile(confirmed="1")
        doc.auto_key = False
        l = line([(0.0, 0.0), (4.0, 0.0)])
        t0 = time.perf_counter()
        ops.sculpt.stroke(brush="GRAB", path=path(*[(2.0, 0.002 * i) for i in range(20)]), radius=radius,
                          strength=1.0, mirror="0", resolution="1000", decimate=decimate)
        return time.perf_counter() - t0
    return best_of(run)


for what, small, big in (("Simplify", lambda: simplify_time(250), lambda: simplify_time(2000)),
                         ("Resample", lambda: stroke_time(0.6, "0"), lambda: stroke_time(0.075, "0")),
                         ("Resample and Decimate", lambda: stroke_time(0.6, "1"), lambda: stroke_time(0.075, "1"))):
    a, b = small(), big()
    print("%s: %.0f ms, 8 times the points %.0f ms" % (what, a * 1000, b * 1000))
    assert b < 24.0 * a + 0.5, (what, "far worse than linear in the points", a, b)

print("Sculpt Collapse: ok")
