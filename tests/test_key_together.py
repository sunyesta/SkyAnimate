"""Key Together: a group's layers keyed as one movement. Turned on, every layer in the group (its move, turn, size and
opacity, its bones and its drawing's points) is keyed wherever any is, the keys added only to hold a layer in place
being fillers; an edit keys them all; the timing chart has one "<group> together" row whose timing edits reach them
all. Turned off, the fillers never edited go (a whole drawing's keys on a frame stay together), and those edited (their
value or the timing either side) stay. Reversible: on again adds them again; undo puts them back.

Run with:  OpenAnimate --background --python tests/test_key_together.py
"""

import os
import shutil
import tempfile

import oanim

ops = oanim.ops
layers = oanim.data.layers
doc = oanim.data.document
doc.auto_key = True
doc.auto_freeze_keys = False


def frames(layer, path):
    try:
        return [k["frame"] for k in layer.keyframes(path)]
    except KeyError:
        return []


def key(layer, path, frame):
    return next(k for k in layer.keyframes(path) if k["frame"] == frame)


def settle():
    doc.frame_current = doc.frame_current + 1
    doc.frame_current = doc.frame_current - 1


ops.layer.new()
a = layers.active
a.add_curve([(0.0, 0.0), (1.0, 0.0)])
ops.layer.new()
b = layers.active
b.add_curve([(0.0, 3.0), (1.0, 3.0)])
for l in layers:
    l.selected = l.id in (a.id, b.id)
assert ops.layer.group() == {'FINISHED'}
group = layers.active
assert group.id not in (a.id, b.id)
assert not group.key_together, "off for new groups"

# Animated apart, then turned on: b is held in place on a's frames, and the group with them.
doc.frame_current = 10
a.set_point(1, (1.0, 1.0))
doc.frame_current = 0
assert frames(a, "points[1].pos") == [0, 10] and frames(b, "points[0].pos") == []
group.key_together = True
settle()
assert frames(b, "points[0].pos") == [0, 10] and frames(b, "position") == [0, 10], frames(b, "points[0].pos")
assert frames(group, "rotation") == [0, 10] and frames(a, "opacity") == [0, 10]
doc.frame_current = 5
assert abs(b.points[1][1] - 3.0) < 1e-4, ("held where it was", b.points[1])
doc.frame_current = 0

# An edit on one layer keys them all.
doc.frame_current = 20
b.set_point(0, (0.0, 4.0))
doc.frame_current = 0
assert frames(a, "points[0].pos") == [0, 10, 20] and frames(group, "position") == [0, 10, 20]

# The timing chart: one row for the group, none of its layers'.
layers.active = a
for l in layers:
    l.selected = l.id == a.id
a.select_points([0, 1])
assert ops.wm.tool_motion_path() == {'FINISHED'}
labels = [t["label"] for t in oanim.wm.motion_path_timing()]
assert labels == [group.name + " together"], labels
row = oanim.wm.motion_path_timing()[0]
assert [(s["start"], s["end"]) for s in row["stretches"]] == [(0, 10), (10, 20)], row["stretches"]

# Turned off: the fillers go, a's and b's own keys stay.
group.key_together = False
settle()
assert frames(a, "points[1].pos") == [0, 10], frames(a, "points[1].pos")
assert frames(b, "points[0].pos") == [0, 20], ("b's own keys, its whole drawing", frames(b, "points[0].pos"))
assert frames(b, "position") == [] and frames(group, "rotation") == [] and frames(a, "opacity") == []
assert frames(a, "points[0].pos") == [0, 10], "a's drawing keyed whole, on its own frames"
doc.frame_current = 5
assert abs(b.points[0][1] - 3.25) < 0.2, ("b moves straight to its key again", b.points[0])
doc.frame_current = 0
# On again adds them again.
group.key_together = True
settle()
assert frames(b, "position") == [0, 10, 20] and frames(a, "points[1].pos") == [0, 10, 20]

# Edited while on, a filler stays: b's value at 10, and the timing of the stretch into 10 (the group's row).
doc.frame_current = 10
b.set_point(1, (1.0, 5.0))
doc.frame_current = 0
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", frame="15", layer=str(group.id), drawing="1") == {
    'FINISHED'}
assert key(a, "position", 20)["interp"] == "CUSTOM" and key(b, "points[0].pos", 20)["interp"] == "CUSTOM"
group.key_together = False
settle()
assert frames(b, "points[1].pos") == [0, 10, 20], ("edited at 10: stays", frames(b, "points[1].pos"))
assert frames(a, "points[1].pos") == [0, 10, 20], ("its stretch into 20 reshaped: stays", frames(a, "points[1].pos"))
assert frames(a, "position") == [0, 10, 20], ("the stretch 10-20 reshaped: 10 and 20 stay", frames(a, "position"))
assert frames(group, "scale") == [0, 10, 20], frames(group, "scale")

# --- One key deleted takes the frame from them all ------------------------------------------------------------------
group.key_together = True
settle()
doc.frame_current = 30
a.set_point(0, (0.5, 0.0))
doc.frame_current = 0
assert frames(b, "points[0].pos")[-1] == 30
a.keyframe_delete("points[0].pos", 30)
settle()
assert 30 not in frames(b, "points[0].pos") and 30 not in frames(group, "position"), frames(b, "points[0].pos")

# --- Saved with the file, fillers and all -----------------------------------------------------------------------------
doc.frame_current = 40
a.set_point(0, (0.0, 0.5))
doc.frame_current = 0
assert 40 in frames(b, "position")
folder = tempfile.mkdtemp()
try:
    path = os.path.join(folder, "together.oanim")
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
    group = next(l for l in oanim.data.layers.all() if l.name == group.name)
    b = next(l for l in oanim.data.layers.all() if l.name == b.name)
    assert group.key_together
    group.key_together = False
    settle()
    assert 40 not in frames(b, "position"), ("its fillers known after opening", frames(b, "position"))

    # One point of a whole drawing edited on a frame: all its points' keys there stay (its drawing keyed whole).
    a = next(l for l in oanim.data.layers.all() if l.name == a.name)
    group.key_together = True
    settle()
    doc.frame_current = 50
    a.set_point(0, (0.0, 2.0))
    doc.frame_current = 51
    doc.frame_current = 50
    assert 50 in frames(b, "points[0].pos")
    b.set_point(1, (1.0, 6.0))
    doc.frame_current = 0
    group.key_together = False
    settle()
    assert 50 in frames(b, "points[1].pos") and 50 in frames(b, "points[0].pos"), frames(b, "points[0].pos")
    assert 50 not in frames(b, "position"), frames(b, "position")
finally:
    shutil.rmtree(folder, ignore_errors=True)

print("Key Together tests passed")
