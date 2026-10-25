"""Ctrl L > Copy Masks (layer.masks_copy): the active layer's mask cards and Clip to Below replace the other
picked layers'; a copied step naming the layer it lands on leaves it out, and a step left naming none
goes. One undo step; it needs another layer picked.

Run with:  OpenAnimate --background --python tests/test_masks_copy.py
"""

import oanim

layers = oanim.data.layers
ops = oanim.ops


def new(name):
    ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(0, 0), (1, 0), (1, 1)], closed=True, fill=True)
    return l


hand = new("hand")
arm = new("arm")
leg = new("leg")
face = new("face")
assert ops.mask.add(sources="hand|arm", mode="SUBTRACT") == {'FINISHED'}
assert ops.mask.add(sources="arm", mode="ADD") == {'FINISHED'}
face.clip_to_below = True
assert len(face.masks) == 2

# Nothing else picked: nothing to copy to.
assert ops.layer.masks_copy() == {'CANCELLED'}

# Arm and leg picked, face active.
arm.selected = True
leg.selected = True
layers.active = face
assert leg.masks == []
face_before = face.masks
assert ops.layer.masks_copy() == {'FINISHED'}
assert oanim.wm.undo_history()[0][-1] == "Copy Masks"
assert len(leg.masks) == 2 and leg.clip_to_below
assert leg.masks[0]["steps"][0]["layers"] == ["hand", "arm"], leg.masks[0]
# Arm doesn't mask itself: its copy of the first card keeps hand only, and the second card (arm only) has
# no step left.
assert arm.masks[0]["steps"][0]["layers"] == ["hand"], arm.masks[0]
assert arm.masks[1]["steps"] == [], arm.masks[1]
assert face.masks == face_before, "the source is unchanged"

ops.ed.undo()
assert leg.masks == [] and not leg.clip_to_below

print("masks_copy: ok")
