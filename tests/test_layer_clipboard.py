"""Copy and paste layers (Ctrl C and Ctrl V over the Layers editor), from one file into another:
the picked layers with everything in them, above the active layer; masks and Draw Order naming
layers left behind find the new file's layers of the same name; inside a bone layer, Parent Bone
and weights find its bones by name; shared materials come along; one undo step.

Run with:  OpenAnimate --background --python tests/test_layer_clipboard.py
"""

import pathlib
import tempfile

import _oanim
import oanim

layers = oanim.data.layers
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_clipboard_"))


def find(name):
    return next((l for l in layers.all() if l.name == name), None)


def new(name, points):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve(points, closed=True, fill=True)
    return l


def shown(layer):
    points, _, _ = layer.evaluated()
    return [tuple(round(c, 4) for c in layer.to_document(p)) for p in points]


# --- The first file -----------------------------------------------------------------------------------
oanim.data.document.frame_current = 0
head = new("Head_X", [(-1.0, 0.0), (1.0, 0.0), (0.0, 1.5)])
mouth = new("Mouth", [(-0.2, 0.3), (0.2, 0.3), (0.0, 0.4)])
ear = new("Ear_L", [(1.0, 0.5), (1.4, 0.6), (1.5, 1.2), (1.1, 1.0)])
ear.location_x = 0.25
layers.active = ear
assert oanim.ops.mask.add(sources="Head_X") == {'FINISHED'}
assert oanim.ops.mask.add(sources="Mouth", outside="1") == {'FINISHED'}
ear.select_points([0, 1, 2, 3])
assert oanim.ops.material.new(name="Skin") == {'FINISHED'}
oanim.ops.mesh.select_none()

# A group with a layer that masks another inside it.
oanim.ops.layer.new_group()
group = layers.active
group.name = "Hair"
strand = new("Strand", [(0.0, 1.5), (0.3, 2.0), (0.1, 2.2)])
oanim.ops.layer.move_to_group(group=str(group.id))
tie = new("Tie", [(0.0, 1.6), (0.2, 1.6), (0.1, 1.8)])
oanim.ops.layer.move_to_group(group=str(group.id))
layers.active = find("Strand")
assert oanim.ops.mask.add(sources="Tie") == {'FINISHED'}

# In a bone layer: bound to a bone.
layers.active = find("Head_X")
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
rig.name = "Rig"
assert oanim.ops.armature.bone_primitive_add() == {'FINISHED'}
_oanim.layer_bone_set(rig.id, rig.bones[0]["name"], {"name": "arm"})
hand = new("Hand", [(0.5, 0.0), (0.7, 0.1), (0.6, 0.3)])
oanim.ops.layer.move_to_group(group=str(rig.id))
layers.active = find("Hand")
assert oanim.ops.layer.parent_bone_set(bone="arm") == {'FINISHED'}

ear_points, hair_names = shown(find("Ear_L")), [l.name for l in find("Hair").children]

# Ctrl C over the Layers editor copies the active layer and the picked ones.
layers.active = find("Ear_L")
find("Hair").select = True
assert oanim.wm.event_simulate("C", ctrl=True, area="Layers")
assert not oanim.wm.event_simulate("V", ctrl=True, area="Properties"), "only over the Layers editor"
first_file = str(folder / "first.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=first_file) == {'FINISHED'}

# --- Another file ----------------------------------------------------------------------------------
assert oanim.ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
assert find("Ear_L") is None
other_head = new("Head_X", [(-2.0, 0.0), (2.0, 0.0), (0.0, 3.0)])
assert oanim.wm.event_simulate("V", ctrl=True, area="Layers")
ear = find("Ear_L")
assert ear is not None, [l.name for l in layers.all()]
assert shown(ear) == ear_points, "where it was, as it was"
assert [m["layers"] for m in ear.masks] == [["Head_X"]], ("this file's Head_X; Mouth isn't here", ear.masks)
assert ear.shape_material(0) == "Skin"
assert [l.name for l in find("Hair").children] == hair_names
assert [m["layers"] for m in find("Strand").masks] == [["Tie"]], "the copy's own Tie"
names = [l.name for l in layers.all()]
assert names.index("Hair") < names.index("Ear_L") < names.index("Head_X"), ("in their order, above the active layer", names)
assert "Rig" not in names and "Mouth" not in names, names
assert layers.active.name == "Ear_L" and find("Hair").select
assert oanim.wm.undo_history()[0][-1] == "Paste 2 Layers", oanim.wm.undo_history()[0][-3:]

# Pasting again: names stay unique, the material is the same one.
oanim.ops.layer.paste()
assert find("Ear_L 1") is not None and find("Hair 1") is not None
assert [m["name"] for m in oanim.data.materials].count("Skin") == 1
oanim.ops.ed.undo()
oanim.ops.ed.undo()
assert find("Ear_L") is None, "one undo step each"

# A bound layer pasted into a bone layer with a bone of the same name keeps its bone; elsewhere it
# lets go.
assert oanim.ops.wm.open_mainfile(filepath=first_file, confirmed="1") == {'FINISHED'}
find("Hair").select = False
layers.active = find("Hand")
assert oanim.ops.layer.copy() == {'FINISHED'}
assert oanim.ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
assert oanim.ops.armature.bone_primitive_add() == {'FINISHED'}
assert oanim.ops.armature.bone_primitive_add() == {'FINISHED'}
_oanim.layer_bone_set(rig.id, rig.bones[1]["name"], {"name": "arm"})
inner = new("Body", [(0.0, 0.0), (0.1, 0.0), (0.0, 0.1)])
oanim.ops.layer.move_to_group(group=str(rig.id))
layers.active = find("Body")
assert oanim.ops.layer.paste() == {'FINISHED'}
hand = find("Hand")
assert hand.parent_bone == "arm", hand.parent_bone
assert find("Rig") is None, "only the layer, not the bone layer it was in"
assert hand.id in [l.id for l in find(rig.name).children]
layers.active = find(rig.name)
assert oanim.ops.layer.paste() == {'FINISHED'}
loose = layers.active
assert loose.name == "Hand 1", loose.name
assert [l.name for l in find(rig.name).children] == ["Hand", "Body"]
assert loose.parent_bone == "", ("no bone layer here", loose.parent_bone)
print("layer_clipboard: ok")
