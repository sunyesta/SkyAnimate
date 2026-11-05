"""The timeline opens what holds the pick: picking points in a drawing or bones in a bone layer opens that
layer's row and the groups it is in (and its Bones folder and the picked bones, or its Points folder), and
turns the layer's name red. Folded while the pick lasts, a row stays folded; once the pick is gone, every
row goes back to the fold the project keeps, which a pick never changes.

Run with:  OpenAnimate --background --python tests/test_timeline_pick_open.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True


def rows(as_shown=True):
    return wm.timeline_row_details(as_shown)


def row(name, as_shown=True):
    return next((r for r in rows(as_shown) if r["name"] == name), None)


def index(name):
    return next(i for i, r in enumerate(rows()) if r["name"] == name)


def fold_key(name):
    return "Timeline/" + row(name, False)["fold"]


# A group "head" holding the drawing "brow_L" (keyed as a whole), and a bone layer "rig" with two bones.
brow = layers.active
brow.name = "brow_L"
brow.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0)])
ops.layer.group()
head = layers.active
head.name = "head"
ops.layer.new_bone()
rig = layers.active
rig.name = "rig"
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0")
ops.armature.bone_add(name="leg", head_x="0", head_y="0", tail_x="0", tail_y="-1")

doc.frame_current = 10
layers.active = brow
assert brow.key_whole_drawing
ops.mesh.select_all()
assert ops.mesh.translate(move_x="0.25", move_y="0") == {'FINISHED'}
rig.bone_set("arm", angle=0.5)
rig.bone_set("leg", angle=0.25)
layers.active = rig
assert ops.anim.keyframe_insert() == {'FINISHED'}
doc.frame_current = 0
doc.timeline_only_selected = False  # every animated layer, picked or not

# Nothing picked, everything folded as the project keeps it.
layers.active = brow
ops.mesh.select_none()
rig.select_bone("")
layers.active = rig
for name in ("head", "brow_L", "rig", "Bones", "arm"):
    wm.set_ui_open(fold_key(name), False)
assert not row("head")["open"] and row("brow_L") is None and not row("rig")["open"], rows()

# Points picked in brow_L: head and brow_L open, brow_L red, its Drawing red under it.
layers.active = brow
brow.select_points([0])
assert row("head")["open"] and row("head")["holds_pick"], row("head")
assert row("brow_L")["open"] and row("brow_L")["red"], row("brow_L")
assert not row("head")["red"], "only the layer holding the pick is red"
assert row("Drawing") and row("Drawing")["red"], rows()
assert not row("rig")["open"] and not row("rig")["red"], "nothing picked in the rig"

# Folded while picked: it stays folded, and the project's fold isn't touched.
assert wm.timeline_row_fold(index("brow_L"), False)
assert not row("brow_L")["open"] and row("Drawing") is None
assert wm.ui_open().get(fold_key("brow_L"), False) is False, "the project's fold is as it was"

# The pick gone: back to the project's folds, not red.
ops.mesh.select_none()
assert not row("head")["open"] and row("brow_L") is None, rows()
wm.set_ui_open(fold_key("head"), True)
assert not row("brow_L")["red"] and not row("brow_L")["open"], row("brow_L")
wm.set_ui_open(fold_key("head"), False)

# Picked again, it opens again (being folded lasted only for that pick).
brow.select_points([1])
assert row("brow_L")["open"] and row("Drawing")["red"], rows()
ops.mesh.select_none()

# A bone picked: rig, its Bones and the bone open, rig red; the other bone stays folded.
layers.active = rig
rig.select_bone("arm")
assert row("rig")["open"] and row("rig")["red"], row("rig")
assert row("Bones")["open"] and row("arm")["open"] and row("arm")["red"], rows()
assert row("Angle"), "the picked bone's channels show"
assert not row("leg")["open"] and not row("leg")["red"]

# Deselected: folded as the project keeps it again.
rig.select_bone("")
assert not row("rig")["open"] and not row("rig")["red"] and row("Bones") is None, rows()

print("timeline pick open: ok")
