"""Symmetrize (right-click on points): one side's selected points copied onto their partners across
the layer's vertical axis, paired by where each reflection lands nearest; points on the axis snap
onto it with even handles (flat when nearly flat); uneven sides change nothing, warn in the status
bar and select the points without a partner; posed frames mirror the pose. Then the
ViewportActionPanel's redo: changing Direction runs it again in place of the first run.

Run with:  OpenAnimate --background --python tests/test_symmetrize.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0
doc.auto_key = True


def close(a, b, eps=1e-4):
    if isinstance(a, (tuple, list)):
        return all(abs(x - y) < eps for x, y in zip(a, b))
    return abs(a - b) < eps


layer = layers.active
# A face-like loop: top and bottom on the axis (one a hair off it), two hand-drawn points each side.
shape = [(0.003, 1.0), (0.8, 0.7), (1.0, -0.2), (0.0, -1.0), (-1.1, -0.25), (-0.75, 0.75)]
layer.add_curve(shape, closed=True, fill=True)
oanim.ops.mesh.select_all()


def where(i):
    return tuple(layer.points[i][:2])


def handles(i):
    """Point i's (in, out) handles as offsets from it (curve 0, curve point i)."""
    _, hin, hout = layer.curve_handles(0)[i]
    p = where(i)
    return (hin[0] - p[0], hin[1] - p[1]), (hout[0] - p[0], hout[1] - p[1])


# --- Right to Left (the default) ------------------------------------------------------------------
assert oanim.ops.mesh.symmetrize() == {'FINISHED'}
assert close(where(5), (-0.8, 0.7)) and close(where(4), (-1.0, -0.2)), (where(4), where(5))
assert close(where(1), (0.8, 0.7)) and close(where(2), (1.0, -0.2)), "the source side stays"
assert close(where(0)[0], 0.0) and close(where(3)[0], 0.0), "on the axis: snapped onto it"
# Mirror images run the other way round the loop: point 5's out handle is point 1's in, reflected.
in1, out1 = handles(1)
in5, out5 = handles(5)
assert close(out5, (-in1[0], in1[1]), 1e-3) and close(in5, (-out1[0], out1[1]), 1e-3), (handles(1), handles(5))
# The top point's handles mirror each other.
in0, out0 = handles(0)
assert close(in0, (-out0[0], out0[1]), 1e-3), handles(0)

# --- The ViewportActionPanel: change Direction, and it runs again in place of the first run -----------
last = oanim.wm.last_operation()
assert last is not None and last[0] == "mesh.symmetrize" and last[1]["direction"] == "RIGHT_TO_LEFT", last
assert oanim.wm.redo_last(direction="LEFT_TO_RIGHT") == "FINISHED"
assert close(where(1), (0.75, 0.75)) and close(where(2), (1.1, -0.25)), "the left side copied to the right now"
assert close(where(5), (-0.75, 0.75)), "the left side stays"
assert oanim.wm.last_operation()[1]["direction"] == "LEFT_TO_RIGHT"
history = oanim.wm.undo_history()[0]
assert history.count("Symmetrize") == 1, ("one step, not two", history)
oanim.ops.ed.undo()
assert close(where(5), (-0.75, 0.75)) and close(where(1), (0.8, 0.7)) and close(where(0), (0.003, 1.0)), \
    "one undo: as before Symmetrize"
assert oanim.wm.last_operation() is None, "an undo leaves nothing to adjust"

# --- Uneven sides: nothing changes, a warning, the odd one selected ---------------------------------------
oanim.ops.mesh.select_none()
layer.select_points([1, 2, 5])
before = [where(i) for i in range(6)]
shown = oanim.wm.banner_report()[0]
assert oanim.ops.mesh.symmetrize() == {'CANCELLED'}
assert [where(i) for i in range(6)] == before, "nothing moved"
report = oanim.wm.banner_report()
assert report[0] > shown and report[1] == "WARNING" and "2 on the right, 1 on the left" in report[4], report
selected = [i for i, p in enumerate(layer.points) if p[2]]
assert len(selected) == 1 and selected[0] in (1, 2), ("the point without a partner", selected)

# --- One point on the axis: just its handles, flat when nearly flat --------------------------------------
oanim.ops.layer.new()
line = layers.active
line.add_curve([(-1.0, 0.1), (0.0, 0.0), (1.0, -0.05)])
layer = line
oanim.ops.mesh.select_none()
here = where(1)
line.move_handle(0, 1, "OUT", (here[0] + 0.4, here[1] + 0.03))  # 4 degrees off flat (the in handle turns too)
line.select_points([1])
assert oanim.ops.mesh.symmetrize() == {'FINISHED'}
hin, hout = handles(1)
assert close(hout, (0.4, 0.0), 1e-3) and close(hin, (-0.4, 0.0), 1e-3), ("flat and even", hin, hout)
# A point off the axis alone can't be symmetrized.
oanim.ops.mesh.select_none()
line.select_points([2])
assert oanim.ops.mesh.symmetrize() == {'CANCELLED'}

# --- Posed: the pose is mirrored, not the drawing ----------------------------------------------------------
oanim.ops.layer.new()
pose = layers.active
pose.add_curve([(1.0, 0.0), (0.0, 1.0), (-1.0, 0.0)])
layer = pose
oanim.ops.mesh.select_none()
doc.frame_current = 10
pose.set_point(0, (1.2, 0.3))
doc.frame_current = 11
doc.frame_current = 10
pose.select_points([0, 2])
assert oanim.ops.mesh.symmetrize() == {'FINISHED'}
assert close(where(2), (-1.2, 0.3)), where(2)
doc.frame_current = 11
k = [(x["frame"], tuple(x["value"])) for x in pose.keyframes("points[2].pos")]
assert [f for f, v in k] == [0, 10] and close(k[1][1], (-0.2, 0.3)), ("keyed as its Transform", k)
doc.frame_current = 0
assert close(where(2), (-1.0, 0.0)), "its setup stays"

# Looks: the copied side's shapes give their look to their images (a shape on both sides keeps its own).
oanim.ops.layer.new()
eyes = oanim.data.layers.active
eyes.add_curve([(0.5, 0.0), (1.0, 0.0), (1.0, 0.5), (0.5, 0.5)], closed=True, fill=True)
eyes.add_curve([(-1.0, 0.0), (-0.5, 0.0), (-0.5, 0.5), (-1.0, 0.5)], closed=True, fill=True)
eyes.set_shape_style(0, fill=(1.0, 0.0, 0.0, 1.0), line_width=0.03, has_outline=False)
eyes.select_points(range(eyes.point_count))
assert oanim.ops.mesh.symmetrize(direction="RIGHT_TO_LEFT") == {'FINISHED'}
left = eyes.shape_style(1)
assert all(abs(a - b) < 1e-6 for a, b in zip(left["fill"], (1.0, 0.0, 0.0, 1.0))), left
assert abs(left["line_width"] - 0.03) < 1e-6 and not left["has_outline"], left

print("Symmetrize tests passed")
