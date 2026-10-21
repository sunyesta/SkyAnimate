"""Brush settings brought over from Photoshop: the built-in Round tip, tip shape, Scattering,
Transfer, sections switched off, one setting changed on several shapes, and presets remembered.

Run with:  OpenAnimate --background --python tests/test_brush_settings.py
"""

import json
import pathlib
import tempfile

import oanim

ROUND = "Round (built-in)"
root = pathlib.Path(tempfile.mkdtemp(prefix="oanim_brush_settings_"))
library = root / "Library"
assert oanim.ops.preferences.asset_library_set(directory=str(library)) == {'FINISHED'}

layer = oanim.data.layers.active
layer.add_curve([(-1.0, 0.0), (1.0, 0.0)])
layer.add_curve([(-1.0, 0.5), (1.0, 0.5)])
oanim.ops.mesh.select_all()

# --- The built-in Round tip ----------------------------------------------------------------------
assert oanim.ops.mesh.brush_set(brush=ROUND) == {'FINISHED'}, "the Round tip needs no library shape"
assert abs(layer.shape_brush(0)["spacing"] - 0.25) < 1e-6, "a shape brings no settings"
assert oanim.ops.brush.preset_load(preset="Hard Round", library="1") == {'FINISHED'}, "the built-in preset does"
b = layer.shape_brush(0)
assert b["name"] == ROUND and abs(b["spacing"] - 0.1) < 1e-6 and b["hardness"] == 1.0
plain = len(oanim._oanim.layer_shape_dabs(layer.id, 0))
assert plain > 10, plain

# --- One setting, several shapes ------------------------------------------------------------------
oanim.ops.mesh.select_none()
layer.select_points([2, 3])
assert oanim.ops.brush.setting_set(setting="spacing", value="0.3") == {'FINISHED'}
oanim.ops.mesh.select_all()
assert oanim.ops.brush.setting_set(setting="hardness", value="0.4") == {'FINISHED'}
b0, b1 = layer.shape_brush(0), layer.shape_brush(1)
assert abs(b0["hardness"] - 0.4) < 1e-6 and abs(b1["hardness"] - 0.4) < 1e-6, "the setting goes to every selected shape"
assert abs(b0["spacing"] - 0.1) < 1e-6 and abs(b1["spacing"] - 0.3) < 1e-6, "their other settings stay their own"
oanim.ops.ed.undo()
assert layer.shape_brush(0)["hardness"] == 1.0
oanim.ops.ed.redo()
assert oanim.ops.brush.setting_set(setting="nope", value="1") == {'CANCELLED'}
assert oanim.ops.brush.setting_set(setting="count", value="lots") == {'CANCELLED'}

# --- Scattering: several stamps at each place -------------------------------------------------------
oanim.ops.mesh.select_none()
layer.select_points([0, 1])
assert oanim.ops.brush.setting_set(setting="count", value="3") == {'FINISHED'}
assert oanim.ops.brush.setting_set(setting="scatter", value="0.5") == {'FINISHED'}
dabs = oanim._oanim.layer_shape_dabs(layer.id, 0)
assert len(dabs) == plain * 3, (len(dabs), plain)
assert any(abs(y) > 0.001 for _, y, _ in dabs), "scattered stamps leave the line"
assert all(abs(y) < 0.1 for _, y, _ in dabs), "within half a line width either side"
assert dabs == oanim._oanim.layer_shape_dabs(layer.id, 0), "the same stamps every time"
# Switched off, the section keeps its values but doesn't apply.
assert oanim.ops.brush.setting_set(setting="scattering", value="0") == {'FINISHED'}
assert len(oanim._oanim.layer_shape_dabs(layer.id, 0)) == plain
assert layer.shape_brush(0)["count"] == 3
assert oanim.ops.brush.setting_set(setting="scattering", value="1") == {'FINISHED'}
assert oanim.ops.brush.setting_set(setting="countJitter", value="1") == {'FINISHED'}
fewer = len(oanim._oanim.layer_shape_dabs(layer.id, 0))
assert plain <= fewer < plain * 3, fewer

# --- Saved with the file ---------------------------------------------------------------------------
for key, value in (("tipAngle", "0.5"), ("roundness", "0.3"), ("flipX", "1"), ("sizeJitter", "0.25"),
                   ("sizeFadeIn", "4"), ("opacityJitter", "0.5"), ("opacityFadeOut", "6"), ("transfer", "0")):
    assert oanim.ops.brush.setting_set(setting=key, value=value) == {'FINISHED'}, key
want = layer.shape_brush(0)
path = str(root / "settings.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = oanim.data.layers.active
got = layer.shape_brush(0)
for key in want:
    if isinstance(want[key], float):
        assert abs(got[key] - want[key]) < 1e-4, (key, got[key], want[key])
    else:
        assert got[key] == want[key], (key, got[key], want[key])

# --- Presets remember where the settings came from ------------------------------------------------
oanim.ops.mesh.select_none()
layer.select_points([0, 1])
assert oanim.ops.brush.preset_add(name="Scatter Ink") == {'FINISHED'}
assert layer.shape_brush(0)["preset"] == "Scatter Ink", "the brush is now that preset's"
p = {x["name"]: x for x in oanim.assets.brush_presets()}["Scatter Ink"]
assert p["brush"]["preset"] == "", "a preset doesn't name itself"
oanim.ops.mesh.select_none()
layer.select_points([2, 3])
assert oanim.ops.brush.preset_load(preset="Scatter Ink") == {'FINISHED'}
assert layer.shape_brush(1)["preset"] == "Scatter Ink" and layer.shape_brush(1)["count"] == 3

# --- Library presets in folders (Photoshop's groups) ----------------------------------------------
(library / "Brushes" / "Inks").mkdir(parents=True, exist_ok=True)
(library / "Brushes" / "Inks" / "Wet.oabrush").write_text(json.dumps(
    {"version": 1, "name": "Wet", "brush": {"name": ROUND, "hardness": 0.2}}))
assert oanim.ops.preferences.brushes_reload() == {'FINISHED'}
lib = {x["name"]: x for x in oanim.assets.brush_presets(True)}
assert lib["Wet"]["group"] == "Inks" and not lib["Wet"]["shape_missing"]
assert oanim.ops.brush.preset_remove(preset="Wet", library="1") == {'CANCELLED'}
assert not (library / "Brushes" / "Inks" / "Wet.oabrush").exists()

# --- Stretched lines: new stamps fade in, or pop in whole ----------------------------------------
doc = oanim.data.document
doc.auto_key = True
doc.frame_current = 0
layer.add_curve([(0.0, 2.0), (1.0, 2.0), (2.0, 2.0)])
shape = layer.shape_count - 1
first = layer.point_count - 3
oanim.ops.mesh.select_none()
layer.select_points([first])
assert oanim.ops.mesh.brush_set(brush=ROUND) == {'FINISHED'}
doc.frame_current = 20
layer.set_point(first + 1, (1.9, 2.0))
faded = oanim._oanim.layer_shape_dabs(layer.id, shape)
assert any(0.0 < a < 1.0 for _, _, a in faded), "by default the gaps' stamps fade in"
assert oanim.ops.brush.setting_set(setting="popIn", value="1") == {'FINISHED'}
popped = oanim._oanim.layer_shape_dabs(layer.id, shape)
assert all(a == 1.0 for _, _, a in popped), "with Pop In they show whole or not at all"
assert len(popped) == sum(1 for _, _, a in faded if a >= 0.5), "each pops in halfway through its fade"
doc.frame_current = 0
doc.auto_key = False

# --- The brush eyedropper: another shape's whole brush ----------------------------------------------
oanim.ops.mesh.select_none()
layer.select_points([0, 1])
assert oanim.ops.brush.setting_set(setting="roundness", value="0.2") == {'FINISHED'}
source = layer.shape_brush(0)
oanim.ops.mesh.select_none()
layer.select_points([2, 3])
assert layer.shape_brush(1) != source
assert oanim.ops.brush.copy_from(layer=str(layer.id), shape="0") == {'FINISHED'}
assert layer.shape_brush(1) == source, "every setting comes along"
oanim.ops.ed.undo()
assert layer.shape_brush(1) != source
assert oanim.ops.brush.copy_from(layer=str(layer.id), shape="99") == {'CANCELLED'}
oanim.ops.mesh.select_none()
assert oanim.ops.brush.copy_from(layer=str(layer.id), shape="0") == {'CANCELLED'}, "nothing selected"

# --- The look eyedroppers: fill colour, line colour, line width ------------------------------------
layer.set_shape_style(0, fill=(1, 0, 0, 1), line=(0, 0, 1, 1))
oanim.ops.mesh.select_none()
layer.select_points([2, 3])
layer.set_shape_style(1, line_width=0.05)
before = layer.shape_style(1)
for what in ("FILL", "LINE", "WIDTH"):
    assert oanim.ops.mesh.style_copy_from(layer=str(layer.id), shape="0", what=what) == {'FINISHED'}, what
got, src = layer.shape_style(1), layer.shape_style(0)
assert tuple(got["fill"]) == tuple(src["fill"]) and tuple(got["line"]) == tuple(src["line"])
assert abs(got["line_width"] - src["line_width"]) < 1e-6 and abs(before["line_width"] - 0.05) < 1e-6
oanim.ops.ed.undo()
assert abs(layer.shape_style(1)["line_width"] - 0.05) < 1e-6, "each pick is an undo step"
assert oanim.ops.mesh.style_copy_from(layer=str(layer.id), shape="0", what="NOPE") == {'CANCELLED'}

print("brush settings: ok")
