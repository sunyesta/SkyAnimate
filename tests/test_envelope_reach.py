"""How far a cage's move reaches. Across a gap in the cage it reaches nothing: the two sides of a narrow gap are kept apart
in Rigid's lattice, and an Envelope's side has no say over what it can't get to through the cage. Range limits it on purpose:
what lies further from a side that has moved than Range stays with the cage as a whole, however the whole cage moves, and
Range grows with the side's move so nothing folds.

Run with:  OpenAnimate --background --python tests/test_envelope_reach.py
"""
import math
import os
import shutil
import tempfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0
made = 0


def new_layer(name):
    global made
    made += 1
    ops.layer.new()
    layer = layers.active
    layer.name = f"{name} {made}"
    return layer


def settle(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def bent_by(cage_points, art_points, method, **settings):
    doc.frame_current = 0
    cage = new_layer("Cage")
    cage.add_curve(cage_points, closed=True, fill=True)
    settle(0)
    art = new_layer("Art")
    art.add_curve(art_points)
    assert ops.object.modifier_add(type="CAGE_DEFORM") == {'FINISHED'}
    art.modifier_set(0, layer=cage.name, shape=cage.shapes[0][0], cage_method=method, resample_self=False, precision=0.0,
                     **settings)
    settle(0)
    return cage, art


def places(art):
    return [tuple(p[:2]) for p in art.evaluated()[0]]


def move_cage(cage, move):
    """Each point of the cage goes where `move` says, on a frame of its own."""
    doc.frame_current = 4
    for k, p in enumerate(list(cage.points)):
        q = move(k, p[0], p[1])
        if q:
            cage.set_point(k, q)
    settle(4)


def move_whole(cage, whole):
    """The cage as a whole, its handles too."""
    handles = cage.evaluated_handles(0)
    points = list(cage.points)
    doc.frame_current = 4
    for k, p in enumerate(points):
        cage.set_point(k, whole(p[0], p[1]))
    for k, (_, handle_in, handle_out) in enumerate(handles):
        cage.move_handle(0, k, "IN", whole(*handle_in))
        cage.move_handle(0, k, "OUT", whole(*handle_out))
    settle(4)


def u_shape(gap, at=0.0):
    # Points close by each corner of the gap, or its smooth curves would bow into it (and at 0.02 wide, shut it).
    g = gap / 2
    c = 0.02
    return [(-1.0, 1.0), (-1.0, 0.5), (-1.0, 0.0), (-1.0, -0.5), (-1.0, -1.0), (0.0, -1.0), (1.0, -1.0), (1.0, -0.5),
            (1.0, 0.0), (1.0, 0.5), (1.0, 1.0), (0.6, 1.0), (at + g + c, 1.0), (at + g, 1.0 - c), (at + g, 0.5), (at + g, 0.0),
            (at + g, -0.4 + c), (at + g - c, -0.4), (at - g + c, -0.4), (at - g, -0.4 + c), (at - g, 0.0), (at - g, 0.5),
            (at - g, 1.0 - c), (at - g - c, 1.0), (-0.6, 1.0)]


MOVE = 0.4

# Pulling the top of a U's left arm leaves the right arm alone, however narrow the gap between them, and the left arm
# goes with it. (Square cells joined the two sides of a gap under a few of them wide; mean value coordinates threw what
# lies by a narrow one; an Envelope's sides reached straight over.)
for gap in (0.02, 0.05, 0.1, 0.2, 0.4):
    for method in ("RIGID", "ENVELOPE"):
        for way in ((-MOVE, 0.0), (0.0, MOVE)):
            shape = u_shape(gap)
            # Some right by the still wall, wherever it falls in the lattice's cells.
            right = [(gap / 2 + 0.1, y) for y in (0.9, 0.6, 0.3, 0.0)]
            right += [(gap / 2 + 0.004 * k, 0.7 + 0.001 * k) for k in range(1, 20)]
            left = [(-gap / 2 - 0.1, 0.9), (-gap / 2 - 0.1, 0.7)]
            # Close by the moved wall: right up against the gap, where each side has to read its own nodes.
            hugging = [(-gap / 2 - 0.015, 0.8), (-gap / 2 - 0.015, 0.7)]
            cage, art = bent_by(shape, right + left + hugging, method)
            rest = places(art)
            move_cage(cage, lambda k, x, y: (x + way[0], y + way[1]) if shape[k][0] < 0.0 and shape[k][1] > 0.4 else None)
            now = places(art)
            moved = [math.dist(a, b) for a, b in zip(rest, now)]
            far, near, by_wall = moved[:len(right)], moved[len(right):-2], moved[-2:]
            assert max(far) < MOVE * 0.015, ("the move reached across the gap", gap, method, way, far)
            assert min(near) > MOVE * 0.6, ("the arm it moved goes with it", gap, method, way, near)
            assert min(by_wall) > MOVE * 0.75, ("what lies by the wall goes with it", gap, method, way, by_wall)
            doc.frame_current = 0

# A gap can fall anywhere in the lattice's cells: what lies by its wall still goes by its own side alone.
for step in range(12):
    at = -0.06 + step * 0.0055
    gap = 0.03
    shape = u_shape(gap, at)
    cage, art = bent_by(shape, [(at - gap / 2 - 0.002, 0.7), (at - gap / 2 - 0.004, 0.72), (at - gap / 2 - 0.002, 0.75)],
                        "RIGID")
    rest = places(art)
    move_cage(cage, lambda k, x, y: (x + MOVE, y) if shape[k][0] > at and shape[k][1] > 0.4 else None)
    moved = max(math.dist(a, b) for a, b in zip(rest, places(art)))
    assert moved < MOVE * 0.02, ("went by the far side of the gap", at, moved)
    doc.frame_current = 0

# A cage with a narrow gap moved whole carries everything whole, in the gap and past the outline too.
shape = u_shape(0.05)
art_at = [(0.1, 0.5), (-0.1, 0.5), (0.0, 0.5), (0.0, 0.0 - 0.6), (1.4, 0.0)]
for method in ("RIGID", "ENVELOPE"):
    cage, art = bent_by(shape, art_at, method)
    rest = places(art)
    turn = 0.5
    c, s = math.cos(turn), math.sin(turn)
    whole = lambda x, y: (x * c - y * s + 0.3, x * s + y * c - 0.2)
    move_whole(cage, whole)
    for a, b in zip(rest, places(art)):
        assert math.dist(whole(*a), b) < 0.004, ("moved whole", method, a, b, whole(*a))
    doc.frame_current = 0

# A cage with no dent isn't touched by any of it: Range at its top is what there was, and is the default.
N = 24
circle = [(math.cos(k * 2 * math.pi / N), math.sin(k * 2 * math.pi / N)) for k in range(N)]
column = [(0.0, y / 20) for y in (-19, -16, -12, -6, 0, 6, 12, 16)]
top = [k for k, p in enumerate(circle) if p[1] > 0.7]
push = lambda k, x, y: (x, y + MOVE) if k in top else None

cage, art = bent_by(circle, column, "ENVELOPE")
assert art.modifiers[0]["range"] == 1.0, art.modifiers[0]
rest = places(art)
move_cage(cage, push)
full = places(art)
assert full[4][1] - rest[4][1] > 0.3 * MOVE, ("with no Range the middle follows the side", full[4], rest[4])
doc.frame_current = 0

# Just under the top of Range it hardly differs.
cage, art = bent_by(circle, column, "ENVELOPE", range=0.995)
move_cage(cage, push)
for a, b in zip(full, places(art)):
    assert math.dist(a, b) < 0.03 * MOVE, ("Range jumps near its top", a, b)
doc.frame_current = 0

# Range: what lies further than it from the side that moved stays, what lies by that side goes with it.
cage, art = bent_by(circle, column, "ENVELOPE", range=0.3)
rest = places(art)
move_cage(cage, push)
now = places(art)
for k in range(5):
    assert math.dist(rest[k], now[k]) < 0.01 * MOVE, ("past Range it stays", rest[k], now[k])
assert now[7][1] - rest[7][1] > 0.75 * MOVE, ("by the side it goes with it", rest[7], now[7])
assert all(a[1] < b[1] for a, b in zip(now, now[1:])), ("nothing folds", now)
doc.frame_current = 0

# Pushed in further than Range, what lies by the side gets out of the way of what stays instead of folding through it.
cage, art = bent_by(circle, column, "ENVELOPE", range=0.05)
move_cage(cage, lambda k, x, y: (x, y - 1.1) if k in top else None)
now = places(art)
assert all(a[1] < b[1] for a, b in zip(now, now[1:])), ("nothing folds when a side passes Range", now)
assert now[-1][1] < 1.0 - 1.1, ("the side went through what it pushed", now[-1])
doc.frame_current = 0

# Moving the whole cage still carries everything: Range limits bending, not the cage's own move.
cage, art = bent_by(circle, column, "ENVELOPE", range=0.2)
rest = places(art)
turn = 0.7
c, s = math.cos(turn), math.sin(turn)
whole = lambda x, y: (x * c - y * s + 0.5, x * s + y * c - 0.3)
move_whole(cage, whole)
for a, b in zip(rest, places(art)):
    assert math.dist(whole(*a), b) < 0.004, ("Range left the middle behind", a, b, whole(*a))
doc.frame_current = 0

# Scaled whole too, the middle goes with it (near a side Envelope keeps a line's width, Range or no Range).
cage, art = bent_by(circle, column, "ENVELOPE", range=0.2)
rest = places(art)
whole = lambda x, y: (x * 1.3 + 0.5, y * 1.3 - 0.3)
move_whole(cage, whole)
middle = places(art)[4]
assert math.dist(whole(*rest[4]), middle) < 0.01, ("Range left the middle behind", rest[4], middle, whole(*rest[4]))
doc.frame_current = 0

# Moved whole with one side pushed on top: the far side goes with the cage, and takes nothing from the push.
cage, art = bent_by(circle, column, "ENVELOPE", range=0.3)
rest = places(art)
move_cage(cage, lambda k, x, y: (lambda q: (q[0], q[1] + MOVE) if k in top else q)((x + 0.5, y)))
now = places(art)
for k in range(5):
    assert math.dist((rest[k][0] + 0.5, rest[k][1]), now[k]) < 0.01 * MOVE, ("past Range", rest[k], now[k])
doc.frame_current = 0

# Range doesn't go over a gap: the other arm of a U stays, though it is well within Range straight across.
shape = u_shape(0.2)
cage, art = bent_by(shape, [(0.2, 0.7), (0.3, 0.7), (-0.3, 0.8), (-0.3, 0.7)], "ENVELOPE", range=0.4)
rest = places(art)
move_cage(cage, lambda k, x, y: (x - MOVE, y) if shape[k][0] < 0.0 and shape[k][1] > 0.4 else None)
now = places(art)
assert max(math.dist(a, b) for a, b in zip(rest[:2], now[:2])) < 0.01 * MOVE, ("Range went over the gap", rest, now)
assert min(math.dist(a, b) for a, b in zip(rest[2:], now[2:])) > 0.6 * MOVE, ("the arm goes with it", rest, now)

# What the viewport shows: within the cage, never over its gap, full at the outline and none at the limit.
outline = cage.evaluated_outline(0, 16)[0]
doc.frame_current = 0
outline = cage.evaluated_outline(0, 16)[0]


def inside(p):
    crossings = 0
    for a, b in zip(outline, outline[1:] + outline[:1]):
        if (a[1] > p[1]) != (b[1] > p[1]) and p[0] < a[0] + (p[1] - a[1]) / (b[1] - a[1]) * (b[0] - a[0]):
            crossings += 1
    return crossings % 2 == 1


art.modifier_set(0, range=0.05)
view = art.deform_range_view(0)
assert view is not None
at, quads, limits = view
assert len(at) > 100 and quads > 100 and limits > 10, (len(at), quads, limits)
assert max(p[2] for p in at) == 1.0 and min(p[2] for p in at) < 0.05, at[:5]
assert all(inside(p) for p in at if p[2] < 0.97), "the view left the cage"

circle_cage, circle_art = bent_by(circle, column, "RIGID")
assert circle_art.deform_range_view(0) is None

# Range is saved with the file.
folder = tempfile.mkdtemp()
try:
    path = os.path.join(folder, "reach.oanim")
    art.modifier_set(0, range=0.37)
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
    kept = next(l for l in oanim.data.layers.all() if l.name == art.name)
    assert abs(kept.modifiers[0]["range"] - 0.37) < 1e-6, kept.modifiers[0]
finally:
    shutil.rmtree(folder)

print("envelope reach: ok")
