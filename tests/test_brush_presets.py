"""Brush presets: file presets (saved in the .oanim with a copy of their shape), library presets
(<library>/Brush Presets), loading, auto-renaming and shapes copied into the library.

Run with:  OpenAnimate --background --python tests/test_brush_presets.py
"""

import json
import pathlib
import struct
import tempfile
import zipfile
import zlib

import oanim


def write_png(path, size=16, color=(0, 0, 0)):
    """A small RGB PNG: a dot of `color` on white."""
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


def presets(library=False):
    return {p["name"]: p for p in oanim.assets.brush_presets(library)}


root = pathlib.Path(tempfile.mkdtemp(prefix="oanim_presets_"))
moho = root / "Moho"
moho.mkdir()
write_png(moho / "Ink.png")
with zipfile.ZipFile(moho / "Ink.mohobrush", "w") as z:
    z.writestr("brush.json", json.dumps({"version": 1, "align": True, "jitter": 0.0, "spacing": 0.1,
                                         "brushFiles": [{"brushFileRef": {"relativeTo": "Project",
                                                                          "path": "Ink.png"}}]}))
library = root / "Library"
assert oanim.ops.preferences.asset_library_set(directory=str(library)) == {'FINISHED'}
assert (library / "Brush Presets").is_dir(), "the library gets a presets folder"
assert oanim.ops.preferences.import_moho_brushes(directory=str(moho)) == {'FINISHED'}
assert presets() == {} and presets(True) == {}

layer = oanim.data.layers.active
layer.add_curve([(-0.5, 0.0), (0.5, 0.0)])
layer.add_curve([(-0.5, 0.5), (0.5, 0.5)])
oanim.ops.mesh.select_none()
assert oanim.ops.brush.preset_add(name="Nothing") == {'CANCELLED'}, "no shape selected"
layer.select_points([0])
assert oanim.ops.mesh.brush_set(brush="Ink") == {'FINISHED'}

# --- File presets ------------------------------------------------------------------------------
assert oanim.ops.brush.preset_add(name="Ink") == {'FINISHED'}
assert oanim.ops.brush.preset_add(name="Ink") == {'FINISHED'}
p = presets()
assert sorted(p) == ["Ink", "Ink.001"], "a taken name gets a number, no question asked"
assert p["Ink"]["shape"] == "Ink" and p["Ink"]["shape_images"] == 1, "the preset carries its shape"
assert p["Ink"]["brush"]["align"] and abs(p["Ink"]["brush"]["spacing"] - 0.1) < 1e-6
oanim.ops.ed.undo()
assert sorted(presets()) == ["Ink"], "adding is undoable"
oanim.ops.ed.redo()
assert sorted(presets()) == ["Ink", "Ink.001"]

# Loading copies the settings into the selected shapes.
oanim.ops.mesh.select_none()
layer.select_points([2])
assert layer.shape_brush(1)["name"] == ""
assert oanim.ops.brush.preset_load(preset="Ink") == {'FINISHED'}
assert layer.shape_brush(1)["name"] == "Ink" and layer.shape_brush(1)["align"]
oanim.ops.ed.undo()
assert layer.shape_brush(1)["name"] == ""
assert oanim.ops.brush.preset_load(preset="Nope") == {'CANCELLED'}

assert oanim.ops.brush.preset_remove(preset="Ink.001") == {'FINISHED'}
assert sorted(presets()) == ["Ink"]
oanim.ops.ed.undo()
assert sorted(presets()) == ["Ink", "Ink.001"], "removing a file preset is undoable"
assert oanim.ops.brush.preset_remove(preset="Ink.001") == {'FINISHED'}

# Saved in the file, shape and all: opened without the library's shape, it still paints.
path = str(root / "presets.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
with zipfile.ZipFile(path) as z:
    assert any(n.startswith("brush_shapes/") and n.endswith(".png") for n in z.namelist()), z.namelist()
(library / "Brushes" / "Ink.mohobrush").unlink()
(library / "Brushes" / "Ink.png").unlink()
assert oanim.ops.preferences.brushes_reload() == {'FINISHED'}
assert oanim.assets.brushes() == []
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = oanim.data.layers.active
p = presets()
assert sorted(p) == ["Ink"] and not p["Ink"]["shape_missing"], "the file's copy stands in for the shape"
assert oanim.assets.brush_dab_count("Ink", 1.0, 0.1) > 0, "the carried shape can be drawn"
oanim.ops.mesh.select_none()
layer.select_points([2])
assert oanim.ops.brush.preset_load(preset="Ink") == {'FINISHED'}
assert layer.shape_brush(1)["name"] == "Ink"

# --- Saving to the library ---------------------------------------------------------------------
assert oanim.ops.brush.preset_save_to_library(preset="Ink") == {'FINISHED'}
assert (library / "Brush Presets" / "Ink.json").is_file()
assert (library / "Brushes" / "Ink.png").is_file(), "the library lacked the shape: it's copied in"
lib = presets(True)
assert sorted(lib) == ["Ink"] and lib["Ink"]["shape"] == "Ink" and not lib["Ink"]["shape_missing"]
assert abs(lib["Ink"]["brush"]["spacing"] - 0.1) < 1e-6

# Again: the preset name gets a number; the same shape is reused, not copied again.
assert oanim.ops.brush.preset_save_to_library(preset="Ink") == {'FINISHED'}
lib = presets(True)
assert sorted(lib) == ["Ink", "Ink.001"] and lib["Ink.001"]["shape"] == "Ink"
assert not (library / "Brushes" / "Ink.001.png").exists()

# Another image under the shape's name in the library: the copy gets a number and the saved preset
# points at it.
write_png(library / "Brushes" / "Ink.png", size=20, color=(40, 40, 40))
assert oanim.ops.preferences.brushes_reload() == {'FINISHED'}
assert oanim.ops.brush.preset_save_to_library(preset="Ink") == {'FINISHED'}
lib = presets(True)
assert lib["Ink.002"]["shape"] == "Ink.001", lib["Ink.002"]
assert (library / "Brushes" / "Ink.001.png").is_file()

# --- A preset whose shape is missing ------------------------------------------------------------
(library / "Brush Presets" / "Gone.json").write_text(json.dumps(
    {"version": 1, "name": "Gone", "brush": {"name": "Vanished", "spacing": 0.5, "align": False}}))
assert oanim.ops.preferences.brushes_reload() == {'FINISHED'}
lib = presets(True)
assert lib["Gone"]["shape_missing"]
assert oanim.ops.brush.preset_load(preset="Gone", library="1") == {'FINISHED'}
b = layer.shape_brush(1)
assert b["name"] == "Ink", "the shape stays when the preset's is missing"
assert abs(b["spacing"] - 0.5) < 1e-6 and not b["align"], "the rest of the preset applies"

# Deleting from the library removes the file (no undo step: the document didn't change).
assert oanim.ops.brush.preset_remove(preset="Gone", library="1") == {'CANCELLED'}
assert not (library / "Brush Presets" / "Gone.json").exists()
assert "Gone" not in presets(True)
assert oanim.ops.brush.preset_remove(preset="Gone", library="1") == {'CANCELLED'}

print("brush presets: ok")
