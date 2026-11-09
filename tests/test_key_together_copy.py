"""Key Together on a copy: Duplicate copies a folded layer or group with the list of layers it was keying together, but
under a new id. Those layers never left it, so the copy must not let go of their fillers (which took the source's
fillers away, and reported "layers moved out").

Run with:  OpenAnimate --background --python tests/test_key_together_copy.py
"""

import oanim

ops, layers = oanim.ops, oanim.data.layers
assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
doc = oanim.data.document
doc.auto_key = True
doc.auto_freeze_keys = False


def frames(layer, path):
    try:
        return [k["frame"] for k in layer.keyframes(path)]
    except KeyError:
        return []


def fillers(layer, path):
    try:
        return [k["frame"] for k in layer.keyframes(path) if k["filler"]]
    except KeyError:
        return []


def settle():
    doc.frame_current = doc.frame_current + 1
    doc.frame_current = doc.frame_current - 1


def vec(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    layer.add_curve([(-0.5, 0.0), (0.5, 0.0)])
    return layer


# --- A folded vector layer: its points' fillers stay when it is duplicated -----------------------------------------
lone = vec("Lone")
lone.key_together = True
settle()
doc.frame_current = 10
lone.location_x = 1.0
settle()
assert 10 in fillers(lone, "points[0].pos"), lone.keyframes("points[0].pos")
for l in layers.all():
    l.selected = l.id == lone.id
layers.active = lone
keyed = sorted(lone.keyed_at(10))
assert ops.layer.duplicate() == {'FINISHED'}
settle()
assert sorted(lone.keyed_at(10)) == keyed, ("every channel keeps its key", keyed, lone.keyed_at(10))
assert 10 in fillers(lone, "points[0].pos"), ("the source keeps its fillers", lone.keyframes("points[0].pos"))
assert "moved out" not in str(oanim.wm.banner_report()), oanim.wm.banner_report()

# --- A folded group: its layers' fillers stay when the group is duplicated -----------------------------------------
assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
doc = oanim.data.document
doc.auto_key = True
doc.auto_freeze_keys = False
a = vec("A")
b = vec("B")
for l in layers.all():
    l.selected = l.id in (a.id, b.id)
assert ops.layer.group() == {'FINISHED'}
group = layers.active
group.key_together = True
settle()
doc.frame_current = 10
a.location_x = 1.0
settle()
assert 10 in fillers(b, "position"), b.keyframes("position")
for l in layers.all():
    l.selected = l.id == group.id
layers.active = group
keyed = sorted(b.keyed_at(10))
assert ops.layer.duplicate() == {'FINISHED'}
settle()
assert sorted(b.keyed_at(10)) == keyed, ("every channel keeps its key", keyed, b.keyed_at(10))
assert "moved out" not in str(oanim.wm.banner_report()), oanim.wm.banner_report()
assert 10 in fillers(b, "position"), ("the source's layers keep their fillers", b.keyframes("position"))
assert frames(a, "position") == [0, 10]

print("key together copy: ok")
