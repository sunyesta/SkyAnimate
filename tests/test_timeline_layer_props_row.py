"""A layer's own properties in the timeline sit in a Layer row of their own (folded by default, its keys those of every
property under it), before its drawing, bones and the layers inside it, so they don't mix with sublayers. A switch's
own keys stay on its own row, not in its Layer row. A layer with no property keyed has no Layer row.

Run with:  OpenAnimate --background --python tests/test_timeline_layer_props_row.py
"""

import oanim

ops, layers, wm = oanim.ops, oanim.data.layers, oanim.wm

assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
doc = oanim.data.document
doc.auto_key = True
doc.auto_freeze_keys = False


def vec(name):
    ops.layer.new()
    layer = layers.active
    layer.key_together = False  # open: its own rows show
    layer.name = name
    layer.add_curve([(-0.5, 0.0), (0.5, 0.0)])
    return layer


inner = vec("Inner")
plain = vec("Plain")
for l in layers.all():
    l.selected = l.id in (inner.id, plain.id)
ops.layer.group()
group = layers.active
group.name = "Group"
# The group moves on 4 and turns on 8; Inner moves on 6; Plain has no keys.
doc.frame_current = 4
group.location_x = 1.0
doc.frame_current = 8
group.rotation = 0.5
doc.frame_current = 6
inner.location_x = 2.0
doc.frame_current = 0
layers.active = group
for l in layers.all():
    l.selected = l.id == group.id


def rows():
    return wm.timeline_row_details(True)


def labels():
    return [(r["depth"], r["label"]) for r in rows()]


def find(label, after=None):
    rs = rows()
    start = 0 if after is None else next(i for i, r in enumerate(rs) if r["label"] == after) + 1
    return next((i, r) for i, r in enumerate(rs) if i >= start and r["label"] == label)


i, g = find("Group")
if not g["open"]:
    assert wm.timeline_row_fold(i, True)
# --- The group's own properties: one Layer row, folded, first under it, holding all their keys ---------------------------
i, own = find("Layer", after="Group")
d = next(r["depth"] for r in rows() if r["label"] == "Group")
assert own["depth"] == d + 1 and not own["open"], own
assert own["frames"] == [0, 4, 8], own["frames"]
under = [lab for dep, lab in labels() if dep == d + 1]
assert under[0] == "Layer" and "Inner" in under and "Position" not in under and "Rotation" not in under, labels()
# Opened, its properties under it.
assert wm.timeline_row_fold(i, True)
i, own = find("Layer", after="Group")
assert own["open"]
props = [r for r in rows()[i + 1:] if r["depth"] == own["depth"] + 1]
names = [r["label"] for r in props[:2]]
assert names == ["Position", "Rotation"], labels()
assert [r["frames"] for r in props[:2]] == [[0, 4], [0, 8]], [r["frames"] for r in props[:2]]
# Its keys pick and move as a row's: an Alt click without the tool selects the stretch's end key there.
assert wm.timeline_row_alt_click(i, 5)
assert [k["frame"] for k in group.keyframes("rotation") if k["selected"]] == [8]

# --- Each layer with something of its own keyed has one (Group, Inner); Plain, with nothing, has none ------------------
assert [r["label"] for r in rows()].count("Layer") == 2, labels()
assert "Plain" not in [r["label"] for r in rows()], labels()

# --- A switch: its own keys on its row, not in its Layer row --------------------------------------------------------------
assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
doc = oanim.data.document
doc.auto_key = True
doc.auto_freeze_keys = False
a = vec("A")
b = vec("B")
for l in layers.all():
    l.selected = l.id in (a.id, b.id)
assert ops.layer.wrap_in_switch() == {'FINISHED'}
sw = layers.active
sw.name = "Mouths"
doc.frame_current = 3
assert ops.layer.switch_set(layer=sw.name, child="B") == {'FINISHED'}
doc.frame_current = 5
sw.location_x = 1.0
doc.frame_current = 0
layers.active = sw
for l in layers.all():
    l.selected = l.id == sw.id
i, s = find("Mouths")
if not s["open"]:
    assert wm.timeline_row_fold(i, True)
i, s = find("Mouths")
assert s["frames"] == [0, 3], s["frames"]
i, own = find("Layer", after="Mouths")
assert own["frames"] == [0, 5], ("only its properties, not the switch's keys", own["frames"])

print("timeline layer props row: ok")
