"""Brushes: the asset library, Moho's brush formats, importing, and shapes painted with a brush.

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
assert oanim.assets.library == "", "tests start with no asset library"
assert oanim.assets.brushes() == []
library = root / "Library"
assert oanim.ops.preferences.asset_library_set(directory=str(library)) == {'FINISHED'}
assert (library / "library.json").is_file() and (library / "Brushes").is_dir()
assert os.path.samefile(oanim.assets.library, library)
assert oanim.assets.brushes() == []

# Importing copies the brushes (settings and images) into the library.
assert oanim.ops.preferences.import_moho_brushes(directory=str(moho)) == {'FINISHED'}
brushes = {b["name"]: b for b in oanim.assets.brushes()}
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
assert oanim.ops.preferences.import_moho_brushes(directory=str(moho)) == {'FINISHED'}
assert len(oanim.assets.brushes()) == 4

# --- Dabs along a line ------------------------------------------------------------------------
# A line 1 long and 0.1 wide: Ink (spacing 10% of the width) puts a dab every 0.01, Arrow (200%)
# every 0.2.
assert oanim.assets.brush_dab_count("Ink", 1.0, 0.1) == 101, oanim.assets.brush_dab_count("Ink", 1.0, 0.1)
assert oanim.assets.brush_dab_count("Arrow_1_0_200", 1.0, 0.1) == 6

# --- Shapes with a brush ------------------------------------------------------------------------
layer = oanim.data.layers.active
layer.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)], closed=True, fill=True)
layer.add_curve([(1.0, 0.0), (1.5, 0.2)])
assert layer.shape_brush(0)["name"] == "", "lines start plain"

oanim.ops.mesh.select_none()
assert oanim.ops.mesh.brush_set(brush="Ink") == {'CANCELLED'}, "nothing selected"
layer.select_points([0])
assert oanim.ops.mesh.brush_set(brush="Ink") == {'FINISHED'}
b = layer.shape_brush(0)
assert b["name"] == "Ink" and b["align"] and abs(b["spacing"] - 0.1) < 1e-6, "the brush's settings come along"
assert layer.shape_brush(1)["name"] == "", "unselected shapes keep theirs"
assert oanim.ops.mesh.brush_set(brush="Nope") == {'CANCELLED'}

oanim.ops.ed.undo()
assert layer.shape_brush(0)["name"] == ""
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
