"""Show Points: a button on each modifier that adds points (Mirror, Bevel, Resample, Boolean, the deforms), off by
default, saved with the file; modifiers that add none (Armature) have no such setting.

Run with:  OpenAnimate --background --python tests/test_modifier_show_points.py
"""

import os
import tempfile

import oanim

ops = oanim.ops
layer = oanim.data.layers.active
layer.name = "Art"
layer.add_curve([(-1.0, 0.0), (0.0, 0.5), (1.0, 0.0)])

for kind in ("MIRROR", "BEVEL", "RESAMPLE", "BOOLEAN", "CURVE_DEFORM", "CAGE_DEFORM", "MESH_DEFORM"):
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}, kind
    m = layer.modifiers[-1]
    assert m.get("show_points") is False, (kind, m)

sub = [m["type"] for m in layer.modifiers].index("RESAMPLE")
layer.modifier_set(sub, show_points=True)
assert layer.modifiers[sub]["show_points"] is True

# Armature adds no points: no Show Points to set.
assert ops.layer.new_bone() == {'FINISHED'}
bones = oanim.data.layers.active
oanim.data.layers.active = layer
assert ops.layer.move_to_group(group=str(bones.id)) == {'FINISHED'}
armature = [i for i, m in enumerate(layer.modifiers) if m["type"] == "ARMATURE"]
assert armature, [m["type"] for m in layer.modifiers]
assert "show_points" not in layer.modifiers[armature[0]]
try:
    layer.modifier_set(armature[0], show_points=True)
    raise AssertionError("Armature takes no show_points")
except KeyError:
    pass

# Saved with the file.
folder = tempfile.mkdtemp()
path = os.path.join(folder, "points.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
art = next(l for l in oanim.data.layers.all() if l.name == "Art")
shown = [m["type"] for m in art.modifiers if m.get("show_points")]
assert shown == ["RESAMPLE"], shown
print("Show Points tests passed")
