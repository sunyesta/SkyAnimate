"""Key Together, opened after timing edits: a timing edit on a folded row's stretch only moves the timing dots. Opening
the row lets go of the fillers that were retimed but shape nothing (a layer that never moves keeps no keys), and keeps
those that now shape a motion, so nothing on screen changes either way; after saving and opening too.

Run with:  OpenAnimate --background --python tests/test_key_together_retimed.py
"""

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
    try:
        return [k["frame"] for k in layer.keyframes(path)]
    except KeyError:
        return []


def named(name):
    return next(l for l in L.all() if l.name == name)


def drawing(name, y=0.0):
    ops.layer.new()
    l = L.active
    l.name = name
    l.add_curve([(0.0, y), (1.0, y)])
    l.key_together = False
    return l


def all_keys(layer):
    paths = ["position", "rotation", "scale", "opacity"] + ["points[%d].pos" % i for i in range(len(layer.points))]
    return {p: frames(layer, p) for p in paths if frames(layer, p)}


def plays(names, last=30):
    d = oanim.data.document
    out = []
    for f in range(last + 1):
        d.frame_current = f
        for n in names:
            l = named(n)
            out.append((l.location_x, l.location_y, l.opacity) + tuple(c for p in l.points for c in p[:2]))
    d.frame_current = 0
    return out


def same(a, b):
    return max(abs(u - v) for x, y in zip(a, b) for u, v in zip(x, y)) < 1e-4


def ease(layer, frame):
    d = oanim.data.document
    L.active = layer
    for l in L.all():
        l.selected = l.id == layer.id
    d.frame_current = frame
    assert ops.wm.tool_motion_path() == {'FINISHED'}
    assert ops.anim.motion_path_select(frame=str(frame), layer=str(layer.id)) == {'FINISHED'}
    assert ops.anim.motion_path_stretch_shape(frame=str(frame), layer=str(layer.id), shape="EASE_IN") == {'FINISHED'}
    d.frame_current = 0
    settle()


def folded_group(*names):
    for l in L.all():
        l.selected = l.name in names
    assert ops.layer.group() == {'FINISHED'}
    g = L.active
    assert ops.anim.timeline_fold_together(layer=str(g.id), folded="1") == {'FINISHED'}
    settle()
    return g


def open_row(layer):
    assert ops.anim.timeline_fold_together(layer=str(layer.id), folded="0") == {'FINISHED'}
    settle()


# --- A folded group, one layer still: retimed while folded, opened, the still one keeps no keys --------------------
doc = fresh()
moves, still = drawing("Moves"), drawing("Still", 2.0)
for f, x in ((10, 2.0), (20, 4.0)):
    doc.frame_current = f
    moves.location_x = x
doc.frame_current = 0
settle()
g = folded_group("Moves", "Still")
assert frames(still, "position") == [0, 10, 20], frames(still, "position")
ease(moves, 5)
before = plays(["Moves", "Still"])
open_row(g)
assert all_keys(named("Still")) == {}, ("a layer that never moves keeps no keys", all_keys(named("Still")))
assert frames(named("Moves"), "position") == [0, 10, 20], frames(named("Moves"), "position")
assert same(before, plays(["Moves", "Still"])), "opened, everything plays as it did folded"

# --- A folded drawing alone: its opacity, never changed, keeps no keys; its position its own ------------------------
doc = fresh()
a = drawing("A")
a.key_together = True
settle()
for f, x in ((10, 2.0), (20, 4.0)):
    doc.frame_current = f
    a.location_x = x
doc.frame_current = 0
settle()
assert frames(a, "opacity") == [0, 10, 20], frames(a, "opacity")
ease(a, 5)
before = plays(["A"])
open_row(a)
assert frames(a, "opacity") == [] and frames(a, "rotation") == [], all_keys(a)
assert frames(a, "position") == [0, 10, 20], frames(a, "position")
assert same(before, plays(["A"])), "opened, it plays as it did folded"

# --- A retimed filler that shapes a motion stays: nothing on screen changes ---------------------------------------------
doc = fresh()
glide, beat = drawing("Glide"), drawing("Beat", 2.0)
doc.frame_current = 20
glide.location_x = 4.0
doc.frame_current = 10
beat.location_x = 1.0
doc.frame_current = 0
settle()
g = folded_group("Glide", "Beat")
assert frames(glide, "position") == [0, 10, 20], frames(glide, "position")
ease(beat, 5)
before = plays(["Glide", "Beat"])
open_row(g)
assert 10 in frames(named("Glide"), "position"), ("Glide's filler on 10 now shapes its ease: it stays",
                                                   frames(named("Glide"), "position"))
assert same(before, plays(["Glide", "Beat"])), "opened, everything plays as it did folded"

# --- Saved and opened while folded: the same ----------------------------------------------------------------------------
doc = fresh()
moves, still = drawing("Moves"), drawing("Still", 2.0)
for f, x in ((10, 2.0), (20, 4.0)):
    doc.frame_current = f
    moves.location_x = x
doc.frame_current = 0
settle()
g = folded_group("Moves", "Still")
ease(moves, 5)
path = os.path.join(tempfile.mkdtemp(), "retimed.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
open_row(next(l for l in L.all() if l.type == 'GROUP'))
assert all_keys(named("Still")) == {}, ("after opening the file too", all_keys(named("Still")))

print("key together retimed: ok")
