"""Time Together (the timeline's chain toggle, oanim.data.document.time_together): changing the timing of keys changes
it the same way on the linked channels, those of the selected layers (each one's move, turn, size, shear and opacity
and its drawing's shape) and selected bones keyed on both ends of the stretch edited, from the unmoved key before to
the unmoved key after. Moving keys takes the linked channels' keys through the same stretch (a key halfway stays
halfway), rounded to whole frames and kept a frame apart. Ctrl (ripple=1): the keys after go along. Alt (together=0):
just the one. Saved with the file.

Run with:  OpenAnimate --background --python tests/test_time_together.py
"""

import math
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

# --- Timing edits: the linked keys take the same interpolation, hold and type --------------------------------------
fresh()
doc.time_together = True
a = keyed([10, 20])
same = keyed([10, 20])
inner = keyed([5, 10, 20])
apart = keyed([10, 20])
only_selected(a, same, inner)


def interp(l, f):
    return next(k for k in l.keyframes("position") if k["frame"] == f)["interp"]


pick(a, 10)
assert ops.anim.keys_interpolation_set(interp="LINEAR") == {'FINISHED'}
assert interp(a, 10) == "LINEAR"
assert interp(same, 10) == "LINEAR", "keyed on the same frames: timed alike"
assert interp(inner, 10) == "SMOOTH", "a key between: not the same stretch"
assert interp(apart, 10) == "SMOOTH", "an unselected layer: not linked"
assert ops.anim.keys_interpolation_set(type="BREAKDOWN", hold="2") == {'FINISHED'}
k = next(k for k in same.keyframes("position") if k["frame"] == 10)
assert k["type"] == "BREAKDOWN" and k["hold"] == 2, k
assert ops.ed.undo() == {'FINISHED'}
assert ops.ed.undo() == {'FINISHED'}
assert interp(same, 10) == "SMOOTH", "one undo step each"
pick(a, 10)
assert ops.anim.keys_interpolation_set(interp="EASE_IN", together="0") == {'FINISHED'}
assert interp(a, 10) == "EASE_IN" and interp(same, 10) == "SMOOTH", "together=0 (Alt): just the one"
doc.time_together = False
assert ops.anim.keys_interpolation_set(interp="STEP") == {'FINISHED'}
assert interp(same, 10) == "SMOOTH", "off: just the one"
doc.time_together = True

# --- Sliding a stretch (dragging between two selected keys, slide=1): it keeps its length, a frame short ----------
fresh()
doc.time_together = True
a = keyed([10, 20, 30, 40])
b = keyed([10, 25, 40])
only_selected(a, b)
assert ops.anim.keys_select(frame_start="20", frame_end="30", layers=str(a.id), path="position") == {'FINISHED'}
assert ops.anim.keys_offset(frames="15", slide="1") == {'FINISHED'}
assert frames(a) == [0, 10, 29, 39, 40], ("stopped a frame short of the key after", frames(a))
# b is keyed on 10 and 40, the ends of the stretch edited: its 25, halfway between 20 and 30, stays halfway (34).
assert frames(b) == [0, 10, 34, 40], frames(b)
assert ops.ed.undo() == {'FINISHED'}
assert ops.anim.keys_select(frame_start="20", frame_end="30", layers=str(a.id), path="position") == {'FINISHED'}
assert ops.anim.keys_offset(frames="-15", slide="1") == {'FINISHED'}
assert frames(a) == [0, 10, 11, 21, 40], ("a frame short of the key before", frames(a))
assert ops.ed.undo() == {'FINISHED'}
assert ops.anim.keys_select(frame_start="20", frame_end="30", layers=str(a.id), path="position") == {'FINISHED'}
assert ops.anim.keys_offset(frames="15", slide="1", ripple="1") == {'FINISHED'}
assert frames(a) == [0, 10, 35, 45, 55], ("with Ripple the key after goes along: no stop", frames(a))

# --- + and - (Cascadeur's): a frame more or fewer in the stretch under the playhead ------------------------------
fresh()
doc.time_together = True
a = keyed([10, 20, 30])
b = keyed([10, 15, 20])
only_selected(a, b)
layers.active = a
doc.frame_current = 12
assert ops.anim.span_frame_insert() == {'FINISHED'}
assert frames(a) == [0, 10, 21, 31], ("a frame more, the keys after going along", frames(a))
assert frames(b) == [0, 10, 16, 21], ("linked on 10 and 20: its 15 through the stretch", frames(b))
assert ops.anim.span_frame_remove() == {'FINISHED'}
assert frames(a) == [0, 10, 20, 30] and frames(b) == [0, 10, 15, 20], (frames(a), frames(b))
assert ops.anim.span_frames_add(amount="3", together="0") == {'FINISHED'}
assert frames(a) == [0, 10, 23, 33] and frames(b) == [0, 10, 15, 20], ("together=0: just the one", frames(b))
assert ops.ed.undo() == {'FINISHED'}
assert frames(a) == [0, 10, 20, 30], ("one undo step", frames(a))
c = keyed([10, 11])
layers.active = c
doc.frame_current = 10
assert ops.anim.span_frame_remove() == {'CANCELLED'}, "never below one frame"
assert frames(c) == [0, 10, 11]

# --- Match Timing: the active layer's frames, stretched over each one's own keys ---------------------------------
fresh()
lead = keyed([10, 20])
same_count = keyed([5, 30])
more = keyed([3, 6, 9])
only_selected(lead, same_count, more)
layers.active = lead
before = [k["value"] for k in same_count.keyframes("position")]
assert ops.anim.match_timing() == {'FINISHED'}
assert frames(same_count) == [0, 15, 30], ("0..10..20 stretched over 0..30", frames(same_count))
assert [k["value"] for k in same_count.keyframes("position")] == before, "as many keys: poses kept"
assert frames(more) == [0, 5, 9], ("stretched over 0..9, each key to the nearest", frames(more))
assert frames(lead) == [0, 10, 20], "the active layer is the source"
doc.time_together = True
pick(lead, 10)
assert (same_count.id, "position") not in oanim.wm.time_together_linked(), "a frame range of its own"
assert ops.ed.undo() == {'FINISHED'}
assert ops.ed.undo() == {'FINISHED'}
assert frames(same_count) == [0, 5, 30], ("one undo step", frames(same_count))
only_selected(lead)
assert ops.anim.match_timing() == {'CANCELLED'}, "nothing else selected"
doc.time_together = True

# --- Path brushes: a shape brush keeps the timing, so the linked layer stays; a speed brush retimes it too ----------
def path_mover(keys):
    ops.layer.new()
    l = layers.active
    l.add_curve([(0.0, 0.0), (0.1, 0.0)])
    l.key_whole_drawing = False
    doc.frame_current = 0
    l.location_x, l.location_y = -1.0, -1.0
    for o in layers:
        o.selected = o.id == l.id
    layers.active = l
    for f, (x, y) in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(l.id)) == {'FINISHED'}
    return l


def poses(l, a=0, b=35):
    out = []
    for f in range(a, b + 1):
        doc.frame_current = f
        out.append((l.location_x, l.location_y))
    return out


assert ops.wm.read_homefile() == {'FINISHED'}
doc.auto_key = False
doc.motion_paths = True
doc.path_before = doc.path_after = 60
assert ops.wm.tool_motion_path() == {'FINISHED'}
pa = path_mover({10: (0.0, 0.0), 20: (1.0, 0.5), 30: (5.0, 0.0)})
pb = path_mover({10: (0.0, 2.0), 20: (2.0, 2.5), 30: (3.0, 2.0)})
doc.time_together = True
for o in layers.all():
    o.selected = o.id in (pa.id, pb.id)
layers.active = pa
ops.anim.keys_deselect_all()
a0, b0 = poses(pa), poses(pb)
doc.frame_current = 25
assert ops.anim.motion_path_stroke(brush="GRAB", points="3,0.25;3,0.8", radius="0.5", strength="1",
                                   layer=str(pa.id)) == {'FINISHED'}
assert max(math.dist(p, q) for p, q in zip(a0, poses(pa))) > 0.01, "A grabbed"
moved = max(math.dist(p, q) for p, q in zip(b0, poses(pb)))
assert moved < 1e-4, ("a Grab on A's path retimed B", moved, [k["interp"] for k in pb.keyframes("position")])
assert ops.ed.undo() == {'FINISHED'}
doc.frame_current = 25
assert ops.anim.motion_path_stretch_reset(frame="25", what="SHAPE", layer=str(pa.id)) == {'FINISHED'}
assert max(math.dist(p, q) for p, q in zip(b0, poses(pb))) < 1e-4, "Reset Shape on A retimed B"
assert ops.ed.undo() == {'FINISHED'}
doc.frame_current = 25
assert ops.anim.motion_path_stroke(brush="BLEND_SPEED", points="3,0.25", radius="1", strength="1",
                                   layer=str(pa.id)) == {'FINISHED'}
assert [k["interp"] for k in pb.keyframes("position") if k["frame"] == 30] == ["CUSTOM"], (
    "a speed brush's timing goes to the linked layer", pb.keyframes("position"))
doc.time_together = True

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
