"""Bone display: each bone has a Shape (Bone, Pin with a size, or Custom: a vector layer's drawing)
and a Bone Color (Blender's Default Colors, its 15 theme sets, or a Custom set of three shades). Both
only change how the viewport draws the bone, and both are saved.

Run with:  SkyAnimate --background --python tests/test_bone_shapes.py
"""

import pathlib
import tempfile

import sky

doc = sky.data.document
doc.frame_current = 0
sky.ops.layer.new()
teeth = sky.data.layers.active
teeth.name = "top teeth"
teeth.add_curve([(0.0, 1.0), (1.0, 1.0), (1.0, 1.2), (0.0, 1.2)], closed=True, fill=True)
sky.ops.layer.new_bone()
rig = sky.data.layers.active
sky.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
sky.ops.armature.bone_add(name="pin", head_x="2", head_y="0", tail_x="2.1", tail_y="0", parent="")


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


# --- Shape ------------------------------------------------------------------------------------
assert bone("arm")["shape"] == "BONE", "new bones are drawn as bones"
rig.bone_set("pin", shape="PIN", pin_size=1.5)
assert bone("pin")["shape"] == "PIN" and abs(bone("pin")["pin_size"] - 1.5) < 1e-6
try:
    rig.bone_set("arm", shape="STAR")
    raise AssertionError("an unknown shape is refused")
except ValueError:
    pass
rig.bone_set("arm", shape="CUSTOM", shape_layer="top teeth")
assert bone("arm")["shape"] == "CUSTOM" and bone("arm")["shape_layer"] == "top teeth"
try:
    rig.bone_set("arm", shape_layer="no such layer")
    raise AssertionError("only an existing vector layer can be the shape")
except ValueError:
    pass

# It is how the bone is drawn, nothing more: the bone poses as before.
doc.frame_current = 5
rig.bone_set("pin", angle=1.0)
doc.frame_current = 6
doc.frame_current = 5
assert abs(bone("pin")["angle"] - 1.0) < 1e-5 and bone("pin")["shape"] == "PIN"
doc.frame_current = 0

# --- Bone Color ---------------------------------------------------------------------------------
assert bone("arm")["color_set"] == "DEFAULT"
rig.bone_set("arm", color_set="THEME01")
red = bone("arm")["colors"]
assert bone("arm")["color_set"] == "THEME01"
assert all(abs(a - b) < 1e-3 for a, b in zip(red[0], (0x9a / 255, 0, 0))), ("Blender's red set", red)
# Custom starts from the set it had, as in Blender.
rig.bone_set("arm", color_set="CUSTOM")
assert bone("arm")["color_set"] == "CUSTOM" and bone("arm")["colors"] == red
rig.bone_set("arm", custom_colors=((0.1, 0.2, 0.3), (0.4, 0.5, 0.6), (0.7, 0.8, 0.9)))
assert all(abs(a - b) < 1e-6 for a, b in zip(bone("arm")["colors"][1], (0.4, 0.5, 0.6)))
try:
    rig.bone_set("arm", color_set="THEME99")
    raise AssertionError("an unknown colour set is refused")
except ValueError:
    pass

# Copy Bone Color to Selected: the active bone's colours go to the other selected bones.
sky.ops.armature.select_none()
rig.select_bone("pin")
rig.select_bone("arm", extend=True)  # active
assert sky.ops.armature.copy_bone_color_to_selected() == {'FINISHED'}
assert bone("pin")["color_set"] == "CUSTOM" and bone("pin")["colors"] == bone("arm")["colors"]
sky.ops.armature.select_none()
rig.select_bone("arm")
assert sky.ops.armature.copy_bone_color_to_selected() == {'CANCELLED'}, "no other bone selected"

# --- Saved and opened again ----------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_shapes_")) / "shapes.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in sky.data.layers if l.type == "BONE")
assert bone("pin")["shape"] == "PIN" and abs(bone("pin")["pin_size"] - 1.5) < 1e-6
assert bone("arm")["shape"] == "CUSTOM" and bone("arm")["shape_layer"] == "top teeth"
assert bone("arm")["color_set"] == "CUSTOM"
assert all(abs(a - b) < 1e-4 for a, b in zip(bone("arm")["colors"][2], (0.7, 0.8, 0.9)))

# The shape's layer gone: the bone keeps the setting but has no layer to draw.
teeth = next(l for l in sky.data.layers if l.name == "top teeth")
sky.data.layers.active = teeth
assert sky.ops.layer.delete() == {'FINISHED'}
assert bone("arm")["shape"] == "CUSTOM" and bone("arm")["shape_layer"] == ""

print("Bone shape tests passed")
