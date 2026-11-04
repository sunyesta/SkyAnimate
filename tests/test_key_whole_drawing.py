"""Key Whole Drawing: a vector layer's drawing keyed as a whole. On for new vector layers: an edit on a frame keys every
point there (their positions, widths, curvature and handles), each holding where it is, so the drawing is keyed as
poses. Turned on for a drawing keyed point by point, every point is keyed wherever any is, and the keys on a frame take
the timing most of them have. Its points trace no path of their own; the drawing's timing is edited as a whole in the
timing chart and Path Timing graph (MotionPathTarget::drawing, drawing=1 on the motion path operators). The timeline
shows its keys as one Drawing row. Saved with the file; files from before it open with it off.

Run with:  OpenAnimate --background --python tests/test_key_whole_drawing.py
"""

import os
import shutil
import tempfile

import oanim

ops = oanim.ops
layers = oanim.data.layers


def close(a, b, eps=1e-4):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def frames(layer, path):
    return [k["frame"] for k in layer.keyframes(path)]


def key(layer, path, frame):
    return next(k for k in layer.keyframes(path) if k["frame"] == frame)


SHAPE = ["points[{}].pos", "points[{}].radius", "curves[0].points[{}].curvature", "curves[0].points[{}].handle_in",
         "curves[0].points[{}].handle_out"]


def every_channel(layer, n, curves=None):
    """Every shape channel of a drawing of `n` points on curves of `curves` points each (one curve by default)."""
    out = [p.format(i) for i in range(n) for p in SHAPE[:2]]
    for c, size in enumerate(curves or [n]):
        out += [p.replace("curves[0]", "curves[%d]" % c).format(i) for i in range(size) for p in SHAPE[2:]]
    return out


doc = oanim.data.document
doc.auto_key = True
doc.auto_freeze_keys = False

# --- New vector layers key the whole drawing ------------------------------------------------------------------------
ops.layer.new()
line = layers.active
assert line.key_whole_drawing, "on for new vector layers"
doc.frame_current = 0
line.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
doc.frame_current = 10
line.set_point(1, (1.0, 1.0))
doc.frame_current = 0
for path in every_channel(line, 3):
    assert frames(line, path) == [0, 10], (path, frames(line, path))
doc.frame_current = 5
assert close(line.points[0][:2], (0.0, 0.0)) and close(line.points[2][:2], (2.0, 0.0)), "the others hold where they are"
assert close(line.points[1][:2], (1.0, 0.5)), line.points[1]
doc.frame_current = 20
line.set_point(2, (2.0, 1.0))
doc.frame_current = 0
assert frames(line, "points[0].pos") == [0, 10, 20] and frames(line, "curves[0].points[1].curvature") == [0, 10, 20]
doc.frame_current = 15
assert close(line.points[1][:2], (1.0, 1.0)), "point 1 holds its pose from 10 to 20"

rows = [r[1] for r in oanim.wm.timeline_rows()]
assert "Drawing" in rows and "Points" not in rows, ("one Drawing row", rows)

# One point's key deleted takes that frame from the whole drawing; so does Alt I.
line.keyframe_delete("points[0].pos", 20)
doc.frame_current = 1
doc.frame_current = 0
for path in every_channel(line, 3):
    assert frames(line, path) == [0, 10], (path, frames(line, path))
doc.frame_current = 20
line.set_point(2, (2.0, 1.0))
doc.frame_current = 0
assert frames(line, "points[1].pos") == [0, 10, 20]
doc.frame_current = 20
assert ops.anim.keyframe_delete() == {'FINISHED'}
doc.frame_current = 0
for path in every_channel(line, 3):
    assert frames(line, path) == [0, 10], (path, frames(line, path))

# A curve drawn later is keyed with the rest.
doc.frame_current = 0
line.add_curve([(0.0, 2.0), (1.0, 2.0)])
doc.frame_current = 10
line.set_point(3, (0.0, 3.0))
doc.frame_current = 0
assert frames(line, "points[4].pos") == [0, 10] and frames(line, "points[0].pos") == [0, 10]

# A point deleted takes its keys with it, and no frame from the others.
doc.frame_current = 0
line.add_curve([(5.0, 5.0), (6.0, 5.0), (7.0, 5.0)])
doc.frame_current = 1
doc.frame_current = 0
line.select_points([6])
assert ops.mesh.delete_points() == {'FINISHED'}
doc.frame_current = 1
doc.frame_current = 0
assert len(line.points) == 7, len(line.points)
assert frames(line, "points[0].pos") == [0, 10] and frames(line, "points[5].pos") == [0, 10]

# --- Off: each point keyed on its own (as files from before it open) ------------------------------------------------
ops.layer.new()
loose = layers.active
loose.key_whole_drawing = False
doc.frame_current = 0
loose.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
doc.frame_current = 10
loose.set_point(0, (0.0, 1.0))
loose.set_point(1, (1.0, 1.0))
loose.set_point(2, (2.0, 1.0))
loose.keyframe_set("points[0].pos", 10, interp="EASE_IN")
doc.frame_current = 15
loose.set_point(2, (2.0, 2.0))
doc.frame_current = 0
assert frames(loose, "points[0].pos") == [0, 10] and frames(loose, "points[2].pos") == [0, 10, 15]
assert frames(loose, "points[0].radius") == [], "off: untouched channels stay unkeyed"
before = {}
for f in (0, 10, 15):
    doc.frame_current = f
    before[f] = [tuple(p) for p in loose.points]
doc.frame_current = 0

# Turned on: every channel keyed on 0, 10 and 15, where it already is; frame 10's keys take the timing most have.
loose.key_whole_drawing = True
doc.frame_current = 1
doc.frame_current = 0
for path in every_channel(loose, 3):
    assert frames(loose, path) == [0, 10, 15], (path, frames(loose, path))
assert key(loose, "points[0].pos", 10)["interp"] == "SMOOTH", "most of frame 10's keys are Smooth"
for f in (0, 10, 15):
    doc.frame_current = f
    assert close([tuple(p) for p in loose.points], before[f]), (f, loose.points, before[f])

# --- Motion Path tool: no path for its points; its timing edited as a whole ----------------------------------------
layers.active = line
for other in layers:
    other.selected = other.id == line.id
line.select_points([0, 1, 2])
assert ops.wm.tool_motion_path() == {'FINISHED'}
labels = [p["label"] for p in oanim.wm.motion_paths()]
assert not any("point" in l for l in labels), ("no path for its points", labels)
timing = oanim.wm.motion_path_timing()
drawing_rows = [t for t in timing if t["label"] == line.name + " drawing"]
assert len(drawing_rows) == 1, [t["label"] for t in timing]
assert [(s["start"], s["end"]) for s in drawing_rows[0]["stretches"]] == [(0, 10)], drawing_rows[0]["stretches"]
assert line.name not in [t["label"] for t in timing], "no row of its own: the layer doesn't move itself"
panel = oanim.wm.motion_path_timing_panel()
assert panel and panel["label"] == line.name + " drawing", ("the graph shows the drawing's timing unpicked", panel)

args = {"layer": str(line.id), "drawing": "1"}
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", frame="5", **args) == {'FINISHED'}
first = key(line, "points[0].pos", 10)
assert first["interp"] == "CUSTOM", first
for path in every_channel(line, 5, [3, 2]):
    k = key(line, path, 10)
    assert k["interp"] == "CUSTOM" and close(k["speeds"], first["speeds"]), (path, k)
doc.frame_current = 5
assert close(line.points[1][:2], (1.0, 0.25)), ("eased: a quarter of the way at half time", line.points[1])
assert ops.anim.motion_path_slide(frame="5", progress="0.5", proportional="0", **args) == {'FINISHED'}
assert close(line.points[1][:2], (1.0, 0.5)), line.points[1]
assert close(key(line, "points[4].radius", 10)["speeds"], key(line, "points[0].pos", 10)["speeds"])
assert ops.anim.motion_path_timing_blend(frame="5", **args) == {'FINISHED'}
assert ops.anim.motion_path_dot_speed(frame="5", speed="1.5", **args) == {'FINISHED'}
assert close(key(line, "curves[1].points[1].handle_out", 10)["speeds"], key(line, "points[0].pos", 10)["speeds"])
loose.key_whole_drawing = False
assert ops.anim.motion_path_stretch_shape(shape="EVEN", frame="5", layer=str(loose.id), drawing="1") == {
    'CANCELLED'}, "only a whole drawing"

# --- Saved with the file -------------------------------------------------------------------------------------------
folder = tempfile.mkdtemp()
try:
    path = os.path.join(folder, "whole.oanim")
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
    kept = {l.name: l.key_whole_drawing for l in oanim.data.layers}
    assert kept[line.name] is True and kept[loose.name] is False, kept
finally:
    shutil.rmtree(folder, ignore_errors=True)

print("Key Whole Drawing tests passed")
