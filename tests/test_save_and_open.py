"""Saving and opening .sky files: round trips, Save Copy and Save Incremental, Revert, and
refusing or repairing bad files.

Run with:  SkyAnimate --background --python tests/test_save_and_open.py
"""

import json
import os
import tempfile
import zipfile

import sky

layers = sky.data.layers
doc = sky.data.document
folder = tempfile.mkdtemp(prefix="sky_test_")


def path(name):
    return os.path.join(folder, name)


def snapshot():
    """Everything that should survive a save and open, as plain values."""
    def layer_info(l):
        source = l.reference_source
        return (l.name, l.type, l.visible, l.locked, l.label_color, round(l.location_x, 5), round(l.rotation, 5),
                round(l.opacity, 5), l.mask_mode, l.group_mask, source.name if source else None,
                [(round(x, 5), round(y, 5)) for x, y, _ in l.points], [round(r, 5) for r in l.radii], l.hidden_edges, l.corners,
                l.curve_count, l.shape_count,
                [layer_info(c) for c in l.children])
    return (doc.name, doc.frame_start, doc.frame_end, doc.fps, doc.width, doc.height,
            doc.viewport_quality, doc.render_quality,
            [layer_info(l) for l in layers], [(t.name, t.body) for t in sky.data.texts],
            layers.active.name)


# --- Build a document worth saving ---------------------------------------------------
art = layers.active
art.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)], closed=True, fill=True)
art.add_curve([(0.7, 0.0), (0.9, 0.3), (1.1, 0.0)])
art.select_points([5])
sky.ops.mesh.peak()
art.label_color = "BLUE"
art.location_x = 0.25
art.rotation = 15.0
sky.ops.mesh.select_none()
art.select_points([0])
art.mean_radius = 0.25
sky.ops.mesh.select_none()
art.select_points([1, 2])
art.set_selected_edges_visible(False)
art.set_selected_corner("SHARP")
sky.ops.mesh.select_none()
sky.ops.layer.group()
holder = art.parent
holder.group_mask = "HIDE_ALL"
layers.active = art
sky.ops.layer.make_reference()
ref = layers.active
ref.name = "Mask"
ref.mask_mode = "ADD_INVISIBLE"
ref.opacity = 0.5
doc.name = "Walk"
doc.frame_end = 48
doc.fps = 12.0
doc.viewport_quality = 6
doc.render_quality = 48
sky.data.texts.new().body = "print('hello from the file')\n"
layers.active = art
before = snapshot()

# --- Save ------------------------------------------------------------------------------
assert sky.data.filepath == "" and sky.data.is_dirty
walk = path("walk.sky")
assert sky.ops.wm.save_mainfile(filepath=walk) == {'FINISHED'}
assert sky.data.filepath == walk and not sky.data.is_dirty
assert sky.ops.wm.save_as_mainfile() == {'CANCELLED'}, "without a window there's no dialog to ask for a path"

with zipfile.ZipFile(walk) as archive:
    stored = json.loads(archive.read("document.json"))
assert stored["format"] == "SkyAnimate" and stored["version"] == 1
group = stored["document"]["layers"][0]
assert group["type"] == "GROUP" and group["group_mask"] == "HIDE_ALL"
mask = group["children"][0]
assert mask["mask_mode"] == "ADD_INVISIBLE" and "drawing" not in mask, "a reference stores no copy of the drawing"

# Saving without .sky adds it.
assert sky.ops.wm.save_as_mainfile(filepath=path("walk_alt")) == {'FINISHED'}
assert sky.data.filepath == path("walk_alt.sky") and os.path.exists(path("walk_alt.sky"))

# --- New, then open ----------------------------------------------------------------------
art.name = "Changed"
assert sky.data.is_dirty
assert sky.ops.wm.read_homefile() == {'FINISHED'}
assert len(layers) == 1 and layers[0].name == "Layer 1" and sky.data.filepath == ""
assert sky.ops.wm.open_mainfile(filepath=walk) == {'FINISHED'}
assert snapshot() == before, "everything round-trips"
assert not sky.data.is_dirty and not sky.ops.ed.undo.poll(), "opening starts a fresh undo history"
reopened_ref = layers["Mask"]
assert reopened_ref.reference_source == layers["Layer 1"], "references still follow their original"
layers["Layer 1"].add_curve([(0, 0), (0.1, 0.1)])
assert reopened_ref.point_count == layers["Layer 1"].point_count

# --- Revert ------------------------------------------------------------------------------
layers["Layer 1"].name = "Scribbled"
sky.ops.layer.new()
assert sky.data.is_dirty
assert sky.ops.wm.revert_mainfile() == {'FINISHED'}
assert snapshot() == before

# --- Save Copy and Save Incremental --------------------------------------------------------
assert sky.ops.wm.save_copy(filepath=path("backup.sky")) == {'FINISHED'}
assert sky.data.filepath == walk, "a copy leaves the document on its own file"
assert sky.ops.wm.save_incremental() == {'FINISHED'}
assert sky.data.filepath == path("walk_001.sky")
assert sky.ops.wm.save_incremental() == {'FINISHED'}
assert sky.data.filepath == path("walk_002.sky")
open(path("walk_003.sky"), "wb").close()
assert sky.ops.wm.save_incremental() == {'FINISHED'}
assert sky.data.filepath == path("walk_004.sky"), "existing files are never overwritten"

# Names outside ASCII work too.
unicode_path = path("Ünïcode walk ✓.sky")
assert sky.ops.wm.save_as_mainfile(filepath=unicode_path) == {'FINISHED'}
assert sky.ops.wm.read_homefile() == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=unicode_path) == {'FINISHED'}
assert snapshot() == before


# --- Bad files -----------------------------------------------------------------------------
def write_archive(name, document_json):
    with zipfile.ZipFile(path(name), "w") as archive:
        archive.writestr("document.json", document_json)
    return path(name)


current = snapshot()
assert sky.ops.wm.open_mainfile(filepath=path("missing.sky")) == {'CANCELLED'}
with open(path("plain.sky"), "w") as f:
    f.write("not a zip")
assert sky.ops.wm.open_mainfile(filepath=path("plain.sky")) == {'CANCELLED'}
assert sky.ops.wm.open_mainfile(filepath=write_archive("broken.sky", "{")) == {'CANCELLED'}
newer = dict(stored, version=99)
assert sky.ops.wm.open_mainfile(filepath=write_archive("newer.sky", json.dumps(newer))) == {'CANCELLED'}
assert snapshot() == current, "a file that can't be read leaves the document alone"

# Damaged parts are dropped instead of trusted: a curve through a missing point, a shape edge
# past the end of its curve, a reference to a layer that isn't there, and a repeated id.
damaged = {
    "format": "SkyAnimate", "version": 1,
    "document": {"layers": [
        {"id": 5, "name": "A", "type": "VECTOR", "mask_mode": "NOT_A_MODE",
         "drawing": {"points": [{"pos": [0, 0]}, {"pos": [1, 0]}],
                     "curves": [{"points": [{"point": 0}, {"point": 1}, {"point": 7}]}],
                     "shapes": [{"edges": [[0, 0], [0, 5], [3, 0]]}]}},
        {"id": 5, "name": "B", "type": "VECTOR", "reference_source": 99},
    ]},
}
assert sky.ops.wm.open_mainfile(filepath=write_archive("damaged.sky", json.dumps(damaged))) == {'FINISHED'}
a, b = layers["A"], layers["B"]
assert (a.point_count, a.curve_count, a.shape_count) == (2, 1, 1)
assert a.mask_mode == "MASKED", "unknown enum names fall back to the default"
assert a.id != b.id and b.reference_source is None
assert doc.frame_end == 250, "missing values take their defaults"
sky.ops.layer.new()
assert len({l.id for l in layers.all()}) == 3, "new layers get ids no loaded layer uses"

print("Save and open tests passed")
