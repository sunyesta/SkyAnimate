"""Envelope on a cage whose side has bent into a hook: an eye's cage from a real rig, its bottom point pulled up almost
level with the corners and the corners' handles still pointing down. Two stretches of that side are then about as near to
what lies inside. Each stretch has a say that fades, so a line inside stays smooth where the nearest stretch changes,
keeps its distance from the sides elsewhere, and doesn't jump as the cage moves.

Run with:  OpenAnimate --background --python tests/test_envelope_hook.py
"""
import math

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0

REST = [(0.14444, 0.52579), (0.08438, 0.58522), (0.02633, 0.52619), (0.08337, 0.46797)]
REST_H = [((0.14444, 0.49308), (0.14444, 0.5585)), ((0.11755, 0.58543), (0.05281, 0.58502)),
          ((0.02633, 0.5589), (0.02633, 0.49348)), ((0.0502, 0.46797), (0.11655, 0.46797))]
NOW = [(0.15738, 0.52546), (0.0894, 0.59443), (0.0227, 0.53262), (0.08536, 0.53121)]
NOW_H = [((0.15858, 0.47866), (0.15643, 0.56203)), ((0.12719, 0.59479), (0.05513, 0.59411)),
         ((0.02719, 0.56636), (0.01815, 0.4985)), ((0.05134, 0.52252), (0.1307, 0.54279))]
CENTER, RADIUS, N = (0.0805, 0.5245), 0.0535, 180
FRAME = 8


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


cage = new_layer("Cage")
cage.add_curve(REST, closed=True, fill=True)
for i, (handle_in, handle_out) in enumerate(REST_H):
    cage.move_handle(0, i, "IN", handle_in)
    cage.move_handle(0, i, "OUT", handle_out)
doc.frame_current = 1
doc.frame_current = 0
ring = new_layer("Ring")
ring.add_curve([(CENTER[0] + RADIUS * math.cos(k * 2 * math.pi / N), CENTER[1] + RADIUS * math.sin(k * 2 * math.pi / N))
                for k in range(N)], closed=True)
assert ops.object.modifier_add(type="CAGE_DEFORM") == {'FINISHED'}
ring.modifier_set(0, layer="Cage", shape=cage.shapes[0][0], cage_method="ENVELOPE", resample_self=False, precision=0.0)
doc.frame_current = 1
doc.frame_current = 0
rest = [tuple(p[:2]) for p in ring.evaluated()[0]]
assert len(rest) == N


def outline():
    return cage.evaluated_outline(0, 48)[0]


def seg_distance(p, line):
    best = 1e9
    for a, b in zip(line, line[1:] + line[:1]):
        dx, dy = b[0] - a[0], b[1] - a[1]
        dd = dx * dx + dy * dy
        t = 0.0 if dd < 1e-20 else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / dd))
        best = min(best, math.hypot(p[0] - a[0] - dx * t, p[1] - a[1] - dy * t))
    return best


def turns(points):
    """How sharply the line turns at each point, in degrees."""
    r = []
    for k, b in enumerate(points):
        a, c = points[k - 1], points[(k + 1) % len(points)]
        u, v = (b[0] - a[0], b[1] - a[1]), (c[0] - b[0], c[1] - b[1])
        r.append(abs(math.degrees(math.atan2(u[0] * v[1] - u[1] * v[0], u[0] * v[0] + u[1] * v[1]))))
    return r


rest_gap = [seg_distance(p, outline()) for p in rest]

# Bent as the file has it, on a frame of its own, keyed from the rest so the frames between move it too.
doc.frame_current = FRAME
for i, p in enumerate(NOW):
    cage.set_point(i, p)
for i, (handle_in, handle_out) in enumerate(NOW_H):
    cage.move_handle(0, i, "IN", handle_in)
    cage.move_handle(0, i, "OUT", handle_out)
doc.frame_current = FRAME + 1
doc.frame_current = FRAME
assert all(math.dist(p[:2], q) < 1e-4 for p, q in zip(cage.points, NOW)), cage.points

bent = [tuple(p[:2]) for p in ring.evaluated()[0]]
worst = max(turns(bent))
assert worst < 30.0, ("a kink in the line where the nearest stretch of the side changes", round(worst, 1),
                      turns(bent).index(worst) * 2)

# It still does Envelope's job: along the top, where the side hasn't hooked, the band keeps its width.
line = outline()
for k in range(N):
    angle = k * 2
    if 45 <= angle <= 135:
        gap = seg_distance(bent[k], line)
        assert abs(gap - rest_gap[k]) < 0.25 * rest_gap[k], ("the band keeps its width", angle, gap, rest_gap[k])

# Nothing inside the cage has been pushed out past it.
for k in range(0, N, 3):
    p = bent[k]
    assert CENTER[1] - 0.2 < p[1] < 0.6 and 0.0 < p[0] < 0.17, ("still inside", k * 2, p)

# On the way there nothing jumps: no frame moves a point much further than the frames around it.
doc.frame_current = 0
was = None
biggest = []
for frame in range(0, FRAME + 1):
    doc.frame_current = frame
    now = [tuple(p[:2]) for p in ring.evaluated()[0]]
    if was:
        biggest.append(max(math.dist(a, b) for a, b in zip(now, was)))
    was = now
middle = sorted(biggest)[len(biggest) // 2]
assert max(biggest) < 3.0 * max(middle, 1e-3), ("a jump between frames", [round(b, 4) for b in biggest])

# A round side isn't a hook: what lies well inside a round cage keeps to the side.
doc.frame_current = 0
round_cage = new_layer("Round")
assert round_cage.add_primitive("CIRCLE", [(-1, -1), (1, 1)])
art = new_layer("Art")
art.add_curve([(-0.5, 0.45), (0.0, 0.5), (0.5, 0.45)])
assert ops.object.modifier_add(type="CAGE_DEFORM") == {'FINISHED'}
art.modifier_set(0, layer="Round", shape=round_cage.shapes[0][0], cage_method="ENVELOPE", resample_self=False, precision=0.0)
doc.frame_current = 1
doc.frame_current = 0
at_rest = [tuple(p[:2]) for p in art.evaluated()[0]]
top = max(range(round_cage.point_count), key=lambda k: round_cage.points[k][1])
doc.frame_current = 4
round_cage.set_point(top, (round_cage.points[top][0], round_cage.points[top][1] + 0.4))
doc.frame_current = 5
doc.frame_current = 4
rise = art.evaluated()[0][1][1] - at_rest[1][1]
assert rise > 0.27, ("half a unit from the side, it goes most of the way with it", rise)

print("envelope hook: ok, sharpest turn", round(worst, 1))
