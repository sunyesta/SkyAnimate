"""A, Alt A and Ctrl I on a bone layer select, deselect and invert the curves editable with it along with its bones: those marked
Editable in Bone Group whose collections show, and with Edit Descendants on, every drawing inside it. Others stay as
they are.

Run with:  OpenAnimate --background --python tests/test_bone_layer_select_all.py
"""
import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops.armature
doc.frame_current = 0
oanim.ops.layer.new_bone()
rig = layers.active
ops.bone_add(name="jaw", head_x="0", head_y="0", tail_x="0", tail_y="1", parent="")


def drawing(name, editable):
    oanim.ops.layer.new()
    layer = layers.active
    layer.name = name
    layer.add_curve([(0.0, 2.0), (0.5, 2.5), (1.0, 2.0)])
    oanim.ops.layer.move_to_group(group=str(rig.id))
    layer.edit_with_bones = editable
    return layer


def picked(layer):
    return [p[2] for p in layer.points]


def press(**mods):
    assert oanim.wm.event_simulate("A", area="Viewport", **mods)


lips = drawing("lips", True)
hidden = drawing("hidden", True)
other = drawing("other", False)
layers.active = rig
for layer, collection in ((lips, "Face"), (hidden, "Mouth")):
    assert ops.collection_add(name=collection) == {'FINISHED'}
    layer.select_points([0])
    assert ops.collection_assign(collection=collection) == {'FINISHED'}
    assert layer.collections == [collection]
    press(alt=True)
assert not any(picked(lips) + picked(hidden) + picked(other))

press()
assert rig.bones[0]["selected"], "the bones as before"
assert all(picked(lips)) and all(picked(hidden)), "A: the editable curves too"
assert not any(picked(other)), "not a drawing that isn't editable with the bones"

press(alt=True)
assert not rig.bones[0]["selected"]
assert not any(picked(lips)) and not any(picked(hidden)), "Alt A: deselected too"

# Ctrl I inverts them with the bones.
lips.select_points([1])
assert oanim.wm.event_simulate("I", ctrl=True, area="Viewport")
assert rig.bones[0]["selected"]
assert picked(lips) == [True, False, True] and all(picked(hidden)), "Ctrl I: inverted"
assert not any(picked(other)), "not a drawing that isn't editable with the bones"
press(alt=True)
assert not any(picked(lips)) and not any(picked(hidden))

# A selected handle goes with Alt A, as mesh.select_none takes it.
lips.select_points([1])
press(alt=True)
assert not any(picked(lips))

# Only what shows: a drawing in a hidden collection stays out.
assert ops.collection_set(collection="All", visible="0") == {'FINISHED'}
assert ops.collection_set(collection="Mouth", visible="0") == {'FINISHED'}
assert lips.edited_with_bones and not hidden.edited_with_bones
press()
assert all(picked(lips)) and not any(picked(hidden)), "A hidden collection's drawing stays unselected"
press(alt=True)
assert ops.collection_set(collection="All", visible="1") == {'FINISHED'}

# With Edit Descendants, every drawing inside the bone layer.
rig.edit_descendants = True
press()
assert all(picked(other)), "Edit Descendants brings it"
press(alt=True)
assert not any(picked(other))
rig.edit_descendants = False

# One undo step: undo puts the selection back.
press()
assert all(picked(lips))
oanim.ops.ed.undo()
assert not any(picked(lips)) and not rig.bones[0]["selected"]

print("Bone layer select all tests passed")
