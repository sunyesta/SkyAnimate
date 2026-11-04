"""Saving and opening .oanim files: round trips, Save Copy and Save Incremental, Revert, and
refusing or repairing bad files.

Run with:  OpenAnimate --background --python tests/test_save_and_open.py
"""

import json
import os
import tempfile
import zipfile

import oanim

layers = oanim.data.layers
doc = oanim.data.document
folder = tempfile.mkdtemp(prefix="oanim_test_")


def path(name):
    return os.path.join(folder, name)


def snapshot():
    """Everything that should survive a save and open, as plain values."""
    def layer_info(l):
        source = l.reference_source
        return (l.name, l.type, l.visible, l.render_visible, l.locked, l.label_color, round(l.location_x, 5), round(l.rotation, 5),
                round(l.opacity, 5), l.masks, source.name if source else None,
                [(round(x, 5), round(y, 5)) for x, y, _ in l.points], [round(r, 5) for r in l.radii], l.hidden_edges,
                l.curve_count, l.shape_count,
                [layer_info(c) for c in l.children])
    return (doc.name, doc.frame_start, doc.frame_end, doc.fps, doc.width, doc.height,
            doc.viewport_quality, doc.render_quality, doc.viewport_samples, doc.render_samples,
            [layer_info(l) for l in layers], [(t.name, t.body) for t in oanim.data.texts],
            layers.active.name)


# --- Build a document worth saving ---------------------------------------------------
art = layers.active
art.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)], closed=True, fill=True)
art.add_curve([(0.7, 0.0), (0.9, 0.3), (1.1, 0.0)])
art.select_points([5])
oanim.ops.mesh.peak()
art.label_color = "BLUE"
art.location_x = 0.25
art.rotation = 15.0
oanim.ops.mesh.select_none()
art.select_points([0])
art.mean_radius = 0.25
oanim.ops.mesh.select_none()
art.select_points([1, 2])
art.set_selected_edges_visible(False)
oanim.ops.mesh.select_none()
oanim.ops.layer.group()
holder = art.parent
layers.active = art
oanim.ops.layer.make_reference()
ref = layers.active
ref.name = "Mask"
oanim.ops.mask.add(layer=str(art.id), sources=str(ref.id), outside="1", expand="1", fills_only="1")
oanim.ops.mask.set(layer=str(art.id), index="0", render="0")
ref.visible = False
ref.opacity = 0.5
ref.render_visible = False
doc.name = "Walk"
doc.frame_end = 48
doc.fps = 12.0
doc.viewport_quality = 6
doc.render_quality = 48
doc.viewport_samples = 8
doc.render_samples = 2
# The Playback popover's settings (Blender's names).
assert (doc.sync_mode, doc.play_audio, doc.use_audio_scrub) == ("FRAME_DROP", True, False), "the defaults"
doc.sync_mode = "NONE"
doc.play_audio = False
doc.use_audio_scrub = True
oanim.data.texts.new().body = "print('hello from the file')\n"
layers.active = art
before = snapshot()

# --- Save ------------------------------------------------------------------------------
assert oanim.data.filepath == "" and oanim.data.is_dirty
walk = path("walk.oanim")
assert oanim.ops.wm.save_mainfile(filepath=walk) == {'FINISHED'}
assert oanim.data.filepath == walk and not oanim.data.is_dirty
assert oanim.ops.wm.save_as_mainfile() == {'CANCELLED'}, "without a window there's no dialog to ask for a path"

with zipfile.ZipFile(walk) as archive:
    stored = json.loads(archive.read("document.json"))
assert stored["format"] == "OpenAnimate" and stored["version"] == 5
group = stored["document"]["layers"][0]
assert group["type"] == "GROUP"
mask = group["children"][0]
assert "drawing" not in mask, "a reference stores no copy of the drawing"
masked = group["children"][1]
m = masked["masks"][0]
s = m["steps"][0]
assert (m["start"], s["mode"], s["layers"], m["enabled"], m["render"], s["grow"], s["fills_only"], s["filled"]) ==     ("EVERYTHING", "SUBTRACT", [mask["id"]], True, False, 1.0, True, False), m

# Saving without .oanim adds it.
assert oanim.ops.wm.save_as_mainfile(filepath=path("walk_alt")) == {'FINISHED'}
assert oanim.data.filepath == path("walk_alt.oanim") and os.path.exists(path("walk_alt.oanim"))

# --- New, then open ----------------------------------------------------------------------
art.name = "Changed"
assert oanim.data.is_dirty
assert oanim.ops.wm.read_homefile() == {'FINISHED'}
assert len(layers) == 1 and layers[0].name == "Layer 1" and oanim.data.filepath == ""
assert oanim.ops.wm.open_mainfile(filepath=walk) == {'FINISHED'}
assert snapshot() == before, "everything round-trips"
assert (doc.sync_mode, doc.play_audio, doc.use_audio_scrub) == ("NONE", False, True)
assert not oanim.data.is_dirty and not oanim.ops.ed.undo.poll(), "opening starts a fresh undo history"
reopened_ref = layers["Mask"]
assert reopened_ref.reference_source == layers["Layer 1"], "references still follow their original"
layers["Layer 1"].add_curve([(0, 0), (0.1, 0.1)])
assert reopened_ref.point_count == layers["Layer 1"].point_count

# --- Revert ------------------------------------------------------------------------------
layers["Layer 1"].name = "Scribbled"
oanim.ops.layer.new()
assert oanim.data.is_dirty
assert oanim.ops.wm.revert_mainfile() == {'FINISHED'}
assert snapshot() == before

# --- Save Copy and Save Incremental --------------------------------------------------------
assert oanim.ops.wm.save_copy(filepath=path("backup.oanim")) == {'FINISHED'}
assert oanim.data.filepath == walk, "a copy leaves the document on its own file"
assert oanim.ops.wm.save_incremental() == {'FINISHED'}
assert oanim.data.filepath == path("walk_001.oanim")
assert oanim.ops.wm.save_incremental() == {'FINISHED'}
assert oanim.data.filepath == path("walk_002.oanim")
open(path("walk_003.oanim"), "wb").close()
assert oanim.ops.wm.save_incremental() == {'FINISHED'}
assert oanim.data.filepath == path("walk_004.oanim"), "existing files are never overwritten"

# Names outside ASCII work too.
unicode_path = path("Ünïcode walk ✓.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=unicode_path) == {'FINISHED'}
assert oanim.ops.wm.read_homefile() == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=unicode_path) == {'FINISHED'}
assert snapshot() == before


# --- Bad files -----------------------------------------------------------------------------
def write_archive(name, document_json):
    with zipfile.ZipFile(path(name), "w") as archive:
        archive.writestr("document.json", document_json)
    return path(name)


current = snapshot()
assert oanim.ops.wm.open_mainfile(filepath=path("missing.oanim")) == {'CANCELLED'}
with open(path("plain.oanim"), "w") as f:
    f.write("not a zip")
assert oanim.ops.wm.open_mainfile(filepath=path("plain.oanim")) == {'CANCELLED'}
assert oanim.ops.wm.open_mainfile(filepath=write_archive("broken.oanim", "{")) == {'CANCELLED'}
newer = dict(stored, version=99)
assert oanim.ops.wm.open_mainfile(filepath=write_archive("newer.oanim", json.dumps(newer))) == {'CANCELLED'}
assert snapshot() == current, "a file that can't be read leaves the document alone"

# Damaged parts are dropped instead of trusted: a curve through a missing point, a shape edge
# past the end of its curve, a reference to a layer that isn't there, and a repeated id.
damaged = {
    "format": "SkyAnimate", "version": 1,
    "document": {"layers": [
        {"id": 5, "name": "A", "type": "VECTOR", "label_color": "NOT_A_COLOR",
         "drawing": {"points": [{"pos": [0, 0]}, {"pos": [1, 0]}],
                     "curves": [{"points": [{"point": 0}, {"point": 1}, {"point": 7}]}],
                     "shapes": [{"edges": [[0, 0], [0, 5], [3, 0]]}]}},
        {"id": 5, "name": "B", "type": "VECTOR", "reference_source": 99},
    ]},
}
assert oanim.ops.wm.open_mainfile(filepath=write_archive("damaged.oanim", json.dumps(damaged))) == {'FINISHED'}
# Files saved before the rename from SkyAnimate (.sky, "format": "SkyAnimate") open as they are, and
# scripts written for it (`import sky`) still run.
assert oanim.ops.wm.open_mainfile(filepath=write_archive("damaged.sky", json.dumps(damaged))) == {'FINISHED'}
import sky
assert sky is oanim and sky.data.layers["A"].point_count == 2
assert oanim.ops.wm.open_mainfile(filepath=write_archive("damaged.oanim", json.dumps(damaged))) == {'FINISHED'}
a, b = layers["A"], layers["B"]
assert (a.point_count, a.curve_count, a.shape_count) == (2, 1, 1)
assert a.label_color == "NONE", "unknown enum names fall back to the default"
assert a.id != b.id and b.reference_source is None
assert doc.frame_end == 250, "missing values take their defaults"
assert (doc.sync_mode, doc.play_audio, doc.use_audio_scrub) == ("FRAME_DROP", True, False)
oanim.ops.layer.new()
assert len({l.id for l in layers.all()}) == 3, "new layers get ids no loaded layer uses"

print("Save and open tests passed")
