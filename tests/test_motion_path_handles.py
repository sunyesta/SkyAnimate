"""Motion paths, stage 3: path handles on position keys (a layer's origin, a bone's head, a point). Auto until dragged,
when the path runs as it always has; dragging one bends the path into and out of its key, the other staying in line;
Alt (broken=1) breaks the pair; Auto Handles puts them back. Saved with the file.

Run with:  OpenAnimate --background --python tests/test_motion_path_handles.py
"""

import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return all(abs(x - y) < eps for x, y in zip(a, b))
    return abs(a - b) < eps


def key(layer, channel, frame):
    return next(k for k in layer.keyframes(channel) if k["frame"] == frame)


def where(layer, frame):
    doc.frame_current = frame
    return (layer.location_x, layer.location_y)


# --- A layer's origin keyed in a straight line, evenly: 0.2 a frame ------------------------------------------------
doc.auto_key = True
ops.layer.new()
box = layers.active
box.add_curve([(0.0, 0.0), (0.5, 0.0)])
doc.motion_paths = True
args = dict(layer=str(box.id))
# Keyed through an operator, so each is an undo step.
for frame, x in ((10, 2.0), (20, 4.0), (30, 6.0)):
    assert ops.anim.motion_path_move(frame=str(frame), x=str(x), y="0", **args) == {'FINISHED'}
before = {f: where(box, f) for f in range(10, 31)}

k = key(box, "position", 20)
assert k["path_handles"] == "AUTO", k
assert close(k["path_out"], (2.0 / 3.0, 0.0)) and close(k["path_in"], (-2.0 / 3.0, 0.0)), "Smooth's slope, a third"

# --- Dragging the out handle: the path bends out of the key, the key stays put ----------------------------------------
doc.frame_current = 20
assert ops.anim.motion_path_handle(frame="20", side="OUT", x="4", y="1", **args) == {'FINISHED'}
k = key(box, "position", 20)
assert k["path_handles"] == "ALIGNED" and close(k["path_out"], (0.0, 1.0)), k
assert close(k["path_in"], (0.0, -2.0 / 3.0)), "the in handle turns to stay in line, keeping its length"
assert close(k["value"], (4.0, 0.0)) and close(where(box, 20), (4.0, 0.0))
# Frame 25 is half way through a Smooth stretch: the Bezier (4,0) (4,1) (5.333,0) (6,0) at a half.
assert close(where(box, 25), (4.75, 0.375)), where(box, 25)
assert where(box, 15)[1] < -0.05, "and into it from below"
assert all(close(where(box, f), before[f]) for f in (10, 30)), "the keys either side stay"
ops.ed.undo()
assert key(box, "position", 20)["path_handles"] == "AUTO" and close(where(box, 25), before[25]), "one undo step"
ops.ed.redo()
assert close(where(box, 25), (4.75, 0.375))

# --- Alt: the in handle alone, broken ---------------------------------------------------------------------------------
assert ops.anim.motion_path_handle(frame="20", side="IN", x="3.5", y="0.5", broken="1", **args) == {'FINISHED'}
k = key(box, "position", 20)
assert k["path_handles"] == "BROKEN" and close(k["path_in"], (-0.5, 0.5)) and close(k["path_out"], (0.0, 1.0)), k
assert ops.anim.motion_path_handle(frame="20", side="IN", x="3", y="0.5", **args) == {'FINISHED'}
k = key(box, "position", 20)
assert k["path_handles"] == "BROKEN" and close(k["path_out"], (0.0, 1.0)), "broken stays broken"

# --- Other modes go along the curve by its length: Linear at an even speed --------------------------------------------
box.keyframe_set("position", 30, interp="LINEAR")
points = [where(box, f) for f in range(20, 31)]
steps = [math.dist(points[i], points[i + 1]) for i in range(10)]
assert max(steps) - min(steps) < 0.04 * max(steps), steps
assert points[5][1] > 0.1, "still along the bent path"
box.keyframe_set("position", 30, interp="SMOOTH")

# --- Saved with the file ----------------------------------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_path_handles_")) / "handles.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
doc = oanim.data.document
layers = oanim.data.layers
box = next(l for l in layers if l.id == int(args["layer"]))
k = key(box, "position", 20)
assert k["path_handles"] == "BROKEN" and close(k["path_in"], (-1.0, 0.5)) and close(k["path_out"], (0.0, 1.0)), k
moved_25 = where(box, 25)
assert close(moved_25, (4.75, 0.375)), moved_25

# --- Auto Handles: back as it was ------------------------------------------------------------------------------------
for l in layers:
    l.selected = l.id == box.id
layers.active = box
# Key 10's handles dragged too, but it isn't selected: they stay.
assert ops.anim.motion_path_handle(frame="10", side="OUT", x="3", y="-1", broken="1", **args) == {'FINISHED'}
assert ops.anim.motion_path_select(frame="20", **args) == {'FINISHED'}
assert ops.anim.motion_path_handles_auto() == {'FINISHED'}
assert key(box, "position", 20)["path_handles"] == "AUTO"
assert key(box, "position", 10)["path_handles"] == "BROKEN", "only the selected keys"
assert all(close(where(box, f), before[f]) for f in range(20, 31)), "the path runs as before"
assert ops.anim.motion_path_handles_auto() == {'CANCELLED'}, "nothing left to put back"

# --- A bone: its head's path has handles, in document space through the bone layer's move; its tip's has none --------
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
rig.location_x = 1.0
rig.rotation = 90.0
assert ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="") == {'FINISHED'}
for frame, x in ((10, 1.0), (20, 2.0), (30, 3.0)):
    doc.frame_current = frame
    rig.bone_set("arm", position=(x, 0.0))
rig.select_bone("arm")
bone_args = dict(layer=str(rig.id), bone="arm")
assert ops.anim.motion_path_handle(frame="20", side="OUT", x="3", y="1", **bone_args) == {'CANCELLED'}, "a tip turns"
ops.anim.motion_path_style(head="1", **bone_args)
# The layer is 1 across and turned a quarter: the head shows at (1, 2) on frame 20, moving up the page. Its out handle
# to (0.5, 2), half a unit left, is half a unit across the bone layer's own y.
assert ops.anim.motion_path_handle(frame="20", side="OUT", x="0.5", y="2", **bone_args) == {'FINISHED'}
k = key(rig, 'bones["arm"].position', 20)
assert k["path_handles"] == "ALIGNED" and close(k["path_out"], (0.0, 0.5)), k
doc.frame_current = 25
head = next(p for p in oanim.wm.motion_paths() if p["label"] == "arm head")
assert head["points"][25 - head["first"]][0] < 0.9, "the head's path bends left"

print("Motion path handle tests passed")
