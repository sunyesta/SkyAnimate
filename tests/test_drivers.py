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
    "1 if 2 > 1 else 0": 1, "max(1, 7, 3)": 7, "degrees(pi)": 180, "not 0 and 1": 1, "abs(-3) == 3": 1,
}
for text, want in cases.items():
    assert ops.driver_add(layer=B, path="opacity", expression=text) == {'FINISHED'}
    doc.frame_current += 1
    got = driver(B, "opacity")["value"]
    assert abs(got - want) < 1e-4, (text, got)
# Bad ones say why and leave the value alone.
for text, why in (("sinn(1)", "Unknown function"), ("1 +", "ends too early"), ("bogus * 2", "No variable")):
    assert ops.driver_add(layer=B, path="opacity", expression=text) == {'FINISHED'}
    doc.frame_current += 1
    assert why in driver(B, "opacity")["error"], (text, driver(B, "opacity")["error"])

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

# --- Saved with the file --------------------------------------------------------------------------------
before = [(d["target"], d["expression"], d["type"], d["remap"]) for d in oanim.data.drivers]
path = str(pathlib.Path(tempfile.mkdtemp()) / "drivers.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
after = [(d["target"], d["expression"], d["type"], d["remap"]) for d in oanim.data.drivers]
assert before == after, (before, after)

print("drivers: ok")
