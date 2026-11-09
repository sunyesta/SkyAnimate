"""Only Selected shows what is picked, and nothing when nothing is: Deselect All Layers (Alt A in the Layers
editor) leaves the active layer active, for editing and keys, but unpicked, and the timeline then has no rows
(pinned ones aside) until a layer is picked again or points or bones are picked in the layer being edited.
Only Selected off, or Lock on, shows layers whatever is picked.

Run with:  OpenAnimate --background --python tests/test_timeline_nothing_selected.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True


def names():
    return [r[1] for r in wm.timeline_rows()]


# A drawing "art" with keyed points, and a bone layer "rig" with a keyed bone.
art = layers.active
art.name = "art"
art.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0)])
ops.layer.new_bone()
rig = layers.active
rig.name = "rig"
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0")
doc.frame_current = 10
rig.bone_set("arm", angle=0.5)
assert ops.anim.keyframe_insert() == {'FINISHED'}
layers.active = art
ops.mesh.select_all()
assert ops.mesh.translate(move_x="0.25", move_y="0") == {'FINISHED'}
ops.mesh.select_none()
rig.select_bone("")
doc.frame_current = 0
assert doc.timeline_only_selected
# The camera keyed too (its row shows once it has keys, but not with nothing picked).
cam = oanim.data.camera
assert ops.anim.keyframe_toggle(layer=str(cam.id), paths="rotation") == {'FINISHED'}
assert "Camera" in names(), names()

# The active layer is picked: its rows show.
assert "art" in names() and "rig" not in names(), names()

# Deselect All Layers: nothing picked, no rows; the active layer stays active.
assert ops.layer.select_none() == {'FINISHED'}
assert names() == [], names()
assert layers.active.id == art.id, "still active, for editing"
assert ops.layer.select_none() == {'CANCELLED'}, "nothing left to deselect"
ops.ed.undo()
assert "art" in names(), "an undo step"
ops.ed.redo()
assert names() == []

# Points picked in the layer being edited: it shows, red (keying together, chained shut over the pick).
art.select_points([0])
assert "art" in names(), names()
assert art.key_together and any(r[1] == "art" and r[2] for r in wm.timeline_rows()), wm.timeline_rows()
ops.mesh.select_none()
assert names() == [], names()

# Made active again (the same layer): picked again.
layers.active = art
assert "art" in names(), names()
ops.layer.select_none()
assert names() == []

# A pinned channel shows whatever is picked.
assert ops.anim.channel_pin(layer=str(rig.id), paths='bones["arm"].angle', pinned="1") == {'FINISHED'}
assert "Pinned" in names() and "art" not in names() and "rig" not in names(), names()
assert ops.anim.channel_pin(layer=str(rig.id), paths='bones["arm"].angle', pinned="0") == {'FINISHED'}
assert names() == [], names()

# A layer picked again (made active) shows; a bone picked in it after deselecting shows it too.
layers.active = rig
assert "rig" in names(), names()
ops.layer.select_none()
assert names() == [], names()
rig.select_bone("arm")
assert "rig" in names() and "arm" in names(), names()
rig.select_bone("")
assert names() == [], names()

# A layer picked with Ctrl in the Layers editor (selected) shows.
art.selected = True
assert "art" in names() and "rig" not in names(), names()
art.selected = False

# Only Selected off: every animated layer, nothing picked or not.
doc.timeline_only_selected = False
assert "art" in names() and "rig" in names(), names()
doc.timeline_only_selected = True
assert names() == []

# Lock: the locked layers stay, whatever is picked.
layers.active = art
assert ops.anim.timeline_lock(lock="1") == {'FINISHED'}
ops.layer.select_none()
assert "art" in names(), names()

print("timeline nothing selected: ok")
