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
a.key_together = False  # unfolded: keyed by its group, or channel by channel
a.add_curve([(0.0, 0.0), (1.0, 0.0)])
ops.layer.new()
b = layers.active
b.key_together = False  # unfolded: keyed by its group, or channel by channel
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
assert frames(b, "points[0].pos") == [0, 10, 20], ("b's own keys, its whole drawing, and the pose before its edit "
                                                   "held", frames(b, "points[0].pos"))
assert frames(b, "position") == [] and frames(group, "rotation") == [] and frames(a, "opacity") == []
assert frames(a, "points[0].pos") == [0, 10], "a's drawing keyed whole, on its own frames"
doc.frame_current = 5
assert abs(b.points[0][1] - 3.0) < 1e-4, ("b held at the pose before its edit, kept", b.points[0])
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
# Retimed where nothing moves (a's position, the group's size): shapes nothing, so they go all the same.
assert frames(a, "position") == [], ("still: the stretch 10-20 reshaped, nothing to shape", frames(a, "position"))
assert frames(group, "scale") == [], frames(group, "scale")

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
    l.key_together = False  # unfolded: keyed by its group, or channel by channel
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


# --- Turned on, nothing moves: fillers inside motion already there, or after a layer's last key, shape no curve -------
def poses(ls, at=(0, 3, 5, 7, 10, 13, 15, 17, 20, 25, 30)):
    now = doc.frame_current
    out = []
    for f in at:
        doc.frame_current = f
        out.append([tuple(round(c, 5) for p in l.points for c in p) for l in ls])
    doc.frame_current = now
    return out


assert ops.wm.read_homefile() == {'FINISHED'}
doc = oanim.data.document
layers = oanim.data.layers
doc.auto_key = True
doc.auto_freeze_keys = False
sa = new_vec([(0.0, 0.0), (1.0, 0.0)])
sb = new_vec([(0.0, 3.0), (1.0, 3.0)])
sgroup = group_of([sa, sb])
# Smooth keys: a filler on 20 after a's last key would bend a's curve into 10, one on 10 inside b's 0-20 would bend b's.
doc.frame_current = 10
sa.set_point(1, (1.0, 1.0))
doc.frame_current = 20
sb.set_point(0, (0.0, 4.0))
# A third key, so b's stretch into 20 isn't even (a filler at its middle would then bend it).
doc.frame_current = 24
sb.set_point(0, (0.0, 7.0))
doc.frame_current = 0
assert key(sa, "points[1].pos", 10)["interp"] == "SMOOTH" and key(sb, "points[0].pos", 20)["interp"] == "SMOOTH"
before = poses([sa, sb])
sgroup.key_together = True
settle()
assert frames(sa, "points[1].pos") == [0, 10, 20, 24] and frames(sb, "points[0].pos") == [0, 10, 20, 24]
assert poses([sa, sb]) == before, "turned on, nothing moves"

# Keyed while on, a layer still waits for the rest: its fillers before its new key hold it.
doc.frame_current = 30
sa.set_point(1, (1.0, 3.0))
doc.frame_current = 0
doc.frame_current = 15
assert abs(sa.points[1][1] - 1.0) < 1e-4, ("a holds its pose from 10 to 24, then moves", sa.points[1])
doc.frame_current = 22
assert abs(sa.points[1][1] - 1.0) < 1e-4, sa.points[1]
doc.frame_current = 27
assert sa.points[1][1] > 1.2, sa.points[1]
doc.frame_current = 0

# --- Layers joining and leaving while it is on -----------------------------------------------------------------------
assert ops.wm.read_homefile() == {'FINISHED'}
doc = oanim.data.document
layers = oanim.data.layers
doc.auto_key = True
doc.auto_freeze_keys = False


def top_vec(points):
    l = new_vec(points)
    assert ops.layer.move_to_group(group="0") == {'FINISHED'}
    return l


def together_group(name):
    """a and b grouped, Key Together on, a keyed on 10 and 20."""
    doc.frame_current = 0
    a = top_vec([(0.0, 0.0), (1.0, 0.0)])
    b = top_vec([(0.0, 3.0), (1.0, 3.0)])
    g = group_of([a, b])
    g.name = name
    g.key_together = True
    doc.frame_current = 10
    a.set_point(1, (1.0, 1.0))
    doc.frame_current = 20
    a.set_point(1, (1.0, 2.0))
    doc.frame_current = 0
    settle()
    return g, a, b


# An animated layer moved in (its drawing keyed as a whole, on 5): it misses the group's frames without any having
# been deleted, so it gets them, and the group gets its 5.
g, a, b = together_group("in")
c = top_vec([(0.0, 6.0), (1.0, 6.0)])
doc.frame_current = 5
c.set_point(0, (0.5, 6.0))
doc.frame_current = 0
settle()
layers.active = c
assert ops.layer.move_to_group(group=str(g.id)) == {'FINISHED'}
settle()
assert frames(a, "points[1].pos") == [0, 5, 10, 20], ("the group's keys stay", frames(a, "points[1].pos"))
assert frames(c, "points[0].pos") == [0, 5, 10, 20], frames(c, "points[0].pos")
assert frames(b, "position") == [0, 5, 10, 20], frames(b, "position")
doc.frame_current = 10
assert abs(a.points[1][1] - 1.0) < 1e-4, a.points[1]
doc.frame_current = 0
# Once in, a key deleted on it takes the frame from them all.
assert ops.anim.keys_deselect_all() == {'FINISHED'}
assert ops.anim.keys_select(frame_start="5", layers=str(c.id)) == {'FINISHED'}
assert ops.anim.keys_delete() == {'FINISHED'}
settle()
assert frames(a, "points[1].pos") == [0, 10, 20], frames(a, "points[1].pos")

# A new layer made inside it (a pasted or new one): its drawing, keyed as a whole by itself first, is still new here.
g, a, b = together_group("new")
layers.active = a
n = new_vec([(0.0, 9.0), (1.0, 9.0)])
settle()
doc.frame_current = 3
doc.frame_current = 0
assert frames(a, "points[1].pos") == [0, 10, 20], ("a new layer inside deletes nothing", frames(a, "points[1].pos"))
assert frames(n, "position") == [0, 10, 20], frames(n, "position")
doc.frame_current = 15
n.set_point(0, (2.0, 9.0))
doc.frame_current = 0
settle()
assert frames(a, "points[1].pos") == [0, 10, 15, 20], frames(a, "points[1].pos")

# Moved out: the fillers it never edited go, as when the group is turned off; its own keys stay.
g, a, b = together_group("out")
doc.frame_current = 30
b.set_point(0, (0.0, 4.0))
doc.frame_current = 0
settle()
assert frames(b, "position") == [0, 10, 20, 30]
assert ops.anim.keys_deselect_all() == {'FINISHED'}  # an undo step to come back to
layers.active = b
assert ops.layer.move_to_group(group="0") == {'FINISHED'}
settle()
assert frames(b, "position") == [], ("its fillers go", frames(b, "position"))
assert frames(b, "points[0].pos") == [0, 20, 30], ("its own key stays, the pose before it held",
                                                   frames(b, "points[0].pos"))
assert frames(a, "points[1].pos") == [0, 10, 20, 30], ("the group's stay", frames(a, "points[1].pos"))
assert ops.ed.undo() == {'FINISHED'}
settle()
assert frames(b, "position") == [0, 10, 20, 30], ("undo puts it and its fillers back", frames(b, "position"))
layers.active = b
assert ops.layer.move_to_group(group="0") == {'FINISHED'}
settle()
doc.frame_current = 15
a.set_point(1, (1.0, 1.5))
doc.frame_current = 0
settle()
assert 15 not in frames(b, "points[0].pos"), "out, it isn't keyed with them"

# Moved from one group that is on to another: it lets go of the first's fillers and takes the second's frames.
g1, a1, b1 = together_group("from")
g2, a2, b2 = together_group("to")
doc.frame_current = 7
a2.set_point(1, (1.0, 0.5))
doc.frame_current = 0
settle()
layers.active = b1
assert ops.layer.move_to_group(group=str(g2.id)) == {'FINISHED'}
settle()
assert frames(b1, "position") == [0, 7, 10, 20], frames(b1, "position")
assert frames(a2, "points[1].pos") == [0, 7, 10, 20], frames(a2, "points[1].pos")
assert frames(a1, "points[1].pos") == [0, 10, 20], frames(a1, "points[1].pos")
doc.frame_current = 7
assert abs(a2.points[1][1] - 0.5) < 1e-4, a2.points[1]
doc.frame_current = 0

# A group moved in: its layers join, keys and all.
g, a, b = together_group("group in")
x = top_vec([(0.0, 6.0), (1.0, 6.0)])
y = top_vec([(0.0, 7.0), (1.0, 7.0)])
inner = group_of([x, y])
doc.frame_current = 30
x.set_point(0, (3.0, 6.0))
doc.frame_current = 0
settle()
layers.active = inner
assert ops.layer.move_to_group(group=str(g.id)) == {'FINISHED'}
settle()
assert frames(a, "points[1].pos") == [0, 10, 20, 30], frames(a, "points[1].pos")
assert frames(x, "points[0].pos") == [0, 10, 20, 30], frames(x, "points[0].pos")
assert frames(y, "position") == [0, 10, 20, 30], frames(y, "position")

# A group that is on moved into another that is on: handed over, its layers are new there, so the frames they miss
# (the other's 7) are given to them, not deleted from all; and nothing of theirs moves.
g1, a1, b1 = together_group("on in")
g2, a2, b2 = together_group("on to")
doc.frame_current = 7
a2.set_point(1, (1.0, 0.5))
doc.frame_current = 0
settle()
doc.frame_current = 15
before = (a1.points[1][1], b1.points[0][1])
doc.frame_current = 0
layers.active = g1
assert ops.layer.move_to_group(group=str(g2.id)) == {'FINISHED'}
settle()
assert frames(a2, "points[1].pos") == [0, 7, 10, 20], ("no frame deleted", frames(a2, "points[1].pos"))
assert frames(a1, "points[1].pos") == [0, 7, 10, 20], frames(a1, "points[1].pos")
doc.frame_current = 15
assert all(abs(x - y) < 1e-4 for x, y in zip((a1.points[1][1], b1.points[0][1]), before)), ("nothing moves", (a1.points[1][1], b1.points[0][1]), before)
doc.frame_current = 0

# A group inside one that is on, saved and opened: it handed its keys over, so opening doesn't make it let go of its
# fillers (the outer group would make them again, holding something else).
g, a, b = together_group("inner")
doc.frame_current = 30
b.set_point(0, (0.0, 5.0))
doc.frame_current = 0
settle()
assert frames(b, "points[0].pos") == [0, 10, 20, 30]
doc.frame_current = 10
held = b.points[0][1]
doc.frame_current = 0
assert abs(held - 3.0) < 1e-4, ("its filler on 10 holds it where it was until then", held)
c = top_vec([(0.0, 8.0), (1.0, 8.0)])
outer = group_of([g, c])
outer.key_together = True
settle()
doc.frame_current = 10
assert abs(b.points[0][1] - held) < 1e-4, ("handed over, nothing moves", b.points[0], held)
doc.frame_current = 0
folder = tempfile.mkdtemp()
path = os.path.join(folder, "nested.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
doc = oanim.data.document
layers = oanim.data.layers
b = next(l for l in layers.all() if l.id == b.id)
settle()
doc.frame_current = 10
assert abs(b.points[0][1] - held) < 1e-4, ("opened, it holds the same", b.points[0])
doc.frame_current = 0
assert frames(b, "points[0].pos") == [0, 10, 20, 30], frames(b, "points[0].pos")
shutil.rmtree(folder, ignore_errors=True)

print("Key Together tests passed")
