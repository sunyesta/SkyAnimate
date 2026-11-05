"""The timeline's Only Selected (on for new documents) and Lock: with Only Selected the rows are the picked
layers', the animated layers inside them and the groups they are in (dimmed), and of a bone layer with
bones picked only those bones; off, every animated layer. Lock keeps the layers shown when it was turned
on, whatever is picked after. Both are saved with the project.

Run with:  OpenAnimate --background --python tests/test_timeline_only_selected.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True


def rows():
    return wm.timeline_row_details()


def names():
    return [(r["depth"], r["name"]) for r in rows()]


def shown():
    return [n for d, n in names()]


def row(name):
    return next(r for r in rows() if r["name"] == name)


# A drawing on its own, and a group holding another.
alone = layers.active
alone.name = "alone"
ops.layer.new()
inner = layers.active
inner.name = "inner"
ops.layer.group()
group = layers.active
group.name = "group"
group.edit_descendants = False
doc.frame_current = 10
alone.location_x = 0.5
inner.location_x = 0.5
doc.frame_current = 0
assert doc.timeline_only_selected, "on for new documents"

# The drawing inside the group picked: the group above it, dimmed, for where it is.
layers.active = inner
tree = names()
assert (0, "group") in tree and (1, "inner") in tree, tree
assert row("group")["dim"] and not row("inner")["dim"]
assert "alone" not in [n for d, n in tree], "only what's picked"

# Off: every animated layer.
doc.timeline_only_selected = False
tree = names()
assert (0, "alone") in tree and (1, "inner") in tree, tree
doc.timeline_only_selected = True

# Bones: with some picked, only those; with none, all.
ops.layer.new_bone()
rig = layers.active
rig.name = "rig"
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0")
ops.armature.bone_add(name="leg", head_x="0", head_y="0", tail_x="0", tail_y="-1")
doc.frame_current = 5
rig.bone_set("arm", angle=0.5)
rig.bone_set("leg", angle=0.25)
assert ops.anim.keyframe_insert() == {'FINISHED'}
doc.frame_current = 0
rig.select_bone("arm")
bones = [n for d, n in names() if n in ("arm", "leg")]
assert bones == ["arm"], ("only the picked bone", bones)
doc.timeline_only_selected = False
bones = [n for d, n in names() if n in ("arm", "leg")]
assert bones == ["arm", "leg"], ("Only Selected off: every bone", bones)
doc.timeline_only_selected = True
assert oanim.ops.armature.select_none() == {'FINISHED'}
bones = [n for d, n in names() if n in ("arm", "leg")]
assert bones == ["arm", "leg"], ("none picked: all of them", bones)

# Lock: the layers shown now stay, whatever is picked after.
layers.active = alone
assert ops.anim.timeline_lock(lock="1") == {'FINISHED'} and doc.timeline_lock
layers.active = rig
assert "alone" in shown() and "rig" not in shown(), names()
# Saved with the project.
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_tl_lock_")) / "lock.oanim")
doc.timeline_only_selected = True
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
doc = oanim.data.document
assert doc.timeline_lock
rig = next(l for l in layers.all() if l.name == "rig")
layers.active = rig
assert "alone" in shown() and "rig" not in shown(), ("kept after opening", names())
# Unlocked: back to the picked layers.
assert ops.anim.timeline_lock(lock="0") == {'FINISHED'} and not doc.timeline_lock
assert "rig" in shown() and "alone" not in shown(), names()
# With no lock=, it turns over.
assert ops.anim.timeline_lock() == {'FINISHED'} and doc.timeline_lock
assert ops.anim.timeline_lock() == {'FINISHED'} and not doc.timeline_lock

# Only Selected off is saved too.
doc.timeline_only_selected = False
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
assert not oanim.data.document.timeline_only_selected

print("timeline only selected: ok")
