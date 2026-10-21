"""Brushes: the asset library (Brush Shapes, Brushes by category), upgrading a version 1 library of
Moho's brushes, and shapes painted with a brush.

Run with:  OpenAnimate --background --python tests/test_brushes.py
"""

import json
import os
import pathlib
import struct
import tempfile
import zipfile
import zlib

import _oanim
import oanim


def write_png(path, size=16, color=(0, 0, 0)):
    """A small RGB PNG: a dot of `color` on white, like Moho's ink brushes."""
    rows = b""
    for y in range(size):
        rows += b"\0"
        for x in range(size):
            inside = (x - size / 2) ** 2 + (y - size / 2) ** 2 < (size / 3) ** 2
            rows += bytes(color) if inside else b"\xff\xff\xff"

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    pathlib.Path(path).write_bytes(png)


def write_mohobrush(path, image, **settings):
    brush = {"version": 1, "align": False, "jitter": 6.283185, "spacing": 0.25, "angleDrift": 0.0,
             "randomize": False, "randomOrder": True, "mergedAlpha": False, "sizeVariationAmp": 0.0,
             "sizeVariationScale": 0.3, "randomInterval": 1,
             "brushFiles": [{"brushFileRef": {"relativeTo": "Project", "path": image}}]}
    brush.update(settings)
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("brush.json", json.dumps(brush))


root = pathlib.Path(tempfile.mkdtemp(prefix="oanim_brushes_"))

# --- A new library --------------------------------------------------------------------------------
assert oanim.assets.library == "", "tests start with no asset library"
assert oanim.assets.brushes() == []
fresh = root / "Fresh"
assert oanim.ops.preferences.asset_library_set(directory=str(fresh)) == {'FINISHED'}
assert (fresh / "Brush Shapes").is_dir() and (fresh / "Brushes").is_dir()
assert json.loads((fresh / "library.json").read_text())["version"] == 2
assert os.path.samefile(oanim.assets.library, fresh)
assert oanim.assets.brushes() == []
lib = {p["name"]: p for p in oanim.assets.brush_presets(True)}
assert sorted(lib) == ["Hard Round", "Soft Round"], "the built-in Basic brushes"
assert lib["Hard Round"]["group"] == "Basic" and lib["Hard Round"]["builtin"]
assert oanim.ops.brush.preset_remove(preset="Hard Round", library="1") == {'CANCELLED'}, "built in"

# Shapes are plain PNGs, or folders of them.
write_png(fresh / "Brush Shapes" / "Dot.png")
(fresh / "Brush Shapes" / "Pair").mkdir()
write_png(fresh / "Brush Shapes" / "Pair" / "a.png")
write_png(fresh / "Brush Shapes" / "Pair" / "b.png")
(fresh / "Brush Shapes" / "notes.txt").write_text("not a shape")
assert oanim.ops.preferences.brushes_reload() == {'FINISHED'}
assert {b["name"]: b["images"] for b in oanim.assets.brushes()} == {"Dot": 1, "Pair": 2}

# --- Upgrading a version 1 library (Moho's brushes in Brushes/) ------------------------------------
library = root / "Library"
old = library / "Brushes"
old.mkdir(parents=True)
(library / "library.json").write_text(json.dumps({"format": "SkyAnimate asset library", "version": 1}))
write_png(old / "Ink.png")
write_mohobrush(old / "Ink.mohobrush", "Ink.png", align=True, jitter=0.0, spacing=0.1)
(old / "Hair").mkdir()
write_png(old / "Hair" / "1.png")
write_png(old / "Hair" / "2.png")
write_mohobrush(old / "Hair.mohobrush", "Hair", randomize=True, randomInterval=4, sizeVariationAmp=0.5)
# Older brushes: settings in the name (align 1, jitter 0 degrees, spacing 200%), or none.
write_png(old / "Arrow_1_0_200.png")
write_png(old / "Plain.png")
write_png(old / "SimpleRound.png")
write_png(old / "Brush513.png")
(old / "notes.txt").write_text("not a brush")
assert oanim.ops.preferences.asset_library_set(directory=str(library)) == {'FINISHED'}
shapes = library / "Brush Shapes"
assert (shapes / "Ink.png").is_file() and (shapes / "Hair" / "2.png").is_file(), "the images became shapes"
assert (shapes / "Arrow_1_0_200.png").is_file(), "under their own names, so documents still find them"
assert not (library / "Brushes (old)").exists(), "the old folder goes once it's all copied"
assert not list((library / "Brushes").rglob("*.mohobrush"))
assert json.loads((library / "library.json").read_text())["version"] == 2
names = {b["name"]: b for b in oanim.assets.brushes()}
assert sorted(names) == ["Arrow_1_0_200", "Brush513", "Hair", "Ink", "Plain", "SimpleRound"], sorted(names)
assert names["Hair"]["images"] == 2

# Each brush became a brush file with its settings, in a category.
lib = {p["name"]: p for p in oanim.assets.brush_presets(True)}
assert (library / "Brushes" / "Other" / "Ink.oabrush").is_file()
ink, hair, arrow = lib["Ink"]["brush"], lib["Hair"]["brush"], lib["Arrow"]["brush"]
assert lib["Ink"]["shape"] == "Ink" and ink["align"] and ink["jitter"] == 0.0 and abs(ink["spacing"] - 0.1) < 1e-6
assert hair["randomize"] and hair["randomInterval"] == 4 and abs(hair["sizeVariationAmp"] - 0.5) < 1e-6
assert lib["Arrow"]["shape"] == "Arrow_1_0_200", "the preset drops the settings from its name"
assert arrow["align"] and arrow["jitter"] == 0.0 and abs(arrow["spacing"] - 2.0) < 1e-6
assert lib["SimpleRound"]["group"] == "Basic" and lib["Brush513"]["group"] == "Fun" and lib["Plain"]["group"] == "Other"
# Read again, nothing changes.
assert oanim.ops.preferences.brushes_reload() == {'FINISHED'}
assert len(oanim.assets.brush_presets(True)) == len(lib) and len(oanim.assets.brushes()) == 6

# --- Dabs along a line ------------------------------------------------------------------------
# A line 1 long and 0.1 wide, at the default spacing (25% of the width): a dab every 0.025.
assert oanim.assets.brush_dab_count("Ink", 1.0, 0.1) == 41, oanim.assets.brush_dab_count("Ink", 1.0, 0.1)

# --- Shapes with a brush ------------------------------------------------------------------------
layer = oanim.data.layers.active
layer.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)], closed=True, fill=True)
layer.add_curve([(1.0, 0.0), (1.5, 0.2)])
assert layer.shape_brush(0)["name"] == "", "lines start plain"

oanim.ops.mesh.select_none()
assert oanim.ops.mesh.brush_set(brush="Ink") == {'CANCELLED'}, "nothing selected"
layer.select_points([0])
assert oanim.ops.brush.setting_set(setting="spacing", value="0.5") == {'FINISHED'}
assert oanim.ops.mesh.brush_set(brush="Ink") == {'FINISHED'}
b = layer.shape_brush(0)
assert b["name"] == "Ink" and abs(b["spacing"] - 0.5) < 1e-6, "a shape changes only the shape"
assert layer.shape_brush(1)["name"] == "", "unselected shapes keep theirs"
assert oanim.ops.mesh.brush_set(brush="Nope") == {'CANCELLED'}
# A preset brings its settings.
assert oanim.ops.brush.preset_load(preset="Ink", library="1") == {'FINISHED'}
b = layer.shape_brush(0)
assert b["name"] == "Ink" and b["align"] and abs(b["spacing"] - 0.1) < 1e-6

oanim.ops.ed.undo()
oanim.ops.ed.undo()
assert layer.shape_brush(0)["name"] == ""
oanim.ops.ed.redo()
oanim.ops.ed.redo()
assert layer.shape_brush(0)["name"] == "Ink"

# Saved and opened with the document.
path = str(root / "brushed.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
oanim.ops.mesh.select_all()
assert oanim.ops.mesh.brush_set(brush="") == {'FINISHED'}
assert layer.shape_brush(0)["name"] == ""
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = oanim.data.layers.active
b = layer.shape_brush(0)
assert b["name"] == "Ink" and b["align"] and abs(b["spacing"] - 0.1) < 1e-6, b
assert layer.shape_brush(1)["name"] == ""

# --- Dabs stick to the artwork -------------------------------------------------------------------
# A brushed line drawn on the setup frame, then animated: on frame 10 its middle point moves right,
# stretching the first segment to 1.4 times its length and squashing the second to 0.6. Each dab
# stays at the same place on its segment (it doesn't slide along the line).
doc = oanim.data.document
doc.auto_key = True
doc.frame_current = 0
layer.add_curve([(0.0, 2.0), (1.0, 2.0), (2.0, 2.0)])
shape = layer.shape_count - 1
first = layer.point_count - 3
oanim.ops.mesh.select_none()
layer.select_points([first])
assert oanim.ops.mesh.brush_set(brush="Ink") == {'FINISHED'}
rest = _oanim.layer_shape_dabs(layer.id, shape)
assert len(rest) > 20 and all(a == 1.0 for x, y, a in rest)
doc.frame_current = 10
layer.set_point(first + 1, (1.4, 2.0))
moved = _oanim.layer_shape_dabs(layer.id, shape)
assert len(moved) == len(rest), "the same dabs"
for (x0, y0, a0), (x1, y1, a1) in zip(rest, moved):
    expected = x0 * 1.4 if x0 <= 1.0 else 1.4 + (x0 - 1.0) * 0.6
    assert abs(x1 - expected) < 1e-3 and abs(y1 - 2.0) < 1e-6, ("each dab keeps its place on the line", x0, x1)
# Squashed below 3/4, every other dab fades (here to 40%); the stretched segment keeps all of its.
assert all(a == 1.0 for x, y, a in moved if x < 1.4)
assert any(abs(a - 0.4) < 1e-3 for x, y, a in moved if x > 1.4), "a squashed segment thins out"
# Stretched past 1.5 times, dabs fade in halfway between its own (at 1.9 times, 80% of the way in).
doc.frame_current = 20
layer.set_point(first + 1, (1.9, 2.0))
stretched = _oanim.layer_shape_dabs(layer.id, shape)
fills = [x for x, y, a in stretched if abs(a - 0.8) < 1e-3]
assert len(fills) > 10 and all(x < 1.9 for x in fills), "a stretched segment fills its gaps"
# Back on the setup frame, the line is as drawn.
doc.frame_current = 0
assert _oanim.layer_shape_dabs(layer.id, shape) == rest

print("Brush tests passed")
