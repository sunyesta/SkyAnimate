"""The timeline and the pick: picking points in a drawing or bones in a bone layer turns the layer's name red and
opens its Bones folder and the picked bones (or its Points folder). A layer's own row is never opened by a pick: it
is folded exactly while the layer keys together (Key Together), so opening it would turn that off. Folded over a
pick, it is red instead. Folded while the pick lasts, a folder stays folded; once the pick is gone, every folder goes
back to the fold the project keeps, which a pick never changes.

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
brow.key_together = False  # open: its own rows show
brow.name = "brow_L"
brow.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0)])
ops.layer.group()
head = layers.active
head.key_together = False  # open: its own rows show
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

# Nothing picked; head and rig keying together (folded), the folders folded as the project keeps them.
layers.active = brow
ops.mesh.select_none()
rig.select_bone("")
layers.active = rig
for name in ("Bones", "arm"):
    wm.set_ui_open(fold_key(name), False)
head.key_together = True
rig.key_together = True
assert not row("head")["open"] and row("brow_L") is None and not row("rig")["open"], rows()

# Points picked in brow_L: head stays folded (still keying together), red for the pick inside it.
layers.active = brow
brow.select_points([0])
assert not row("head")["open"] and head.key_together, ("a pick never opens a layer's row", row("head"))
assert row("head")["red"], "folded over the pick: red"
assert not row("head")["holds_pick"]
assert not row("rig")["open"] and not row("rig")["red"], "nothing picked in the rig"

# Opened (Key Together off): brow_L shows, red, its Drawing red under it; head isn't red now.
head.key_together = False
assert row("head")["open"] and not row("head")["red"], row("head")
assert row("brow_L")["open"] and row("brow_L")["red"], row("brow_L")
assert row("Drawing") and row("Drawing")["red"], rows()

# brow_L folded while picked: keying together, still red.
assert wm.timeline_row_fold(index("brow_L"), False) and brow.key_together
assert not row("brow_L")["open"] and row("Drawing") is None and row("brow_L")["red"]
# The pick gone: not red, still folded (the fold is Key Together, which a pick doesn't change).
ops.mesh.select_none()
assert not row("brow_L")["red"] and not row("brow_L")["open"], row("brow_L")
brow.key_together = False

# A bone picked in the folded rig: it stays folded and turns red.
layers.active = rig
rig.select_bone("arm")
assert not row("rig")["open"] and row("rig")["red"], row("rig")
# Opened: its Bones and the picked bone open for the pick; the other bone stays folded.
rig.key_together = False
assert row("rig")["open"] and row("rig")["red"], row("rig")
assert row("Bones")["open"] and row("arm")["open"] and row("arm")["red"], rows()
assert row("Angle"), "the picked bone's channels show"
assert not row("leg")["open"] and not row("leg")["red"]

# Bones folded while picked: it stays folded, and the project's fold isn't touched.
assert wm.timeline_row_fold(index("Bones"), False)
assert not row("Bones")["open"] and row("arm") is None
assert wm.ui_open().get(fold_key("Bones"), False) is False, "the project's fold is as it was"

# Deselected: the folders as the project keeps them again; picked again, Bones opens again.
rig.select_bone("")
assert row("rig")["open"] and not row("rig")["red"] and not row("Bones")["open"], rows()
rig.select_bone("arm")
assert row("Bones")["open"] and row("arm")["red"], rows()

print("timeline pick open: ok")
