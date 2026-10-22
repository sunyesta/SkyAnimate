"""Drivers: expressions, variables reading other values, Average/Sum/Min/Max, remap, loops, the
clipboard (Copy As New Driver, Paste Driver), shape colours, and saving.

Run with:  OpenAnimate --background --python tests/test_drivers.py
"""

import json
import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
doc.auto_key = False
ops = oanim.ops.anim
a = oanim.data.layers.active
a.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5)], closed=True, fill=True)
oanim.ops.layer.new()
b = oanim.data.layers.active
A, B = str(a.id), str(b.id)


def driver(layer, path, component=-1):
    for d in oanim.data.drivers:
        t = d["target"]
        if t["layer"] == int(layer) and t["path"] == path and t["component"] == component:
            return d
    return None


# --- An expression of the frame ------------------------------------------------------------------
assert ops.driver_add(layer=B, path="opacity", expression="frame / 100") == {'FINISHED'}
doc.frame_current = 50
assert abs(b.opacity - 0.5) < 1e-5, b.opacity
doc.frame_current = 20
assert abs(b.opacity - 0.2) < 1e-5
assert not driver(B, "opacity")["error"]

# --- A variable reading another layer's channel -----------------------------------------------------
v = json.dumps([{"name": "r", "layer": a.id, "path": "rotation", "component": -1}])
assert ops.driver_add(layer=B, path="rotation", vars=v, expression="r * 2") == {'FINISHED'}
a.rotation = 0.25
doc.frame_current = 21
assert abs(b.rotation - 0.5) < 1e-5, b.rotation
# The driven value is the driver's: changing it by hand doesn't stick, and nothing gets keyed.
doc.auto_key = True
b.rotation = 3.0
doc.frame_current = 22
assert abs(b.rotation - 0.5) < 1e-5, b.rotation
assert b.keyframes("rotation") == [], "a driven value is never keyed"
doc.auto_key = False

# One component of a two-number channel.
v = json.dumps([{"name": "var", "layer": a.id, "path": "position", "component": 0}])
assert ops.driver_add(layer=B, path="position", component="1", vars=v) == {'FINISHED'}
a.location_x = 0.3
doc.frame_current = 23
assert abs(b.location_y - 0.3) < 1e-5, b.location_y

# --- Types, remap ------------------------------------------------------------------------------------
v = json.dumps([{"name": "x", "layer": a.id, "path": "rotation"}, {"name": "y", "layer": a.id, "path": "opacity"}])
for kind, want in (("AVERAGE", (0.25 + 1.0) / 2), ("SUM", 1.25), ("MIN", 0.25), ("MAX", 1.0)):
    assert ops.driver_add(layer=B, path="scale", component="0", vars=v, type=kind) == {'FINISHED'}
    doc.frame_current += 1
    assert abs(b.scale_x - want) < 1e-5, (kind, b.scale_x)
assert ops.driver_set(layer=B, path="scale", component="0", type="EXPRESSION", expression="x",
                      remap="1", from_min="0", from_max="0.5", to_min="1", to_max="3") == {'FINISHED'}
doc.frame_current += 1
assert abs(b.scale_x - 2.0) < 1e-5, b.scale_x  # 0.25 is halfway from 0 to 0.5: halfway from 1 to 3
a.rotation = 5.0
doc.frame_current += 1
assert abs(b.scale_x - 3.0) < 1e-5, "clamped to the range"

# --- Expressions ------------------------------------------------------------------------------------
cases = {
    "2 + 3 * 4": 14, "-2 ** 2": -4, "(1 + 2) * 3": 9, "7 % 3": 1, "2 ^ 3": 8,
    "clamp(5, 0, 1)": 1, "lerp(0, 10, 0.25)": 2.5, "smoothstep(0, 1, 0.5)": 0.5,
    "1 if 2 > 1 else 0": 1, "#2 + 1": 3, " #pi > 3": 1, "max(1, 7, 3)": 7, "degrees(pi)": 180, "not 0 and 1": 1, "abs(-3) == 3": 1,
}
# (On Scale Y: Opacity, as in Blender, is held to 0..1.)
for text, want in cases.items():
    assert ops.driver_add(layer=B, path="scale", component="1", expression=text) == {'FINISHED'}
    doc.frame_current += 1
    got = driver(B, "scale", 1)["value"]
    assert abs(got - want) < 1e-4, (text, got)
# Bad ones say why and leave the value alone.
for text, why in (("sinn(1)", "Unknown function"), ("1 +", "ends too early"), ("bogus * 2", "No variable")):
    assert ops.driver_add(layer=B, path="scale", component="1", expression=text) == {'FINISHED'}
    doc.frame_current += 1
    assert why in driver(B, "scale", 1)["error"], (text, driver(B, "scale", 1)["error"])
assert ops.driver_add(layer=B, path="opacity", expression="5") == {'FINISHED'}
doc.frame_current += 1
assert b.opacity == 1.0, "a driven value is held to its field's range (Blender's)"

# --- A loop is caught, not evaluated -----------------------------------------------------------------
va = json.dumps([{"name": "var", "layer": b.id, "path": "rotation"}])
assert ops.driver_add(layer=A, path="rotation", vars=va) == {'FINISHED'}
doc.frame_current += 1
assert "Loop" in driver(A, "rotation")["error"] and "Loop" in driver(B, "rotation")["error"]
assert ops.driver_remove(layer=A, path="rotation") == {'FINISHED'}
assert driver(A, "rotation") is None

# --- The clipboard ------------------------------------------------------------------------------------
assert ops.driver_copy_new(layer=A, path="opacity") == {'CANCELLED'}  # no undo step: nothing changed
assert oanim._oanim.driver_clipboard_size() == 1
assert ops.driver_remove(layer=B, path="opacity") == {'FINISHED'}
assert ops.driver_paste(layer=B, path="opacity") == {'FINISHED'}
a.opacity = 0.4
doc.frame_current += 1
assert abs(b.opacity - 0.4) < 1e-5, "B's opacity follows A's"
# A two-number field copies as two drivers, onto a two-number field.
assert ops.driver_copy_new(layer=A, path="position") == {'CANCELLED'}
assert oanim._oanim.driver_clipboard_size() == 2
assert ops.driver_paste(layer=B, path="opacity") == {'CANCELLED'}, "two numbers onto one"
camera = oanim.data.camera
assert ops.driver_paste(layer=str(camera.id), path="position") == {'FINISHED'}
a.location_x, a.location_y = 0.7, -0.2
doc.frame_current += 1
assert [round(x, 4) for x in (camera.location_x, camera.location_y)] == [0.7, -0.2], "the camera follows A"

# --- Shape colours ------------------------------------------------------------------------------------
shape_id = a.shape_ids[0]
if True:
    path = "shapes[%d].fill" % shape_id
    assert ops.driver_add(layer=A, path=path, expression="0.5") == {'FINISHED'}
    doc.frame_current += 1
    assert tuple(round(c, 3) for c in a.shape_style(0)["fill"]) == (0.5, 0.5, 0.5, 0.5)
    assert len([d for d in oanim.data.drivers if d["target"]["path"] == path]) == 4, "one per channel"

# A shape's Transform: its points' average change from the setup pose, moved together.
path = "shapes[%d].position" % shape_id
assert ops.driver_add(layer=A, path=path, component="0", expression="0.25") == {'FINISHED'}
doc.frame_current += 1
d = driver(A, path, 0)
assert not d["error"] and abs(d["value"] - 0.25) < 1e-5, d
assert d["label"].endswith("Position X"), d["label"]
doc.auto_key = True
doc.frame_current += 1
assert abs(driver(A, path, 0)["value"] - 0.25) < 1e-5, "steady, and never keyed as an edit"
doc.auto_key = False
assert ops.driver_remove(layer=A, path=path, component="0") == {'FINISHED'}
# On frame 0 the row is the shape's Position (the middle of its points), and reading it follows them
# as they move: Line Width driven by it changes as the shape does.
frame = doc.frame_current
doc.frame_current = 0
assert ops.driver_copy_new(layer=A, path=path, component="0") == {'CANCELLED'}  # no undo step
assert ops.driver_paste(layer=A, path="shapes[%d].line_width" % shape_id) == {'FINISHED'}
# The triangle's middle, plus the 0.25 the driver above moved it (a value keeps its last driven
# value once its driver goes, as in Blender).
mean_x = (-0.5 + 0.5 + 0.5) / 3 + 0.25
assert abs(driver(A, "shapes[%d].line_width" % shape_id)["value"] - mean_x) < 1e-4, oanim.data.drivers
assert ops.driver_remove(layer=A, path="shapes[%d].line_width" % shape_id) == {'FINISHED'}
doc.frame_current = frame

# --- Settings: values that aren't keyed can still be driven, as in Blender --------------------------
base = "shapes[%d]." % shape_id
assert ops.driver_add(layer=A, path=base + "line_width", expression="0.02 * %d / 2" % doc.height) == {'FINISHED'}
assert ops.driver_add(layer=A, path=base + "has_fill", expression="frame % 2") == {'FINISHED'}
assert ops.driver_add(layer=A, path="props.locked", expression="1") == {'FINISHED'}
assert ops.driver_add(layer="0", path="background", expression="0.25") == {'FINISHED'}
doc.frame_current = 101
assert abs(a.shape_style(0)["line_width"] - 0.02) < 1e-6, a.shape_style(0)  # driven in pixels, stored in units
assert driver(A, base + "has_fill")["value"] == 1.0
doc.frame_current = 102
assert driver(A, base + "has_fill")["value"] == 0.0, "a checkbox is on from 0.5"
assert a.locked is True
bg = [d for d in oanim.data.drivers if d["target"]["layer"] == 0]
assert len(bg) == 4 and all(abs(d["value"] - 0.25) < 1e-6 for d in bg), bg
assert bg[0]["label"] == "Document: Background Color R", bg[0]["label"]
# Text, and settings Blender doesn't drive, can't be.
assert ops.driver_add(layer=A, path="props.name", expression="1") == {'CANCELLED'}
for p in (base + "line_width", base + "has_fill", "props.locked"):
    assert ops.driver_remove(layer=A, path=p) == {'FINISHED'}
assert ops.driver_remove(layer="0", path="background") == {'FINISHED'}
a.locked = False

# --- Shapes sharing a material: the look is the material's, and so is its driver ------------------------
oanim.ops.layer.new()
m = oanim.data.layers.active
M = str(m.id)
m.add_curve([(0, 0), (1, 0), (1, 1), (0, 1)], closed=True, fill=True)
m.add_curve([(2, 0), (3, 0), (3, 1), (2, 1)], closed=True, fill=True)
oanim.ops.mesh.select_none()
m.select_points([0, 1, 2, 3])
assert oanim.ops.material.new(name="Skin") == {'FINISHED'}
oanim.ops.mesh.select_none()
m.select_points([4, 5, 6, 7])
assert oanim.ops.material.assign(material="Skin") == {'FINISHED'}
first = m.shape_ids[0]
# Made on one shape (as before materials took drivers), it moves onto the material.
assert ops.driver_add(layer=M, path="shapes[%d].line_width" % first, expression="frame") == {'FINISHED'}
doc.frame_current = 7
on_material = [d for d in oanim.data.drivers if d["target"]["path"].startswith("materials[")]
assert len(on_material) == 1 and on_material[0]["target"]["layer"] == 0, on_material
assert on_material[0]["label"] == "Material Skin: Line Width", on_material[0]["label"]
px = doc.height / 2
for s in (0, 1):
    assert abs(m.shape_style(s)["line_width"] * px - 7) < 1e-3, (s, m.shape_style(s))
assert abs([x for x in oanim.data.materials if x["name"] == "Skin"][0]["line_width"] * px - 7) < 1e-3
doc.frame_current = 8
assert abs(m.shape_style(1)["line_width"] * px - 8) < 1e-3, "every shape sharing it follows, frame by frame"
p = on_material[0]["target"]["path"]
assert ops.driver_remove(layer="0", path=p) == {'FINISHED'}

# --- Saved with the file --------------------------------------------------------------------------------
before = [(d["target"], d["expression"], d["type"], d["remap"]) for d in oanim.data.drivers]
path = str(pathlib.Path(tempfile.mkdtemp()) / "drivers.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
after = [(d["target"], d["expression"], d["type"], d["remap"]) for d in oanim.data.drivers]
assert before == after, (before, after)

print("drivers: ok")
