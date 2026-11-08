"""The Select tool's Simplify (sculpt.simplify selected_only=1, in its right-click and Drawing menus): only selected
points go; Adjust Last Operation's Resolution runs it again live (a finer one puts back what a coarser took out) and
says how many points went; it starts at the Resolution it last used, kept apart from Dyntopo's; never on a drawing with
animation.

Run with:  OpenAnimate --background --python tests/test_simplify_selected.py
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


# --- Only selected points go -------------------------------------------------------------------------------------------
straight = line([(x * 0.25, 0.0) for x in range(17)])
straight.select_points(list(range(4, 13)))
assert ops.sculpt.simplify(selected_only="1") == {'FINISHED'}
kept = xs(straight)
unselected = [round(x * 0.25, 4) for x in list(range(4)) + list(range(13, 17))]
assert all(x in kept for x in unselected), ("an unselected point went", kept)
assert len(kept) < 17 - 6, ("the selected points the line is straight without stay", kept)
# The panel says how many went, and starts at the Resolution the menu's Simplify last used (50 at first), not Dyntopo's.
assert wm.last_operation()[0] == "sculpt.simplify"
assert wm.last_operation()[1]["resolution"] == "50", wm.last_operation()
assert wm.last_operation_note() == "%d points removed" % (17 - len(kept)), (wm.last_operation_note(), len(kept))
ops.ed.undo()
assert len(straight.points) == 17

# --- Adjust Last Operation: the Resolution runs it again, live ----------------------------------------------------------
arc = line([(math.cos(a / 32 * math.pi) * 2.0, math.sin(a / 32 * math.pi) * 2.0) for a in range(33)])
# (selected by an operator, as a click is: an undo step, so the panel's runs find it again)
assert ops.mesh.select_all() == {'FINISHED'}
assert ops.sculpt.simplify(selected_only="1") == {'FINISHED'}
assert wm.redo_last(resolution=1) == "FINISHED"
coarse = len(arc.points)
assert wm.last_operation_note() == "%d points removed" % (33 - coarse), wm.last_operation_note()
assert wm.redo_last(resolution=500) == "FINISHED"
fine = len(arc.points)
print("arc of 33: %d points at Resolution 1, %d at 500" % (coarse, fine))
assert coarse < fine, ("a finer Resolution puts back points a coarser one took out", coarse, fine)
assert wm.redo_last(resolution=5) == "FINISHED"
# One undo step, whatever was tried.
ops.ed.undo()
assert len(arc.points) == 33, len(arc.points)
# Dyntopo's Resolution is its own.
assert wm.sculpt_settings()["resolution"] == 50.0, wm.sculpt_settings()

# --- Next time it starts at the Resolution last used -------------------------------------------------------------------
again = line([(x * 0.25, 0.0) for x in range(9)])
again.select_points(list(range(9)))
assert ops.sculpt.simplify(selected_only="1") == {'FINISHED'}
assert wm.last_operation()[1]["resolution"] == "5", wm.last_operation()

# --- Nothing it can take out: done all the same, so a finer Resolution in the panel can put points back ------------------
ends = line([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
ends.select_points([0, 2])
assert ops.sculpt.simplify(selected_only="1") == {'FINISHED'}
assert len(ends.points) == 3 and wm.last_operation_note() == "0 points removed", wm.last_operation_note()
# Nothing selected: nothing to do.
ops.mesh.select_none()
assert ops.sculpt.simplify(selected_only="1") == {'CANCELLED'}

# --- Never on a drawing with animation ----------------------------------------------------------------------------------
keyed = line([(x * 0.25, 0.0) for x in range(9)])
keyed.keyframe_insert("points[0].pos", 10)
keyed.select_points(list(range(9)))
assert ops.sculpt.simplify(selected_only="1") == {'CANCELLED'}
assert len(keyed.points) == 9

print("Simplify selected: ok")
