"""Key Together, its keys taken from every channel and files opened: a key deleted on one channel of a folded layer goes
from them all, and the frame shown is worked out again at once (not when the playhead next moves); in an action entered
again the same; right after opening a file, a layer moved out of a folded group lets go of its fillers as it does
mid-session.

Run with:  OpenAnimate --background --python tests/test_key_together_deletes.py
"""

import collections
import os
import tempfile

import oanim

ops, L = oanim.ops, oanim.data.layers


def fresh():
    assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
    d = oanim.data.document
    d.auto_key = True
    d.auto_freeze_keys = False
    return d


def settle():
    d = oanim.data.document
    d.frame_current = d.frame_current + 1
    d.frame_current = d.frame_current - 1


def frames(layer, path):
    return [k["frame"] for k in layer.keyframes(path)]


def named(name):
    return next(l for l in L.all() if l.name == name)


def drawing(name, points):
    ops.layer.new()
    l = L.active
    l.name = name
    l.add_curve(points)
    l.key_together = True
    return l


# --- A key deleted on one channel: the frame shown at once is without the others' keys there --------------------------
doc = fresh()
a = drawing("A", [(-0.5, 0.0), (0.0, 0.3), (0.5, 0.0)])
doc.frame_current = 10
a.location_x = 1.0
settle()
doc.frame_current = 30
a.opacity = 0.5
settle()
assert frames(a, "position") == [0, 10, 30], frames(a, "position")
ops.anim.keys_deselect_all()
assert ops.anim.keys_select(frame_start="30", layers=str(a.id), path="position") == {'FINISHED'}
assert ops.anim.keys_delete() == {'FINISHED'}
assert frames(a, "opacity") == [0, 10], frames(a, "opacity")
assert abs(a.opacity - 1.0) < 1e-4, ("the frame shown is worked out again at once", a.opacity)
assert ops.ed.undo() == {'FINISHED'}
assert ops.ed.redo() == {'FINISHED'}
assert abs(a.opacity - 1.0) < 1e-4, ("the step holds the frame as it is", a.opacity)

# --- In an action entered again: a key deleted on one channel goes from them all ------------------------------------
doc = fresh()
ops.layer.new_bone()
rig = L.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
e = drawing("E", [(-0.5, 2.0), (0.0, 2.3), (0.5, 2.0)])
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
L.active = rig
assert ops.anim.action_new(name="wave") == {'FINISHED'}
doc.frame_current = 6
e.location_x = 0.5
settle()
doc.frame_current = 9
e.set_point(1, (0.2, 2.6))
settle()
doc.frame_current = 0
settle()
assert ops.anim.action_set(name="") == {'FINISHED'}
settle()
doc.frame_current = 14
e.location_y = 1.0
settle()
doc.frame_current = 0
settle()
assert ops.anim.action_set(name="wave") == {'FINISHED'}
settle()


def by_frames(l):
    paths = set()
    for f in l.key_frames:
        paths |= set(l.keyed_at(f))
    return dict(collections.Counter(tuple(frames(l, p)) for p in paths))


assert list(by_frames(e)) == [(0, 6, 9)], by_frames(e)
L.active = e
e.selected = True
ops.anim.keys_deselect_all()
assert ops.anim.keys_select(frame_start="9", layers=str(e.id), path="rotation") == {'FINISHED'}
assert ops.anim.keys_delete() == {'FINISHED'}
settle()
assert list(by_frames(e)) == [(0, 6)], ("frame 9 goes from every channel", by_frames(e))

# --- Right after opening: a layer moved out of a folded group lets go of the group's fillers -------------------------
doc = fresh()
for name, y, f in (("C", -1.0, 8), ("D", -2.0, 16)):
    l = drawing(name, [(-0.5, y), (0.0, y - 0.3), (0.5, y)])
    l.key_together = False
    doc.frame_current = f
    l.location_x = 0.4
    settle()
doc.frame_current = 0
settle()
for l in L.all():
    l.selected = l.name in ("C", "D")
assert ops.layer.group() == {'FINISHED'}
g = L.active
assert ops.anim.timeline_fold_together(layer=str(g.id), folded="1") == {'FINISHED'}
settle()
assert frames(named("C"), "position") == [0, 8, 16], frames(named("C"), "position")
path = os.path.join(tempfile.mkdtemp(), "moved_out.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
c = named("C")
for l in L.all():
    l.selected = l.id == c.id
L.active = c
assert ops.layer.move_to_group(group="0") == {'FINISHED'}
settle()
assert frames(named("C"), "position") == [0, 8], ("its own keys, the group's filler on 16 let go",
                                                   frames(named("C"), "position"))

print("key together deletes: ok")
