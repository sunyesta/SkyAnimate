"""Materials: shared looks for shapes. A shape has its own look or is linked to one material, which sets
its fill and line colours, line width and brush (Fill / Outline on and off stay the shape's). Changing a
linked shape's look changes the material and every shape using it; unlinking keeps the look as the
shape's own; deleting a material leaves its shapes their look; nothing is deleted unless asked.

Run with:  OpenAnimate --background --python tests/test_materials.py
"""

import os
import tempfile

import oanim

layers = oanim.data.layers
doc = oanim.data.document
doc.frame_current = 0


def close(a, b):
    return all(abs(x - y) < 1e-5 for x, y in zip(a, b))


def materials():
    return {m["name"]: m for m in oanim.data.materials}


def pick(*shapes):
    oanim.ops.mesh.select_none()
    for s in shapes:
        layer.select_points(list(range(s * 4, s * 4 + 4)))


layer = layers.active
layer.add_curve([(0, 0), (1, 0), (1, 1), (0, 1)], closed=True, fill=True)
layer.add_curve([(2, 0), (3, 0), (3, 1), (2, 1)], closed=True, fill=True)
layer.add_curve([(4, 0), (5, 0), (5, 1), (4, 1)], closed=True, fill=True)
assert oanim.data.materials == [] and layer.shape_material(0) == "", "a shape's look starts as its own"
layer.set_shape_style(0, fill=(0.2, 0.6, 0.3, 1.0), line_width=0.02)

# New: the shape's look becomes a shared material, named as asked.
pick(0)
assert oanim.ops.material.new(name="Skin") == {'FINISHED'}
assert layer.shape_material(0) == "Skin" and materials()["Skin"]["users"] == 1
assert close(materials()["Skin"]["fill"], (0.2, 0.6, 0.3, 1.0)) and abs(materials()["Skin"]["line_width"] - 0.02) < 1e-6
assert oanim.wm.undo_history()[0][-1] == "New Material Skin"

# Linking another shape gives it the material's look.
pick(1)
assert oanim.ops.material.assign(material="Skin") == {'FINISHED'}
assert layer.shape_material(1) == "Skin" and materials()["Skin"]["users"] == 2
assert close(layer.shape_style(1)["fill"], (0.2, 0.6, 0.3, 1.0))
assert oanim.ops.material.assign(material="Skin") == {'CANCELLED'}, "linked already"
assert oanim.ops.material.assign(material="Nope") == {'CANCELLED'}

# Changing a linked shape's look changes the material, and every shape using it.
layer.set_shape_style(0, fill=(0.9, 0.1, 0.1, 1.0), line=(0.0, 0.0, 1.0, 1.0))
assert close(layer.shape_style(1)["fill"], (0.9, 0.1, 0.1, 1.0)) and close(layer.shape_style(1)["line"], (0, 0, 1, 1))
assert close(materials()["Skin"]["fill"], (0.9, 0.1, 0.1, 1.0))
assert not close(layer.shape_style(2)["fill"], (0.9, 0.1, 0.1, 1.0)), "a shape with its own look doesn't change"
# Fill and Outline on/off stay each shape's.
layer.set_shape_style(0, has_fill=False)
assert layer.shape_style(1)["has_fill"] and not layer.shape_style(0)["has_fill"]
layer.set_shape_style(0, has_fill=True)

# Unlink (the user count): the shape keeps the look as its own, and no longer follows.
pick(1)
assert oanim.ops.material.unlink() == {'FINISHED'}
assert layer.shape_material(1) == "" and close(layer.shape_style(1)["fill"], (0.9, 0.1, 0.1, 1.0))
assert materials()["Skin"]["users"] == 1
layer.set_shape_style(0, fill=(0.5, 0.5, 0.5, 1.0))
assert close(layer.shape_style(1)["fill"], (0.9, 0.1, 0.1, 1.0)), "unlinked: Skin's change doesn't reach it"

# New on a shape that came from Skin: a variant named after it.
pick(1)
assert oanim.ops.material.new() == {'FINISHED'} and layer.shape_material(1) == "Skin.001"
assert oanim.ops.material.rename(material="Skin.001", name="Shadow") == {'FINISHED'}
assert layer.shape_material(1) == "Shadow"
assert oanim.ops.material.rename(material="Shadow", name="Skin") == {'FINISHED'}, "a taken name gets a number"
assert layer.shape_material(1) == "Skin.001"
oanim.ops.ed.undo()
assert layer.shape_material(1) == "Shadow"

# Delete: the shapes keep its look as their own.
look = layer.shape_style(0)["fill"]
assert oanim.ops.material.remove(material="Skin") == {'FINISHED'}
assert "Skin" not in materials() and layer.shape_material(0) == "" and close(layer.shape_style(0)["fill"], look)
oanim.ops.ed.undo()
assert layer.shape_material(0) == "Skin", "undo brings it back, linked"

# Unused materials stay until Remove Unused.
pick(2)
assert oanim.ops.material.new(name="Spare") == {'FINISHED'}
assert oanim.ops.material.unlink() == {'FINISHED'}
assert materials()["Spare"]["users"] == 0, "nothing deletes it on its own"
assert oanim.ops.material.remove_unused() == {'FINISHED'} and "Spare" not in materials()
assert oanim.ops.material.remove_unused() == {'CANCELLED'}, "every material is used"

# A duplicated layer's shapes share the materials.
assert oanim.ops.layer.duplicate() == {'FINISHED'}
copy = layers.active
assert copy.shape_material(0) == "Skin" and materials()["Skin"]["users"] == 2
copy.set_shape_style(0, fill=(0.1, 0.2, 0.9, 1.0))
assert close(layer.shape_style(0)["fill"], (0.1, 0.2, 0.9, 1.0)), "the original's shape follows the shared material"
layers.active = copy
oanim.ops.layer.delete()
layers.active = layer

# Saved with the project: the materials and the links.
path = os.path.join(tempfile.mkdtemp(prefix="oanim_materials_"), "materials.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = oanim.data.layers.active
assert sorted(materials()) == ["Shadow", "Skin"], materials()
assert layer.shape_material(0) == "Skin" and layer.shape_material(1) == "Shadow" and layer.shape_material(2) == ""
layer.set_shape_style(1, line_width=0.05)
assert abs(materials()["Shadow"]["line_width"] - 0.05) < 1e-6, "still linked after opening"

print("test_materials: OK")
