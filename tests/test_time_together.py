"""Time Together (the timeline's chain toggle, oanim.data.document.time_together): changing the timing of keys changes
it the same way on the linked channels, those of the selected layers (each one's move, turn, size, shear and opacity
and its drawing's shape) and selected bones keyed on both ends of the stretch edited, from the unmoved key before to
the unmoved key after. Moving keys takes the linked channels' keys through the same stretch (a key halfway stays
halfway), rounded to whole frames and kept a frame apart. Ctrl (ripple=1): the keys after go along. Alt (together=0):
just the one. Saved with the file.

Run with:  OpenAnimate --background --python tests/test_time_together.py
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


def frames(layer, path="position"):
    try:
        return [k["frame"] for k in layer.keyframes(path)]
    except KeyError:
        return []


def keyed(at):
    """A new layer, its move keyed on frames `at` (and 0)."""
    doc.frame_current = 0
    ops.layer.new()
    l = layers.active
    for i, f in enumerate(at):
        doc.frame_current = f
        l.location_x = float(i + 1)
    doc.frame_current = 0
    return l


def only_selected(*ls):
    ids = [l.id for l in ls]
    for l in layers.all():
        l.selected = l.id in ids


def pick(layer, frame, path="position"):
    assert ops.anim.keys_select(frame_start=str(frame), layers=str(layer.id), path=path) == {'FINISHED'}


def fresh():
    assert ops.wm.read_homefile() == {'FINISHED'}
    global doc, layers
    doc = oanim.data.document
    layers = oanim.data.layers
    doc.auto_key = True
    doc.auto_freeze_keys = False


# --- Linked channels follow through the same stretch -----------------------------------------------------------------
fresh()
a = keyed([10, 20])
b = keyed([5, 15, 20])
off = keyed([10, 21])
apart = keyed([5, 20])
only_selected(a, b, off)
assert not doc.time_together, "off for new documents"
pick(a, 10)
assert oanim.wm.time_together_linked() == [], "nothing linked while it is off"
doc.time_together = True
linked = oanim.wm.time_together_linked()
assert (b.id, "position") in linked, linked
assert (off.id, "position") not in linked, "a frame off at the end: not timed with it"
assert (apart.id, "position") not in linked, "an unselected layer is never linked"
assert (a.id, "position") not in linked, "the edited channel itself isn't marked"
assert ops.anim.keys_offset(frames="2") == {'FINISHED'}
assert frames(a) == [0, 12, 20], frames(a)
# 0..10..20 became 0..12..20: b's 5 is halfway to the moved key (6), its 15 halfway after it (16).
assert frames(b) == [0, 6, 16, 20], ("through the same stretch", frames(b))
assert frames(off) == [0, 10, 21] and frames(apart) == [0, 5, 20]
assert ops.ed.undo() == {'FINISHED'}
assert frames(a) == [0, 10, 20] and frames(b) == [0, 5, 15, 20], ("one undo step", frames(a), frames(b))

# Alt (together=0): just the one.
pick(a, 10)
assert ops.anim.keys_offset(frames="2", together="0") == {'FINISHED'}
assert frames(a) == [0, 12, 20] and frames(b) == [0, 5, 15, 20], frames(b)
assert ops.ed.undo() == {'FINISHED'}

# Off: just the one.
doc.time_together = False
pick(a, 10)
assert ops.anim.keys_offset(frames="2") == {'FINISHED'}
assert frames(b) == [0, 5, 15, 20], frames(b)
assert ops.ed.undo() == {'FINISHED'}
doc.time_together = True

# Scaling the spacing goes through the stretch the same way.
pick(a, 10)
doc.frame_current = 0
assert ops.anim.keys_scale(factor="0.5") == {'FINISHED'}
assert frames(a) == [0, 5, 20], frames(a)
assert frames(b) == [0, 3, 13, 20], ("scaled through the same stretch", frames(b))
assert ops.ed.undo() == {'FINISHED'}

# The last key moved: the stretch ends on it, and the linked keys after it go along by as much.
c = keyed([10, 20, 30])
d = keyed([10, 20, 25])
only_selected(c, d)
pick(c, 20)
pick(c, 30)
assert ops.anim.keys_select(frame_start="20", frame_end="30", layers=str(c.id), path="position") == {'FINISHED'}
assert (d.id, "position") not in oanim.wm.time_together_linked(), "keyed apart at the end: not linked"

# --- Whole frames, a frame apart -------------------------------------------------------------------------------------
fresh()
doc.time_together = True
a = keyed([10, 20])
b = keyed([8, 9, 10, 20])
only_selected(a, b)
pick(a, 10)
assert ops.anim.keys_offset(frames="-9") == {'FINISHED'}
assert frames(a) == [0, 1, 20], frames(a)
assert frames(b) == [0, 1, 2, 3, 20], ("squeezed onto whole frames, a frame apart, none lost", frames(b))
assert [k["value"][0] for k in b.keyframes("position")] == [0.0, 1.0, 2.0, 3.0, 4.0], "the poses kept in order"

# --- Ripple (Ctrl): the keys after go along --------------------------------------------------------------------------
fresh()
doc.time_together = True
a = keyed([10, 20, 30])
b = keyed([4, 10, 25])
only_selected(a, b)
pick(a, 10)
assert ops.anim.keys_offset(frames="3", ripple="1") == {'FINISHED'}
assert frames(a) == [0, 13, 23, 33], ("the keys after went along", frames(a))
assert frames(b) == [0, 5, 13, 28], ("linked on 0..10: its 4 through the stretch, the rest along", frames(b))
assert ops.ed.undo() == {'FINISHED'}
pick(a, 10)
assert ops.anim.keys_offset(frames="3") == {'FINISHED'}
assert frames(a) == [0, 13, 20, 30], ("Squeeze: the keys after stay", frames(a))
assert ops.ed.undo() == {'FINISHED'}

# --- Bones: only the selected ones -----------------------------------------------------------------------------------
fresh()
doc.time_together = True
lead = keyed([10, 20])
ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.select_none()
ops.armature.bone_add(name="leg", head_x="0", head_y="-1", tail_x="1", tail_y="-1", parent="")
for name, at in (("arm", (5, 20)), ("leg", (5, 20))):
    ops.armature.select_none()
    rig.select_bone(name)
    for f in at:
        doc.frame_current = f
        assert ops.armature.rotate(angle="10") == {'FINISHED'}
    doc.frame_current = 0
ops.armature.select_none()
rig.select_bone("leg")
only_selected(lead, rig)
pick(lead, 10)
linked = oanim.wm.time_together_linked()
assert (rig.id, 'bones["leg"].angle') in linked, linked
assert (rig.id, 'bones["arm"].angle') not in linked, ("an unselected bone isn't linked", linked)
assert ops.anim.keys_offset(frames="10") == {'FINISHED'}
assert frames(rig, 'bones["leg"].angle') == [0, 10, 20], frames(rig, 'bones["leg"].angle')
assert frames(rig, 'bones["arm"].angle') == [0, 5, 20], frames(rig, 'bones["arm"].angle')

# --- Saved with the file ---------------------------------------------------------------------------------------------
folder = tempfile.mkdtemp()
try:
    path = os.path.join(folder, "together.oanim")
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
    assert oanim.data.document.time_together is True
finally:
    shutil.rmtree(folder, ignore_errors=True)

print("Time Together tests passed")
