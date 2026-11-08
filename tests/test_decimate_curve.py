"""The Select tool's Decimate Curve (mesh.decimate, in its right-click and Drawing menus; Blender's for curves): the
Ratio is the share of the selected points kept, the ones the line changes least without going first, so a curve thins
evenly and a bump keeps its points; unselected points stay; Adjust Last Operation's Ratio runs it again live (a higher
one puts back what a lower took out) and says how many of how many went; it starts at the Ratio it last used (0.5 the
first time); never on a drawing with animation. The Dyntopo popover's Simplify (sculpt.simplify) gets the count too.

Run with:  OpenAnimate --background --python tests/test_decimate_curve.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm

doc.auto_key = False


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


def xs(l):
    return sorted(round(p[0], 4) for p in l.points)


# --- The Ratio of the selected points is kept; unselected points stay ------------------------------------------------
straight = line([(x * 0.25, 0.0) for x in range(17)])
straight.select_points(list(range(4, 13)))
assert ops.mesh.decimate() == {'FINISHED'}
# The first time it keeps half.
assert wm.last_operation()[0] == "mesh.decimate"
assert wm.last_operation()[1]["ratio"] == "0.5", wm.last_operation()
kept = xs(straight)
unselected = [round(x * 0.25, 4) for x in list(range(4)) + list(range(13, 17))]
assert all(x in kept for x in unselected), ("an unselected point went", kept)
# 9 selected: half of them (rounded) go.
assert len(kept) == 17 - 5, ("half of the 9 selected points go", kept)
assert wm.last_operation_note() == "5 of 9 points removed", wm.last_operation_note()
ops.ed.undo()
assert len(straight.points) == 17

straight.select_points(list(range(4, 13)))
assert ops.mesh.decimate(ratio="1") == {'FINISHED'}
assert len(straight.points) == 17 and wm.last_operation_note() == "0 of 9 points removed", wm.last_operation_note()
assert ops.mesh.decimate(ratio="0") == {'FINISHED'}
assert len(straight.points) == 17 - 9, ("Ratio 0 takes out every selected point it may", xs(straight))

# --- The ones the line changes least without go first ----------------------------------------------------------------
bump = line([(x * 0.25, 0.5 * math.exp(-((x * 0.25 - 2.0) / 0.3) ** 2)) for x in range(17)])
bump.select_points(list(range(17)))
assert ops.mesh.decimate(ratio="0.5") == {'FINISHED'}
left = sorted((round(p[0], 4), round(p[1], 4)) for p in bump.points)
assert len(left) == 17 - 9, ("half of 17 (rounded up) go", left)
# The flat runs give up their points; the bump's top and its flanks stay.
for x in (1.75, 2.0, 2.25):
    assert any(abs(px - x) < 1e-3 for px, _ in left), ("the bump lost a point it needs", x, left)
flat = [px for px, _ in left if px < 1.0 or px > 3.0]
assert len(flat) <= 4, ("the flat runs keep points they don't need", left)

# An arc thins evenly: no stretch between kept points much longer than the rest.
arc = line([(math.cos(a / 32 * math.pi) * 2.0, math.sin(a / 32 * math.pi) * 2.0) for a in range(33)])
arc.select_points(list(range(33)))
assert ops.mesh.decimate(ratio="0.25") == {'FINISHED'}
angles = sorted(math.atan2(p[1], p[0]) for p in arc.points)
gaps = [b - a for a, b in zip(angles, angles[1:])]
print("arc of 33 at Ratio 0.25: %d points, gaps %s" % (len(angles), ["%.2f" % g for g in gaps]))
assert len(angles) == 33 - 25, len(angles)
# (each end's handle points at its neighbour, as add_curve makes an open curve's ends, so the line really turns there
# and the point next to an end is the last to go: evenly between them)
inner = gaps[1:-1]
assert max(inner) < 1.5 * min(inner), ("the arc didn't thin evenly", gaps)

# --- Adjust Last Operation: the Ratio runs it again, live -------------------------------------------------------------
again = line([(math.cos(a / 32 * math.pi) * 2.0, math.sin(a / 32 * math.pi) * 2.0) for a in range(33)])
# (selected by an operator, as a click is: an undo step, so the panel's runs find it again)
assert ops.mesh.select_all() == {'FINISHED'}
assert ops.mesh.decimate() == {'FINISHED'}
assert wm.last_operation()[1]["ratio"] == "0.25", ("it starts at the Ratio last used", wm.last_operation())
assert wm.redo_last(ratio=0.1) == "FINISHED"
low = len(again.points)
assert wm.last_operation_note() == "%d of 33 points removed" % (33 - low), wm.last_operation_note()
assert wm.redo_last(ratio=0.9) == "FINISHED"
high = len(again.points)
print("arc of 33: %d points at Ratio 0.1, %d at 0.9" % (low, high))
assert low < high, ("a higher Ratio puts back points a lower one took out", low, high)
assert high == 33 - round(0.1 * 33), high
# One undo step, whatever was tried.
ops.ed.undo()
assert len(again.points) == 33, len(again.points)
# Dyntopo's Resolution is untouched.
assert wm.sculpt_settings()["resolution"] == 50.0, wm.sculpt_settings()

# --- Nothing selected, or a drawing with animation: nothing to do -----------------------------------------------------
ops.mesh.select_none()
assert ops.mesh.decimate() == {'CANCELLED'}
keyed = line([(x * 0.25, 0.0) for x in range(9)])
keyed.keyframe_insert("points[0].pos", 10)
keyed.select_points(list(range(9)))
assert ops.mesh.decimate() == {'CANCELLED'}
assert len(keyed.points) == 9

# --- The popover's Simplify says how many went, too --------------------------------------------------------------------
whole = line([(x * 0.25, 0.0) for x in range(9)])
assert ops.sculpt.simplify(resolution="50") == {'FINISHED'}
assert wm.last_operation_note() == "%d points removed" % (9 - len(whole.points)), wm.last_operation_note()
assert len(whole.points) < 9

print("Decimate Curve: ok")
