"""Brushes: the asset library, Moho's brush formats, importing, and shapes painted with a brush.

Run with:  SkyAnimate --background --python tests/test_brushes.py
"""

import json
import os
import pathlib
import struct
import tempfile
import zipfile
import zlib

import sky


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


root = pathlib.Path(tempfile.mkdtemp(prefix="sky_brushes_"))

# --- A Moho brush folder to import from -------------------------------------------------------
moho = root / "Moho Brushes"
moho.mkdir()
write_png(moho / "Ink.png")
write_mohobrush(moho / "Ink.mohobrush", "Ink.png", align=True, jitter=0.0, spacing=0.1)
(moho / "Hair").mkdir()
write_png(moho / "Hair" / "1.png")
write_png(moho / "Hair" / "2.png")
write_mohobrush(moho / "Hair.mohobrush", "Hair", randomize=True, randomInterval=4, sizeVariationAmp=0.5)
# An older brush: settings in the name (align 1, jitter 0 degrees, spacing 200%).
write_png(moho / "Arrow_1_0_200.png")
write_png(moho / "Plain.png")
(moho / "notes.txt").write_text("not a brush")

# --- The asset library --------------------------------------------------------------------------
assert sky.assets.library == "", "tests start with no asset library"
assert sky.assets.brushes() == []
library = root / "Library"
assert sky.ops.preferences.asset_library_set(directory=str(library)) == {'FINISHED'}
assert (library / "library.json").is_file() and (library / "Brushes").is_dir()
assert os.path.samefile(sky.assets.library, library)
assert sky.assets.brushes() == []

# Importing copies the brushes (settings and images) into the library.
assert sky.ops.preferences.import_moho_brushes(directory=str(moho)) == {'FINISHED'}
brushes = {b["name"]: b for b in sky.assets.brushes()}
assert sorted(brushes) == ["Arrow_1_0_200", "Hair", "Ink", "Plain"], sorted(brushes)
assert (library / "Brushes" / "Ink.mohobrush").is_file()
assert (library / "Brushes" / "Hair" / "2.png").is_file()
assert not (library / "Brushes" / "notes.txt").exists()

ink, hair, arrow, plain = brushes["Ink"], brushes["Hair"], brushes["Arrow_1_0_200"], brushes["Plain"]
assert ink["align"] and ink["jitter"] == 0.0 and abs(ink["spacing"] - 0.1) < 1e-6 and ink["images"] == 1
assert hair["images"] == 2 and hair["randomize"] and hair["randomInterval"] == 4
assert abs(hair["sizeVariationAmp"] - 0.5) < 1e-6
assert arrow["align"] and arrow["jitter"] == 0.0 and abs(arrow["spacing"] - 2.0) < 1e-6
assert not plain["align"] and abs(plain["spacing"] - 0.25) < 1e-6

# Importing again adds nothing and keeps what's there.
assert sky.ops.preferences.import_moho_brushes(directory=str(moho)) == {'FINISHED'}
assert len(sky.assets.brushes()) == 4

# --- Dabs along a line ------------------------------------------------------------------------
# A line 1 long and 0.1 wide: Ink (spacing 10% of the width) puts a dab every 0.01, Arrow (200%)
# every 0.2.
assert sky.assets.brush_dab_count("Ink", 1.0, 0.1) == 101, sky.assets.brush_dab_count("Ink", 1.0, 0.1)
assert sky.assets.brush_dab_count("Arrow_1_0_200", 1.0, 0.1) == 6

# --- Shapes with a brush ------------------------------------------------------------------------
layer = sky.data.layers.active
layer.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)], closed=True, fill=True)
layer.add_curve([(1.0, 0.0), (1.5, 0.2)])
assert layer.shape_brush(0)["name"] == "", "lines start plain"

sky.ops.mesh.select_none()
assert sky.ops.mesh.brush_set(brush="Ink") == {'CANCELLED'}, "nothing selected"
layer.select_points([0])
assert sky.ops.mesh.brush_set(brush="Ink") == {'FINISHED'}
b = layer.shape_brush(0)
assert b["name"] == "Ink" and b["align"] and abs(b["spacing"] - 0.1) < 1e-6, "the brush's settings come along"
assert layer.shape_brush(1)["name"] == "", "unselected shapes keep theirs"
assert sky.ops.mesh.brush_set(brush="Nope") == {'CANCELLED'}

sky.ops.ed.undo()
assert layer.shape_brush(0)["name"] == ""
sky.ops.ed.redo()
assert layer.shape_brush(0)["name"] == "Ink"

# Saved and opened with the document.
path = str(root / "brushed.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
sky.ops.mesh.select_all()
assert sky.ops.mesh.brush_set(brush="") == {'FINISHED'}
assert layer.shape_brush(0)["name"] == ""
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = sky.data.layers.active
b = layer.shape_brush(0)
assert b["name"] == "Ink" and b["align"] and abs(b["spacing"] - 0.1) < 1e-6, b
assert layer.shape_brush(1)["name"] == ""

print("Brush tests passed")
