"""A switch layer's timeline row: open, it shows only its own switch keys (which child shows there), as an open group's
row shows none of what it holds, those keys being in its children's rows; no separate Switch row under it. Folded, it
shows every key inside it.

Run with:  OpenAnimate --background --python tests/test_timeline_switch_row.py
"""

import oanim

ops, layers, wm = oanim.ops, oanim.data.layers, oanim.wm

assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
doc = oanim.data.document
doc.auto_key = True
doc.auto_freeze_keys = False
made = {}
for name in ("Closed", "Open"):
    ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(-0.5, 0.0), (0.5, 0.0)])
    made[name] = l
for l in layers.all():
    l.selected = l.id in (made["Closed"].id, made["Open"].id)
assert ops.layer.wrap_in_switch() == {'FINISHED'}
sw = layers.active
sw.name = "Mouths"
assert sw.type == "SWITCH"
# The switch: Open on 3, Closed on 9. Closed's own keys: 6 and 12.
for frame, child in ((3, "Open"), (9, "Closed")):
    doc.frame_current = frame
    assert ops.layer.switch_set(layer=sw.name, child=child) == {'FINISHED'}
for frame, x in ((6, 0.5), (12, 1.0)):
    doc.frame_current = frame
    made["Closed"].location_x = x
doc.frame_current = 1
doc.frame_current = 0
layers.active = sw
for l in layers.all():
    l.selected = l.id == sw.id


def rows():
    return wm.timeline_row_details(True)


def row(label):
    return next((i, r) for i, r in enumerate(rows()) if r["label"] == label)


assert [k["frame"] for k in sw.keyframes("switch")] == [0, 3, 9], sw.keyframes("switch")

# --- Open: only its own switch keys; Closed's keys in Closed's rows ------------------------------------------------------
i, r = row("Mouths")
if not r["open"]:
    assert wm.timeline_row_fold(i, True)
    i, r = row("Mouths")
assert r["open"]
assert r["frames"] == [0, 3, 9], r["frames"]
labels = [x["label"] for x in rows()]
assert "Switch" not in labels, ("the switch's keys are on its own row", labels)
assert [x["frames"] for x in rows() if x["label"] == "Layer"] == [[0, 6, 12]], rows()

# --- Folded: everything inside it ----------------------------------------------------------------------------------------
assert wm.timeline_row_fold(i, False)
i, r = row("Mouths")
assert not r["open"]
assert r["frames"] == [0, 3, 6, 9, 12], r["frames"]

print("timeline switch row: ok")
