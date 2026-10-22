"""Applying and duplicating modifiers, as Blender's header menu and Ctrl A / Shift D over a modifier do:
Apply makes it part of the drawing (on Setup, frame 0), Duplicate copies it below itself
("Mirror.001"). The Armature modifier comes with the bone layer: it can't be applied or copied.

Run with:  OpenAnimate --background --python tests/test_modifier_apply.py
"""

import oanim

layers = oanim.data.layers
doc = oanim.data.document
doc.frame_current = 0

art = layers.active
art.add_curve([(0.2, 0.0), (0.6, 0.3), (0.9, -0.2)])
assert oanim.ops.object.modifier_add(type="MIRROR") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["MIRROR"]

# Duplicate: below itself, the next free number.
assert oanim.ops.object.modifier_duplicate(index="0") == {'FINISHED'}
assert [m["name"] for m in art.modifiers] == ["Mirror", "Mirror.001"], art.modifiers
assert oanim.ops.object.modifier_duplicate(index="1") == {'FINISHED'}
assert [m["name"] for m in art.modifiers] == ["Mirror", "Mirror.001", "Mirror.002"], art.modifiers
oanim.ops.object.modifier_remove(index="2")
oanim.ops.object.modifier_remove(index="1")

# Apply: only on Setup.
doc.frame_current = 5
assert oanim.ops.object.modifier_apply(index="0") == {'CANCELLED'}
doc.frame_current = 0
shown = len(art.evaluated()[0])
assert art.point_count == 3 and shown > 3
assert oanim.ops.object.modifier_apply(index="0") == {'FINISHED'}
assert art.modifiers == [] and art.point_count == shown, "the reflection is points of the drawing's own now"
assert len(art.evaluated()[0]) == shown, "and it looks as it did"
assert oanim.wm.undo_history()[0][-1] == "Apply Mirror"
oanim.ops.ed.undo()
assert [m["type"] for m in art.modifiers] == ["MIRROR"] and art.point_count == 3

# The Armature modifier can't be applied or duplicated.
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
oanim.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
layers.active = art
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
art = next(l for l in layers.all() if l.id == art.id)
layers.active = art
k = next(i for i, m in enumerate(art.modifiers) if m["type"] == "ARMATURE")
assert oanim.ops.object.modifier_apply(index=str(k)) == {'CANCELLED'}
assert oanim.ops.object.modifier_duplicate(index=str(k)) == {'CANCELLED'}
assert sum(m["type"] == "ARMATURE" for m in art.modifiers) == 1

print("modifier_apply: ok")
