"""Symmetrize's Match Current Shape: off frame 0, sides and pairs come from where the points show now, through
their bones, and each copy lands on the exact mirror image of its partner; points near the axis go onto it; Axis
picks the layer's line or the middle of the selection. It keys this frame only: no other frame, weight or shape's
look changes. Sided pairs (Ear_L, Ear_R) do the same across the pair.

Run with:  OpenAnimate --background --python tests/test_symmetrize_match.py
"""

import oanim

ops, doc, layers, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm
doc.auto_key = True


def close(a, b, eps=1e-4):
    if isinstance(a, (tuple, list)):
        return all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def where(layer, i):
    return tuple(layer.points[i][:2])


def line(points, name="Line", closed=False, fill=False):
    """A new layer holding one curve."""
    ops.layer.new()
    layer = layers.active
    layer.name = name
    layer.add_curve(points, closed=closed, fill=fill)
    return layer


def pose(layer, frame, moved):
    """`moved` {point: place} on `frame`, keyed, and left on that frame with the points selected."""
    layers.active = layer
    doc.frame_current = frame
    for i, at in moved.items():
        layer.set_point(i, at)
    doc.frame_current = frame + 1
    doc.frame_current = frame
    layer.select_points(range(layer.point_count), False)


def everything(layer):
    layer.select_points(range(layer.point_count), True)


# --- The points that have crossed: what is on the right now is what's copied --------------------------------
SETUP = [(2.0, 0.0), (1.0, 1.0), (-1.0, 1.0), (-2.0, 0.0)]
AT_10 = {0: (2.2, 0.1), 1: (-1.2, 0.5), 2: (1.5, 0.8), 3: (-2.0, 0.0)}

# Turned off the setup says: 0 and 1 are the right side, so 2 is made from 1.
plain = line(SETUP)
pose(plain, 10, AT_10)
everything(plain)
assert ops.mesh.symmetrize(match_shape="0") == {'FINISHED'}
assert close(where(plain, 2), (1.2, 0.5)), where(plain, 2)
assert close(where(plain, 1), (-1.2, 0.5)), "the setup's right side stays"
doc.frame_current = 0

# By default, off frame 0, 0 and 2 are the right side now.
moved = line(SETUP)
pose(moved, 10, AT_10)
everything(moved)
assert ops.mesh.symmetrize() == {'FINISHED'}
assert wm.last_operation()[1]["match_shape"] == "1", "the panel shows it on"
assert close(where(moved, 2), (1.5, 0.8)) and close(where(moved, 0), (2.2, 0.1)), "the right side now stays"
assert close(where(moved, 1), (-1.5, 0.8)) and close(where(moved, 3), (-2.2, 0.1)), (where(moved, 1), where(moved, 3))
doc.frame_current = 11
keys = {f["frame"]: tuple(f["value"]) for f in moved.keyframes("points[1].pos")}
assert close(keys[10], (-2.5, -0.2)), ("keyed as its Transform", keys)
doc.frame_current = 0
assert close([where(moved, i) for i in range(4)], SETUP), "the setup stays"

# The line mirrors as well as its points: a line's two sides pair from opposite ends, not by the nearest, which here
# would give 0 the image of 3 and leave the line crossing itself.
bent = line([(-0.6, 0.0), (-0.3, 0.3), (0.0, 0.0), (0.3, 0.3), (0.6, 0.0)])
pose(bent, 10, {3: (0.05, 0.4), 4: (0.42, 0.43)})
everything(bent)
assert ops.mesh.symmetrize(match_shape="1") == {'FINISHED'}
assert close(where(bent, 0), (-0.42, 0.43)) and close(where(bent, 1), (-0.05, 0.4)), (where(bent, 0), where(bent, 1))
doc.frame_current = 0

# A closed loop pairs from wherever its sides meet, whichever point it starts at.
loop = line([(0.5, 0.5), (0.0, 1.0), (-0.5, 0.5), (-0.5, -0.5), (0.0, -1.0), (0.5, -0.5)], closed=True)
pose(loop, 10, {0: (0.9, 0.2), 5: (0.7, -0.7)})
everything(loop)
assert ops.mesh.symmetrize(match_shape="1") == {'FINISHED'}
assert close(where(loop, 2), (-0.9, 0.2)) and close(where(loop, 3), (-0.7, -0.7)), (where(loop, 2), where(loop, 3))
doc.frame_current = 0

# Left to Right copies what is on the left now.
left = line(SETUP)
pose(left, 10, AT_10)
everything(left)
assert ops.mesh.symmetrize(match_shape="1", direction="LEFT_TO_RIGHT") == {'FINISHED'}
assert close(where(left, 2), (1.2, 0.5)) and close(where(left, 0), (2.0, 0.0)), (where(left, 2), where(left, 0))
doc.frame_current = 0

# --- A setup that isn't symmetric: the change is mirrored otherwise, the image exact with the option ----------
crooked = [(1.0, 0.0), (-1.3, 0.4)]
for option, wanted in (("0", (-1.5, 0.6)), ("1", (-1.2, 0.2))):
    layer = line(crooked)
    pose(layer, 10, {0: (1.2, 0.2)})
    everything(layer)
    assert ops.mesh.symmetrize(match_shape=option) == {'FINISHED'}
    assert close(where(layer, 1), wanted), (option, where(layer, 1))
    doc.frame_current = 0

# --- Points near the axis go onto it, not to their setup's x -----------------------------------------------
layer = line([(1.0, 0.0), (0.5, 1.0), (-1.0, 0.0)])
pose(layer, 10, {1: (0.002, 1.2)})
everything(layer)
assert ops.mesh.symmetrize(match_shape="1") == {'FINISHED'}
assert close(where(layer, 1), (0.0, 1.2)), where(layer, 1)
doc.frame_current = 0

# Handles come across as mirror images.
layer = line([(1.0, 0.0), (0.0, 1.0), (-1.0, 0.0)])
pose(layer, 10, {0: (1.4, 0.3)})
layer.move_handle(0, 0, "OUT", (1.1, 0.9))
doc.frame_current = 11
doc.frame_current = 10
everything(layer)
assert ops.mesh.symmetrize(match_shape="1") == {'FINISHED'}
_, _, out0 = layer.curve_handles(0)[0]
_, in2, _ = layer.curve_handles(0)[2]
assert close(in2, (-out0[0], out0[1]), 1e-3), (out0, in2)
doc.frame_current = 0

# --- Handles on welded points: each lands on the handle facing the same way, on whichever curve that is --------------
def point_at(layer, xy):
    return min(range(layer.point_count), key=lambda i: abs(where(layer, i)[0] - xy[0]) + abs(where(layer, i)[1] - xy[1]))


def handle_toward(layer, point, other):
    """The handle of `point` reaching for `other` on their curve, as an offset from the point (coarse, past detail)."""
    points, curves = layer.shown(level=0)
    for cps, _ in curves:
        ids = [c[0] for c in cps]
        if point in ids and other in ids:
            i, j = ids.index(point), ids.index(other)
            h = cps[i][1 if j < i else 2]
            return (h[0] - points[point][0], h[1] - points[point][1])
    raise AssertionError((point, other))


def weld(layer, xy):
    layer.select_points(range(layer.point_count), False)
    layer.select_points([i for i in range(layer.point_count) if close(where(layer, i), xy)])
    assert ops.mesh.merge() == {'FINISHED'}


def hair():
    """Two lines meeting at each of (1, 0) and (-1, 0), as the hair's do: not in the same order on the two sides."""
    layer = line([(1.0, 0.0), (1.5, 1.0)], "Hair")
    layer.add_curve([(-1.4, 1.0), (-1.0, 0.0)])
    layer.add_curve([(1.0, 0.0), (0.5, -1.0)])
    layer.add_curve([(-1.0, 0.0), (-0.6, -1.0)])
    weld(layer, (1.0, 0.0))
    weld(layer, (-1.0, 0.0))
    right, left = point_at(layer, (1.0, 0.0)), point_at(layer, (-1.0, 0.0))
    up_r, down_r = point_at(layer, (1.5, 1.0)), point_at(layer, (0.5, -1.0))
    up_l, down_l = point_at(layer, (-1.4, 1.0)), point_at(layer, (-0.6, -1.0))
    wires = layer.detail["wires"]
    for c, wire in enumerate(wires):
        if right in wire:
            i = wire.index(right)
            to = (1.1, 0.4) if up_r in wire else (0.8, -0.3)
            layer.move_handle(c, i, "OUT" if i == 0 else "IN", to)
    layer.select_points(range(layer.point_count), False)
    layer.select_points([right, left])
    return layer, right, left, up_r, down_r, up_l, down_l


# On frame 0.
layer, right, left, up_r, down_r, up_l, down_l = hair()
assert ops.mesh.symmetrize() == {'FINISHED'}
assert close(handle_toward(layer, left, up_l), (-0.1, 0.4), 1e-3), handle_toward(layer, left, up_l)
assert close(handle_toward(layer, left, down_l), (0.2, -0.3), 1e-3), handle_toward(layer, left, down_l)
assert close(handle_toward(layer, right, up_r), (0.1, 0.4), 1e-3), "the source side stays"

# Off it, on the shape.
layer, right, left, up_r, down_r, up_l, down_l = hair()
pose(layer, 10, {right: (1.2, 0.1), left: (-0.8, 0.3)})
layer.select_points([right, left])
assert ops.mesh.symmetrize() == {'FINISHED'}
assert close(where(layer, left), (-1.2, 0.1))
up, down = handle_toward(layer, right, up_r), handle_toward(layer, right, down_r)
assert close(handle_toward(layer, left, up_l), (-up[0], up[1]), 1e-3), (up, handle_toward(layer, left, up_l))
assert close(handle_toward(layer, left, down_l), (-down[0], down[1]), 1e-3), (down, handle_toward(layer, left, down_l))
doc.frame_current = 0

# Left to Right copies the left side's the same way.
layer, right, left, up_r, down_r, up_l, down_l = hair()
was = handle_toward(layer, right, down_r)
layer.move_handle(*[(c, w.index(left), "OUT" if w.index(left) == 0 else "IN")
                    for c, w in enumerate(layer.detail["wires"]) if left in w and down_l in w][0], (-0.7, -0.2))
assert ops.mesh.symmetrize(direction="LEFT_TO_RIGHT") == {'FINISHED'}
assert close(handle_toward(layer, right, down_r), (-0.3, -0.2), 1e-3), (was, handle_toward(layer, right, down_r))

# A point on the axis with a line ending at it from each side: the source side's handle sets the other's.
layer = line([(1.0, 0.0), (0.0, 1.0)], "Peak")
layer.add_curve([(-1.0, 0.2), (0.0, 1.0)])
weld(layer, (0.0, 1.0))
peak = point_at(layer, (0.0, 1.0))
side_r, side_l = point_at(layer, (1.0, 0.0)), point_at(layer, (-1.0, 0.2))
for c, wire in enumerate(layer.detail["wires"]):
    i = wire.index(peak)
    layer.move_handle(c, i, "OUT" if i == 0 else "IN", (0.3, 0.8) if side_r in wire else (-0.1, 0.9))
layer.select_points(range(layer.point_count), False)
layer.select_points([peak])
assert ops.mesh.symmetrize() == {'FINISHED'}
assert close(handle_toward(layer, peak, side_l), (-0.3, -0.2), 1e-3), handle_toward(layer, peak, side_l)
assert close(handle_toward(layer, peak, side_r), (0.3, -0.2), 1e-3), handle_toward(layer, peak, side_r)

# --- A detail point on one side and a point of its own on the other: the lines are drawn the same ---------------------
# Its neighbours draw their handles shortened, so copying them as stored would leave the two sides different.
def drawn(layer):
    """The curve as it draws: (where, handle in, handle out) along it, the handles as offsets from their point."""
    points, curves = layer.shown()
    return [(points[c[0]], (c[1][0] - points[c[0]][0], c[1][1] - points[c[0]][1]),
             (c[2][0] - points[c[0]][0], c[2][1] - points[c[0]][1])) for c in curves[0][0]]


for direction in ("RIGHT_TO_LEFT", "LEFT_TO_RIGHT"):
    layer = line([(1.0, 1.0), (0.0, 0.0), (-0.5, 0.45), (-1.0, 1.0)], "Detail")
    layer.move_handle(0, 1, "IN", (0.3, 0.05))
    layer.move_handle(0, 1, "OUT", (-0.1, -0.02))
    pose(layer, 10, {0: (1.1, 1.0)})
    doc.frame_current = 0
    layer.set_detail_level(1)
    detail = layer.split_segment(0, 0, 0.5)
    assert layer.detail["levels"][detail] == 1
    doc.frame_current = 10
    everything(layer)
    assert ops.mesh.symmetrize(direction=direction) == {'FINISHED'}
    line_now = drawn(layer)
    assert len(line_now) == 5, line_now
    for i in range(5):
        at, into, out = line_now[i]
        other_at, other_in, other_out = line_now[4 - i]
        assert close(at, (-other_at[0], other_at[1]), 1e-3), (direction, i, line_now)
        if i > 0:
            assert close(into, (-other_out[0], other_out[1]), 1e-3), (direction, i, into, other_out)
    doc.frame_current = 0

# Detail on one side alone, the other going straight to the same point: the handle as it is, past the detail.
layer = line([(1.0, 1.0), (0.0, 0.0), (-1.0, 1.0)], "Detail alone")
layer.move_handle(0, 0, "OUT", (0.8, 0.5))
pose(layer, 10, {0: (1.1, 1.0)})
doc.frame_current = 0
layer.set_detail_level(1)
layer.split_segment(0, 0, 0.5)
doc.frame_current = 10
layer.select_points(range(layer.point_count), False)
layer.select_points([0, 2])
assert ops.mesh.symmetrize() == {'FINISHED'}
coarse = handle_toward(layer, 0, 1)
assert close(handle_toward(layer, 2, 1), (-coarse[0], coarse[1]), 1e-3), (coarse, handle_toward(layer, 2, 1))
doc.frame_current = 0

# --- Axis: a shape that has moved off the layer's line --------------------------------------------------------

layer = line([(1.0, 0.0), (0.5, 1.0), (-0.5, 1.0), (-1.0, 0.0)])
pose(layer, 10, {0: (6.0, 0.0), 1: (5.5, 1.2), 2: (4.5, 1.0), 3: (4.0, 0.0)})
everything(layer)
assert ops.mesh.symmetrize(match_shape="1") == {'CANCELLED'}, "all on one side of the layer's line"
assert wm.banner_report() is not None and close(where(layer, 1), (5.5, 1.2)), "nothing changed"
everything(layer)
assert ops.mesh.symmetrize(match_shape="1", axis="SELECTION") == {'FINISHED'}
assert close(where(layer, 2), (4.5, 1.2)) and close(where(layer, 3), (4.0, 0.0)), (where(layer, 2), where(layer, 3))
assert close(where(layer, 1), (5.5, 1.2)), "the right side stays"
doc.frame_current = 0

# --- Uneven sides on this frame only: warns, selects the point without a partner, changes nothing ----------------
layer = line(SETUP)
pose(layer, 10, {2: (0.5, 1.0)})
everything(layer)
assert ops.mesh.symmetrize() == {'FINISHED'}, "from the menu: even in the setup, so its pairs"
assert wm.last_operation()[1]["match_shape"] == "0", "the panel shows what ran"
assert "the setup's pairs" in wm.banner_report()[3], wm.banner_report()
doc.frame_current = 0
layer = line(SETUP)
pose(layer, 10, {2: (0.5, 1.0)})
everything(layer)
assert ops.mesh.symmetrize(match_shape="1") == {'CANCELLED'}
assert [i for i, p in enumerate(layer.points) if p[2]] == [1, 2], "the ones left without a partner"
assert close(where(layer, 2), (0.5, 1.0))
doc.frame_current = 0

# --- This frame only -----------------------------------------------------------------------------------------
layer = line(SETUP)
pose(layer, 10, {0: (2.1, 0.0), 1: (0.9, 1.3), 2: (-1.1, 1.0), 3: (-2.1, 0.0)})
pose(layer, 20, {1: (1.1, 0.7), 2: (-0.4, 0.2)})
doc.frame_current = 10
at_10 = [where(layer, i) for i in range(4)]
doc.frame_current = 20
everything(layer)
assert ops.mesh.symmetrize(match_shape="1") == {'FINISHED'}
assert close(where(layer, 2), (-1.1, 0.7))
doc.frame_current = 10
assert close([where(layer, i) for i in range(4)], at_10), "another keyed frame stays"
doc.frame_current = 0
assert close([where(layer, i) for i in range(4)], SETUP), "the setup stays"

# One undo step.
layer = line(SETUP)
pose(layer, 10, {1: (1.1, 0.7)})
assert ops.mesh.select_all() == {'FINISHED'}, "an undo step to come back to"
was = where(layer, 2)
assert ops.mesh.symmetrize(match_shape="1") == {'FINISHED'}
assert not close(where(layer, 2), was)
ops.ed.undo()
assert close(where(layer, 2), was), "undone"
doc.frame_current = 0

# Shape looks aren't copied: they hold on every frame, and these pairs aren't the setup's.
ops.layer.new()
eyes = layers.active
eyes.add_curve([(0.5, 0.0), (1.0, 0.0), (1.0, 0.5), (0.5, 0.5)], closed=True, fill=True)
eyes.add_curve([(-1.0, 0.0), (-0.5, 0.0), (-0.5, 0.5), (-1.0, 0.5)], closed=True, fill=True)
eyes.set_shape_style(0, fill=(1.0, 0.0, 0.0, 1.0))
eyes.set_shape_style(1, fill=(0.0, 1.0, 0.0, 1.0))
pose(eyes, 10, {0: (0.6, 0.1)})
everything(eyes)
assert ops.mesh.symmetrize(match_shape="1") == {'FINISHED'}
assert close(eyes.shape_style(1)["fill"], (0.0, 1.0, 0.0, 1.0)), "keeps its look"
doc.frame_current = 0

# On frame 0 it asks for nothing different.
a, b = line([(1.0, 0.0), (-1.2, 0.3)]), line([(1.0, 0.0), (-1.2, 0.3)])
for layer, args in ((a, {}), (b, {"match_shape": "1", "axis": "SELECTION"})):
    layers.active = layer
    everything(layer)
    assert ops.mesh.symmetrize(**args) == {'FINISHED'}
assert close(where(a, 1), (-1.0, 0.0)) and close(where(b, 1), where(a, 1)), (where(a, 1), where(b, 1))
c = line([(1.0, 0.0), (-1.2, 0.3)])
everything(c)
assert ops.mesh.symmetrize() == {'FINISHED'}
assert wm.last_operation()[1]["match_shape"] == "0", "the panel on frame 0: off, so Weights shows and Axis doesn't"

# Weights asked for by a script off frame 0: the setup's pairs, which is the way they copy.
layer = line(SETUP)
pose(layer, 10, AT_10)
everything(layer)
assert ops.mesh.symmetrize(weights="1") == {'FINISHED'}
assert close(where(layer, 2), (1.2, 0.5)) and wm.last_operation()[1]["match_shape"] == "0"
doc.frame_current = 0

# The ViewportActionPanel: turning it on runs Symmetrize again in place of the first run.
layer = line(SETUP)
pose(layer, 10, AT_10)
assert ops.mesh.select_all() == {'FINISHED'}, "an undo step to come back to"
assert ops.mesh.symmetrize(match_shape="0") == {'FINISHED'}
assert close(where(layer, 2), (1.2, 0.5))
assert wm.redo_last(match_shape=True) == "FINISHED"
assert close(where(layer, 2), (1.5, 0.8)) and close(where(layer, 1), (-1.5, 0.8)), (where(layer, 1), where(layer, 2))
assert wm.last_operation()[1]["match_shape"] == "1"
doc.frame_current = 0

# A panel change that can't run leaves the last result, and the panel on what was asked, so Axis can be picked next.
layer = line([(1.0, 0.0), (0.5, 1.0), (-0.5, 1.0), (-1.0, 0.0)])
pose(layer, 10, {0: (6.0, 0.0), 1: (5.5, 1.2), 2: (4.5, 1.0), 3: (4.0, 0.0)})
assert ops.mesh.select_all() == {'FINISHED'}, "an undo step to come back to"
assert ops.mesh.symmetrize() == {'FINISHED'}
plain_result = [where(layer, i) for i in range(4)]
assert wm.redo_last(match_shape=True) == "CANCELLED", "all on one side of the layer's line"
assert close([where(layer, i) for i in range(4)], plain_result), "the last result stands"
last = wm.last_operation()
assert last is not None and last[1]["match_shape"] == "1", last
assert doc.frame_current == 10
assert wm.redo_last(axis="SELECTION") == "FINISHED"
assert close(where(layer, 2), (4.5, 1.2)) and close(where(layer, 3), (4.0, 0.0)), (where(layer, 2), where(layer, 3))
assert wm.last_operation()[1]["axis"] == "SELECTION"
doc.frame_current = 0

# --- Run from the menu ---------------------------------------------------------------------------------------
# Both right of the axis in the setup, one on each side on this frame: the setup's way there is nothing to pair.
layer = line([(0.15, 0.5), (0.2, 0.9)])
pose(layer, 10, {0: (-0.33, 0.96), 1: (0.4, 0.92)})
assert ops.mesh.select_all() == {'FINISHED'}, "an undo step to come back to"
assert ops.mesh.symmetrize() == {'FINISHED'}, "not uneven on this frame"
assert close(where(layer, 0), (-0.4, 0.92)) and close(where(layer, 1), (0.4, 0.92)), (where(layer, 0), where(layer, 1))
args = wm.last_operation()[1]
assert args["match_shape"] == "1" and args["axis"] == "LAYER", ("the panel shows what ran", args)
# Turned off in the panel: the warning, saying whose sides it counted, the last result standing and the panel open.
assert wm.redo_last(match_shape=False) == "CANCELLED"
assert "in the setup pose" in wm.banner_report()[3], wm.banner_report()
assert close(where(layer, 0), (-0.4, 0.92)), "the last result stands"
assert wm.last_operation()[1]["match_shape"] == "0"
assert wm.redo_last(match_shape=True) == "FINISHED"
assert close(where(layer, 0), (-0.4, 0.92))
doc.frame_current = 0

# Told not to, it doesn't, and the same selection on frame 0 goes on warning.
layer = line([(0.15, 0.5), (0.2, 0.9), (0.6, 0.2)])
pose(layer, 10, {0: (-0.33, 0.96), 1: (0.4, 0.92)})
layer.select_points([0, 1])
assert ops.mesh.symmetrize(match_shape="0") == {'CANCELLED'}
assert close(where(layer, 0), (-0.33, 0.96))
doc.frame_current = 0
layer.select_points(range(3), False)
layer.select_points([0, 1])
assert ops.mesh.symmetrize() == {'CANCELLED'}, "frame 0 has only the one shape"
assert "setup pose" not in wm.banner_report()[3]

# A shape off the layer's line, from the menu: the setup can't pair it either, so the middle of the selection, and the
# panel opens to change it.
layer = line([(1.0, 0.0), (1.5, 1.0), (2.0, 1.0), (2.5, 0.0)])
pose(layer, 10, {0: (4.0, 0.0), 1: (4.5, 1.0), 2: (5.5, 1.2), 3: (6.0, 0.0)})
everything(layer)
assert ops.mesh.symmetrize() == {'FINISHED'}
args = wm.last_operation()[1]
assert args["match_shape"] == "1" and args["axis"] == "SELECTION", args
assert "the middle of the selection" in wm.banner_report()[3], wm.banner_report()
assert close(where(layer, 1), (4.5, 1.2)), where(layer, 1)
doc.frame_current = 0

# Told which way, it doesn't look for another.
layer = line(SETUP)
pose(layer, 10, {2: (0.5, 1.0)})
layer.select_points([0, 1, 2])
assert ops.mesh.symmetrize(match_shape="1") == {'CANCELLED'}
assert "on this frame" in wm.banner_report()[3], wm.banner_report()
doc.frame_current = 0

# Nothing pairs by any of them: the warning, in this frame's numbers.
layer = line([(2.0, 0.0), (1.0, 1.0), (0.5, 0.2), (-1.0, 1.0), (-2.0, 0.0)])
pose(layer, 10, {3: (0.8, 1.0)})
layer.select_points([0, 1, 3, 2])
assert ops.mesh.symmetrize() == {'CANCELLED'}
assert "on this frame" in wm.banner_report()[3], wm.banner_report()
doc.frame_current = 0

# --- Raising the Threshold never breaks a pair -------------------------------------------------------------------
# Partners by the shape, not the setup. A threshold that takes in one of them, or both, leaves them partners.
for threshold in ("0.01", "0.2", "0.42", "0.45", "0.5", "1.0", "5.0"):
    layer = line([(0.0, 0.7), (0.19, 0.5)])
    pose(layer, 10, {0: (0.46, 0.83), 1: (-0.41, 0.85)})
    everything(layer)
    assert ops.mesh.symmetrize(match_shape="1", threshold=threshold) == {'FINISHED'}, threshold
    assert close(where(layer, 0), (0.46, 0.83)) and close(where(layer, 1), (-0.46, 0.83)), (threshold, where(layer, 1))
    doc.frame_current = 0

# In the panel, dragged up past where one point is taken in, and on past both.
layer = line([(0.0, 0.7), (0.19, 0.5)])
pose(layer, 10, {0: (0.46, 0.83), 1: (-0.41, 0.85)})
assert ops.mesh.select_all() == {'FINISHED'}, "an undo step to come back to"
assert ops.mesh.symmetrize() == {'FINISHED'}
assert wm.last_operation()[1]["match_shape"] == "1"
for threshold in (0.2, 0.43, 0.6, 2.0):
    assert wm.redo_last(threshold=threshold) == "FINISHED", threshold
    assert close(where(layer, 1), (-0.46, 0.83)), (threshold, where(layer, 1))
doc.frame_current = 0

# On frame 0 too.
layer = line([(0.46, 0.83), (-0.41, 0.85)])
everything(layer)
assert ops.mesh.symmetrize(threshold="0.43") == {'FINISHED'}
assert close(where(layer, 1), (-0.46, 0.83)), where(layer, 1)

# The axis's own points stay the axis's: the top and the bottom of a shape are across it, but no partners.
layer = line([(0.002, 1.0), (1.0, 0.0), (-0.003, -1.0), (-1.0, 0.0)])
everything(layer)
assert ops.mesh.symmetrize(threshold="0.05") == {'FINISHED'}
assert close(where(layer, 0), (0.0, 1.0)) and close(where(layer, 2), (0.0, -1.0)), (where(layer, 0), where(layer, 2))
assert close(where(layer, 3), (-1.0, 0.0)), where(layer, 3)

# A point near the axis isn't taken for the partner of one already paired, or of one far across.
layer = line([(0.12, 0.0), (-0.12, 0.0), (-0.09, 0.0)])
everything(layer)
assert ops.mesh.symmetrize(threshold="0.1") == {'FINISHED'}
assert close(where(layer, 1), (-0.12, 0.0)) and close(where(layer, 2), (0.0, 0.0)), (where(layer, 1), where(layer, 2))
layer = line([(1.0, 0.0), (-0.005, 0.02)])
everything(layer)
assert ops.mesh.symmetrize(threshold="0.01") == {'CANCELLED'}, "uneven: it's on the axis, not the partner of 0"
assert close(where(layer, 1), (-0.005, 0.02))

# --- Points added to a moving curve are matched like the rest ------------------------------------------------------
layer = line([(2.0, 0.0), (1.0, 1.0), (-1.0, 1.0), (-2.0, 0.0)])
pose(layer, 10, {1: (1.2, 1.3)})
doc.frame_current = 0
left_new = layer.split_segment(0, 2, 0.5)
right_new = layer.split_segment(0, 0, 0.5)
doc.frame_current = 10
everything(layer)
assert ops.mesh.symmetrize(match_shape="1") == {'FINISHED'}
a, b = where(layer, right_new), where(layer, left_new)
assert close(b, (-a[0], a[1]), 1e-3), (a, b)
assert close(where(layer, 2), (-1.2, 1.3)), where(layer, 2)
doc.frame_current = 0

# --- Through bones: mirror images as they show ---------------------------------------------------------------
ops.wm.read_homefile(confirmed="1")
doc, layers = oanim.data.document, oanim.data.layers
doc.auto_key = True


def rig():
    ops.layer.new_bone()
    bones = layers.active
    ops.armature.bone_add(name="a", head_x="-0.5", head_y="0", tail_x="0", tail_y="0")
    ops.armature.bone_add(name="b", head_x="0", head_y="0", tail_x="0.5", tail_y="0")
    ops.layer.new()
    art = layers.active
    art.add_curve([(-0.6, 0.0), (-0.3, 0.3), (0.0, 0.0), (0.3, 0.3), (0.6, 0.0)])
    ops.layer.move_to_group(group=str(bones.id))
    layers.active = art
    ops.weight.automatic()
    layers.active = bones
    doc.frame_current = 10
    bones.bone_set("b", angle=0.8)
    doc.frame_current = 11
    doc.frame_current = 10
    layers.active = art
    return art, bones


art, bones = rig()
points, _ = art.shown()
assert not close(points[4], (-points[0][0], points[0][1]), 1e-2), "the bone bends it"
weights_before = art.point_bones
everything(art)
assert ops.mesh.symmetrize(match_shape="1", weights="1") == {'FINISHED'}
points, _ = art.shown()
for i, j in ((0, 4), (1, 3)):
    assert close(points[j], (-points[i][0], points[i][1]), 1e-3), (i, j, points)
assert abs(points[2][0]) < 1e-3, ("on the axis", points[2])
assert art.point_bones == weights_before, "weights hold on every frame"
doc.frame_current = 0
points, _ = art.shown()
assert close(points[1], (-0.3, 0.3), 1e-3), "the setup stays"

# Sided pairs: across the pair, by the shape as it shows.
ops.wm.read_homefile(confirmed="1")
doc, layers = oanim.data.document, oanim.data.layers
doc.auto_key = True
ear_r = line([(-1.0, 0.0), (-1.5, 0.5), (-1.1, 1.0)], "Ear_R")
ear_l = line([(1.0, 0.0), (1.4, 0.6), (1.0, 1.0)], "Ear_L")
pose(ear_l, 10, {1: (1.9, 0.7)})
everything(ear_l)
assert ops.mesh.symmetrize(match_shape="1") == {'FINISHED'}
assert close(where(ear_r, 1), (-1.9, 0.7)) and close(where(ear_r, 2), (-1.0, 1.0)), [where(ear_r, i) for i in range(3)]
assert close(where(ear_l, 1), (1.9, 0.7)), "the source side stays"
doc.frame_current = 0
assert close(where(ear_r, 1), (-1.5, 0.5)), "the setup stays"

# A threshold that takes in a sided pair's points leaves them partners.
ops.wm.read_homefile(confirmed="1")
doc, layers = oanim.data.document, oanim.data.layers
doc.auto_key = True
ear_r = line([(-1.0, 0.0), (-1.5, 0.5), (-1.1, 1.0)], "Ear_R")
ear_l = line([(1.0, 0.0), (1.4, 0.6), (1.0, 1.0)], "Ear_L")
pose(ear_l, 10, {1: (1.9, 0.7)})
everything(ear_l)
assert ops.mesh.symmetrize(match_shape="1", threshold="3.0") == {'FINISHED'}
assert close(where(ear_r, 1), (-1.9, 0.7)) and close(where(ear_l, 1), (1.9, 0.7)), (where(ear_r, 1), where(ear_l, 1))
doc.frame_current = 0

# Both ears moved off the middle: the middle of the pair.
ops.wm.read_homefile(confirmed="1")
doc, layers = oanim.data.document, oanim.data.layers
doc.auto_key = True
ear_r = line([(-1.0, 0.0), (-1.5, 0.5), (-1.0, 1.0)], "Ear_R")
ear_l = line([(1.0, 0.0), (1.5, 0.5), (1.0, 1.0)], "Ear_L")
pose(ear_r, 10, {0: (2.0, 0.0), 1: (1.5, 0.5), 2: (2.0, 1.0)})
pose(ear_l, 10, {0: (4.0, 0.0), 1: (4.8, 0.6), 2: (4.0, 1.0)})
everything(ear_l)
assert ops.mesh.symmetrize(match_shape="1", axis="SELECTION") == {'FINISHED'}
assert close(where(ear_l, 1), (4.8, 0.6)), "the right side stays"
assert close(where(ear_r, 1), (1.5, 0.6)), [where(ear_r, i) for i in range(3)]
assert close(where(ear_r, 0), (2.3, 0.0)), [where(ear_r, i) for i in range(3)]
doc.frame_current = 0
