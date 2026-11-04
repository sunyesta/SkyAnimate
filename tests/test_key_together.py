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


def new_vec(points):
    ops.layer.new()
    l = layers.active
    l.add_curve(points)
    return l


def group_of(members):
    ids = [m.id for m in members]
    for l in layers.all():
        l.selected = l.id in ids
    assert ops.layer.group() == {'FINISHED'}
    return layers.active


# --- Nested: the inner group on first, then the outer one: the inner one's keys handed over, none lost ---------------
assert ops.wm.read_homefile() == {'FINISHED'}
doc = oanim.data.document
layers = oanim.data.layers
doc.auto_key = True
doc.auto_freeze_keys = False
na = new_vec([(0.0, 0.0), (1.0, 0.0)])
nb = new_vec([(0.0, 3.0), (1.0, 3.0)])
inner = group_of([na, nb])
nc = new_vec([(0.0, 6.0), (1.0, 6.0)])
outer = group_of([inner, nc])
doc.frame_current = 10
na.set_point(1, (1.0, 1.0))
doc.frame_current = 20
nc.set_point(0, (0.0, 7.0))
doc.frame_current = 0
inner.key_together = True
settle()
outer.key_together = True
settle()
settle()
for l in (na, nb, nc):
    assert frames(l, "points[0].pos") == [0, 10, 20], (l.name, frames(l, "points[0].pos"))
outer.key_together = False
settle()
settle()
assert frames(na, "points[1].pos") == [0, 10] and frames(nc, "points[0].pos") == [0, 20], (
    "the outer off: the inner one keys its own again", frames(na, "points[1].pos"), frames(nc, "points[0].pos"))
assert frames(nb, "points[0].pos") == [0, 10], frames(nb, "points[0].pos")
inner.key_together = False
settle()
assert frames(na, "points[1].pos") == [0, 10] and frames(nb, "points[0].pos") == [], frames(nb, "points[0].pos")
doc.frame_current = 20
assert abs(nc.points[0][1] - 7.0) < 1e-4, ("c's own pose kept", nc.points[0])
doc.frame_current = 0

# --- Fillers added in an action go too when it's turned off on the mainline -------------------------------------------
ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.select_none()
ops.armature.bone_add(name="leg", head_x="0", head_y="-1", tail_x="1", tail_y="-1", parent="")
hat = new_vec([(0.0, 5.0), (1.0, 5.0)])
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
layers.active = rig
rig.key_together = True
settle()
assert ops.anim.action_new(name="wave") == {'FINISHED'}
rig.select_bone("arm")
doc.frame_current = 6
assert ops.armature.rotate(angle="30") == {'FINISHED'}
doc.frame_current = 0
settle()
assert frames(rig, 'bones["leg"].angle') == [0, 6] and frames(hat, "position") == [0, 6]
assert ops.anim.action_set(name="") == {'FINISHED'}
settle()
rig.key_together = False
settle()
assert ops.anim.action_set(name="wave") == {'FINISHED'}
settle()
assert frames(rig, 'bones["arm"].angle') == [0, 6], frames(rig, 'bones["arm"].angle')
assert 6 not in frames(rig, 'bones["leg"].angle') and 6 not in frames(hat, "position"), (
    "the action's fillers let go", frames(rig, 'bones["leg"].angle'), frames(hat, "position"))
assert ops.anim.action_set(name="") == {'FINISHED'}

print("Key Together tests passed")
