"""Only Selected: a picked layer shows its name row (even with no keys) and its own keys; its bones' keys only for
the picked bones, and the layers inside it only when picked themselves (Blender's Only Show Selected). Off (or
locked), a picked bone layer brings every bone and the animated layers inside it.

Run with:  OpenAnimate --background --python tests/test_timeline_picked_layer.py
"""

import oanim

oanim.data.document.auto_freeze_keys = False  # keyed channel by channel here, not holding the poses either side

doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True


def rows():
    return wm.timeline_row_details()


def names():
    return [r["name"] for r in rows()]


def row(name):
    return next((r for r in rows() if r["name"] == name), None)


def folded(name):
    """Layer `name`'s row folded (Key Together on, its keys on it; open, a heading without keys), then open again."""
    layer = next(x for x in layers.all() if x.name == name)
    layer.key_together = True
    r = row(name)
    layer.key_together = False
    return r


# A bone layer "Character" with two keyed bones, and a keyed drawing "Brow_L" inside it.
ops.layer.new_bone()
rig = layers.active
rig.name = "Character"
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0")
ops.armature.bone_add(name="leg", head_x="0", head_y="0", tail_x="0", tail_y="-1")
ops.layer.new()
brow = layers.active
brow.name = "Brow_L"
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
brow = next(x for x in layers.all() if x.id == brow.id)
doc.frame_current = 10
brow.location_x = 0.5
layers.active = rig
rig.bone_set("arm", angle=0.5)
rig.bone_set("leg", angle=0.25)
doc.frame_current = 0  # auto-key records the bones' and the drawing's changes
rig.select_bone("")
assert doc.timeline_only_selected

# Picked, no bone picked: just its name row, no keys (its bones' aren't its own), not the drawing inside.
assert names() == ["Character"], names()
assert row("Character")["frames"] == [], row("Character")

# A bone picked: that bone's rows and keys, under the layer, the other bone's nowhere.
rig.select_bone("arm")
assert "arm" in names() and "leg" not in names() and "Brow_L" not in names(), names()
assert folded("Character")["frames"] == [0, 10], rows()
# Its row (folded) picks only the keys it shows: double-clicked, the picked bone's, not the other's.
rig.key_together = True
at = next(i for i, r in enumerate(wm.timeline_row_details(True)) if r["name"] == "Character")
assert ops.anim.keys_deselect_all() == {'FINISHED'}
wm.timeline_key_press(at, 10, double_click=True)
rig.key_together = False


def selected(bone):
    return [k["frame"] for k in rig.keyframes('bones["%s"].angle' % bone) if k["selected"]]


assert 10 in selected("arm") and selected("leg") == [], (selected("arm"), selected("leg"))
assert ops.anim.keys_deselect_all() == {'FINISHED'}
rig.select_bone("")

# The layer's own keys show without a bone picked.
doc.frame_current = 20
rig.opacity = 0.5
doc.frame_current = 0
assert "Opacity" in names() and "Bones" not in names(), names()
assert 20 in folded("Character")["frames"] and 10 not in folded("Character")["frames"], folded("Character")

# The drawing inside shows once picked itself.
brow.selected = True
assert "Brow_L" in names(), names()
brow.selected = False

# Only Selected off: every bone, and the animated layers inside.
doc.timeline_only_selected = False
assert "leg" in names() and "arm" in names() and "Brow_L" in names(), names()
assert 10 in folded("Character")["frames"]
doc.timeline_only_selected = True

# Locked: as it was when locked, every bone and what's inside.
assert ops.anim.timeline_lock(lock="1") == {'FINISHED'}
assert "leg" in names() and "Brow_L" in names(), names()
assert ops.anim.timeline_lock(lock="0") == {'FINISHED'}
assert "Brow_L" not in names()

print("timeline picked layer: ok")
