"""Reference layers with their own changes: permanent ids, looks (per setting, hidden shapes, colour
swaps), Edit Points (offsets, keyed on top of the original's motion), Follow Original's Animation,
Apply to Original and Revert, unused overrides, what a reference can't change, Match Original (posed
by the original's bones, wherever it is) and its own bones with copied weights, saving, and Break
Reference.

Run with:  SkyAnimate --background --python tests/test_references.py
"""

import pathlib
import tempfile

import sky

doc = sky.data.document
layers = sky.data.layers
doc.auto_key = True


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def xy(layer, i):
    return tuple(layer.points[i][:2])


# --- Permanent ids --------------------------------------------------------------------------------
orig = layers.active
orig.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True, fill=True)
orig.add_curve([(2.0, 0.0), (3.0, 0.0)])
sky.ops.mesh.select_none()
ids, shape_ids = orig.point_ids, orig.shape_ids
assert len(set(ids)) == 6 and 0 not in ids and len(set(shape_ids)) == 2 and 0 not in shape_ids
assert sky.ops.layer.make_reference() == {'FINISHED'}
ref = layers.active
assert ref.point_ids == ids and ref.shape_ids == shape_ids, "a reference shows the original's points, by id"
assert ref.reference == {"edit_points": False, "follow_animation": True, "match_original": True}

# --- Looks: each setting of each shape on its own; the rest follows the original -----------------
original_fill = orig.shape_style(0)["fill"]
ref.set_shape_style(0, fill=(1.0, 0.0, 0.0, 1.0))
assert ref.reference_overrides()["looks"] == {shape_ids[0]: {"fill": (1.0, 0.0, 0.0, 1.0)}}
assert ref.shape_style(0)["fill"] == (1.0, 0.0, 0.0, 1.0) and orig.shape_style(0)["fill"] == original_fill
orig.set_shape_style(0, line=(0.0, 0.0, 1.0, 1.0))
assert ref.shape_style(0)["line"] == (0.0, 0.0, 1.0, 1.0), "the line it didn't change follows the original"
assert ref.shape_style(0)["fill"] == (1.0, 0.0, 0.0, 1.0)
# Setting it back to the original's drops the override.
ref.set_shape_style(0, fill=original_fill)
assert ref.reference_overrides()["looks"] == {}
ref.set_shape_style(0, fill=(1.0, 0.0, 0.0, 1.0))

# Hide a shape on the reference only.
assert sky.ops.layer.reference_hide_shape(shape=str(shape_ids[1]), hide="1") == {'FINISHED'}
assert ref.shape_style(1)["hidden"] and not orig.shape_style(1)["hidden"]
assert ref.reference_overrides()["looks"][shape_ids[1]] == {"hidden": True}

# Colour swaps: every shape of the original with that colour, the ones added later too.
black, green = (0.0, 0.0, 0.0, 1.0), (0.0, 1.0, 0.0, 1.0)
assert orig.shape_style(1)["line"] == black
assert sky.ops.layer.reference_swap_color(**{"from": "0,0,0,1", "to": "0,1,0,1", "fill": "0"}) == {'FINISHED'}
assert ref.shape_style(1)["line"] == green and ref.shape_style(0)["line"] == (0.0, 0.0, 1.0, 1.0)
orig.add_curve([(4.0, 0.0), (5.0, 0.0)])
assert ref.shape_count == 3 and ref.shape_style(2)["line"] == green, "a new shape of that colour is swapped too"
assert orig.shape_style(2)["line"] == black

# --- What a reference can't change ------------------------------------------------------------------
try:
    ref.set_point(0, (5.0, 5.0))
    raise AssertionError("points don't move without Edit Points")
except ValueError:
    pass
layers.active = ref
ref.select_points([0])
sky.ops.mesh.delete()
assert ref.point_count == orig.point_count == 8, "points can't be deleted on a reference"

# --- Edit Points: offsets on top of the original ---------------------------------------------------
assert sky.ops.layer.reference_set(edit_points="1") == {'FINISHED'}
assert ref.reference["edit_points"]
sky.ops.mesh.select_none()
doc.frame_current = 0
ref.set_point(1, (1.0, 0.5))
assert ref.reference_overrides()["points"] == {ids[1]: (0.0, 0.5, False)}
assert xy(orig, 1) == (1.0, 0.0) and xy(ref, 1) == (1.0, 0.5)
orig.set_point(1, (1.2, 0.0))
assert close(xy(ref, 1), (1.2, 0.5)), "the original's move comes through, the offset on top"

# Animating: the reference's keys add to the original's motion.
doc.frame_current = 10
orig.set_point(2, (1.0, 2.0))
ref.set_point(2, (1.5, 2.0))
doc.frame_current = 0
assert xy(orig, 2) == (1.0, 1.0) and close(xy(ref, 2), (1.0, 1.0))
doc.frame_current = 10
assert xy(orig, 2) == (1.0, 2.0) and close(xy(ref, 2), (1.5, 2.0))
doc.frame_current = 5
dx = xy(ref, 2)[0] - xy(orig, 2)[0]
assert 0.0 < dx < 0.5 and close((xy(ref, 2)[1],), (xy(orig, 2)[1],)), ("in between, both blend", dx)
assert ref.reference_overrides()["points"][ids[2]][2], "the offset is keyed"

# Follow Original's Animation off: the original's setup shape, with the reference's own keys.
doc.frame_current = 10
assert sky.ops.layer.reference_set(follow_animation="0") == {'FINISHED'}
assert close(xy(ref, 2), (1.5, 1.0))
assert sky.ops.layer.reference_set(follow_animation="1") == {'FINISHED'}
assert close(xy(ref, 2), (1.5, 2.0))

# --- Apply to Original, Revert ------------------------------------------------------------------------
doc.frame_current = 0
assert sky.ops.layer.reference_apply(kind="LOOK", id=str(shape_ids[0])) == {'FINISHED'}
assert orig.shape_style(0)["fill"] == (1.0, 0.0, 0.0, 1.0) and shape_ids[0] not in ref.reference_overrides()["looks"]
assert sky.ops.layer.reference_apply(kind="POINT", id=str(ids[1])) == {'FINISHED'}
assert close(xy(orig, 1), (1.2, 0.5)) and close(xy(ref, 1), (1.2, 0.5))
assert ids[1] not in ref.reference_overrides()["points"]
# A keyed offset goes into the original's keys.
assert sky.ops.layer.reference_apply(kind="POINTS") == {'FINISHED'}
doc.frame_current = 10
assert close(xy(orig, 2), (1.5, 2.0)) and close(xy(ref, 2), (1.5, 2.0))
doc.frame_current = 0
assert close(xy(orig, 2), (1.0, 1.0))
# Hiding stays the reference's own.
assert sky.ops.layer.reference_apply(kind="ALL") == {'FINISHED'}
assert ref.shape_style(1)["hidden"] and not orig.shape_style(1)["hidden"]
assert orig.shape_style(1)["line"] == green, "the swap recoloured the original"
assert ref.reference_overrides()["swaps"] == []
assert sky.ops.layer.reference_revert(kind="LOOK", id=str(shape_ids[1])) == {'FINISHED'}
assert not ref.shape_style(1)["hidden"] and ref.reference_overrides()["looks"] == {}

# --- Unused overrides ---------------------------------------------------------------------------------
ref.set_point(5, (3.0, 1.0))
assert ids[5] in ref.reference_overrides()["points"]
layers.active = orig
sky.ops.mesh.select_none()
orig.select_points([4, 5])
sky.ops.mesh.delete()
layers.active = ref
assert ref.point_count == orig.point_count
assert sky.ops.layer.reference_remove_unused() == {'FINISHED'}
assert ref.reference_overrides()["points"] == {}

# --- Saved -------------------------------------------------------------------------------------------
ref.set_point(0, (-0.5, 0.0))
ref.set_shape_style(0, line_width=0.05)
saved = (ref.reference, ref.reference_overrides(), ref.points, orig.point_ids, orig.shape_ids)
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_refs_")) / "refs.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
orig = next(l for l in sky.data.layers if l.reference_source is None and l.type == "VECTOR")
ref = next(l for l in sky.data.layers if l.reference_source is not None)
assert (ref.reference, ref.reference_overrides(), ref.points, orig.point_ids, orig.shape_ids) == saved

# --- Break Reference keeps what it showed, offsets in its keys ---------------------------------------
layers.active = ref
doc.frame_current = 10
shown_10 = ref.points
doc.frame_current = 0
shown_0 = ref.points
assert sky.ops.layer.break_reference() == {'FINISHED'}
assert ref.reference_source is None and ref.points == shown_0
doc.frame_current = 10
assert ref.points == shown_10, "its animation stays as it was"
doc.frame_current = 0

# --- Match Original: posed by the original's bones, wherever it is ------------------------------------
sky.ops.layer.new()
art = layers.active
art.add_curve([(0.2, 0.0), (0.8, 0.0)])
assert sky.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
layers.active = art
assert sky.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
layers.active = rig
assert sky.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0") == {'FINISHED'}
layers.active = art
assert sky.ops.layer.make_reference() == {'FINISHED'}
twin = layers.active
assert twin.parent == rig
# Out of the bone layer, it still moves with the original's bones.
assert sky.ops.layer.move_to_group(group="0") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("arm", angle=1.5707963)
posed = art.evaluated()[0]
assert abs(posed[1][1] - 0.8) < 1e-3, ("the bone turned the original's point up", posed)
assert all(close(a, b) for a, b in zip(twin.evaluated()[0], posed)), (twin.evaluated()[0], posed)
# Its own transform moves it from there; the shape stays the original's.
twin.location_x = 2.0
assert all(close(a, b) for a, b in zip(twin.evaluated()[0], posed))
twin.location_x = 0.0

# Match Original off, in a bone layer of its own: its own weights, copied by bone name.
doc.frame_current = 0
assert sky.ops.layer.new_bone() == {'FINISHED'}
rig2 = layers.active
assert sky.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0") == {'FINISHED'}
layers.active = twin
assert sky.ops.layer.move_to_group(group=str(rig2.id)) == {'FINISHED'}
assert sky.ops.layer.reference_set(match_original="0") == {'FINISHED'}
assert not twin.reference["match_original"]
assert twin.weights == art.weights and twin.weights[1] != {}, ("weights copied by bone name", twin.weights)
assert set(twin.reference_overrides()["weights"]) == set(art.point_ids)
# It follows its own bone layer now: rig2's bone isn't posed, so it stays put.
doc.frame_current = 10
assert close(twin.evaluated()[0][1], (0.8, 0.0)), twin.evaluated()[0]
rig2.bone_set("arm", angle=1.5707963)
assert abs(twin.evaluated()[0][1][1] - 0.8) < 1e-3, "and moves with its own bones"
doc.frame_current = 0

# --- Layer channels follow the original until taken over --------------------------------------------
sky.ops.layer.new()
src = layers.active
src.add_curve([(0.0, 0.0), (1.0, 0.0)])
assert sky.ops.layer.make_reference() == {'FINISHED'}
copy = layers.active
doc.frame_current = 10
src.location_x = 3.0
doc.frame_current = 5
assert 0.0 < src.location_x < 3.0 and abs(copy.location_x - src.location_x) < 1e-6, "it follows the original's motion"
assert copy.reference_overrides()["channels"] == []
copy.rotation = 30.0
doc.frame_current = 0
assert copy.reference_overrides()["channels"] == ["rotation"], "edited here: its own from now on"
assert copy.rotation == 0.0 and src.rotation == 0.0
doc.frame_current = 5
assert copy.rotation == 30.0 and src.rotation == 0.0, "keyed on the reference"
layers.active = copy
assert any(r[1].endswith("Rotation") and r[4] for r in sky.wm.timeline_rows(changed=True)), "teal in the timeline"
assert sky.ops.layer.reference_sync(path="rotation") == {'FINISHED'}
assert copy.reference_overrides()["channels"] == [] and copy.rotation == 0.0, "synced: the original's again"
# Apply to Original writes a channel it took over into the original.
copy.rotation = 15.0
doc.frame_current = 0
assert sky.ops.layer.reference_apply(kind="CHANNELS") == {'FINISHED'}
doc.frame_current = 5
assert src.rotation == 15.0 and copy.rotation == 15.0 and copy.reference_overrides()["channels"] == []

# --- Turned handles are kept as offsets too ----------------------------------------------------------
doc.frame_current = 0
assert sky.ops.layer.reference_set(edit_points="1") == {'FINISHED'}
built = copy.curve_handles(0)[0][2]
copy.move_handle(0, 0, "OUT", (0.5, 0.5))
pids = copy.point_ids
handles = copy.reference_overrides()["handles"]
assert list(handles) == [(pids[0], pids[1], "OUT")], handles
offset = (0.5 - built[0], 0.5 - built[1])
assert close(handles[(pids[0], pids[1], "OUT")][:2], offset)
assert close(src.curve_handles(0)[0][2], built), "the original's handle stays"
# The original's handle change comes through, the offset on top.
src.move_handle(0, 0, "OUT", (0.4, 0.0))
assert close(copy.curve_handles(0)[0][2], (0.4 + offset[0], 0.0 + offset[1]))
# Saved with it.
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_refs2_")) / "refs2.sky")
saved = copy.reference_overrides()
layers.active = copy
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
copy = layers.active  # the active layer is saved too
assert copy.reference_source is not None
src = copy.reference_source
loaded = copy.reference_overrides()
assert loaded.keys() == saved.keys() and loaded["handles"].keys() == saved["handles"].keys()
assert all(close(loaded["handles"][k][:2], v[:2]) for k, v in saved["handles"].items()), (loaded, saved)
# Applied, the original's handle gets it.
layers.active = copy
assert sky.ops.layer.reference_apply(kind="POINTS") == {'FINISHED'}
assert copy.reference_overrides()["handles"] == {}
assert close(src.curve_handles(0)[0][2], (0.4 + offset[0], offset[1]))
assert close(copy.curve_handles(0)[0][2], (0.4 + offset[0], offset[1]))

# --- Reset or push one setting, or the picked points ---------------------------------------------------
copy.set_shape_style(0, fill=(1.0, 0.0, 0.0, 1.0), line_width=0.03)
sid = copy.shape_ids[0]
assert set(copy.reference_overrides()["looks"][sid]) == {"fill", "line_width"}
assert sky.ops.layer.reference_revert(kind="LOOK", id=str(sid), setting="fill") == {'FINISHED'}
assert set(copy.reference_overrides()["looks"][sid]) == {"line_width"}, "only that setting went back"
assert sky.ops.layer.reference_apply(kind="LOOK", id=str(sid), setting="line_width") == {'FINISHED'}
assert abs(src.shape_style(0)["line_width"] - 0.03) < 1e-6 and copy.reference_overrides()["looks"] == {}
copy.set_point(0, (0.0, 0.3))
copy.set_point(1, (1.0, 0.3))
sky.ops.mesh.select_none()
copy.select_points([0])
assert sky.ops.layer.reference_revert(kind="SELECTED") == {'FINISHED'}
assert list(copy.reference_overrides()["points"]) == [copy.point_ids[1]], "only the picked point went back"

# --- Recolor: every style change at once; hiding stays ---------------------------------------------------
copy.set_shape_style(0, fill=(0.0, 0.0, 1.0, 1.0), line_width=0.04)
assert sky.ops.layer.reference_hide_shape(shape=str(sid), hide="1") == {'FINISHED'}
assert sky.ops.layer.reference_revert(kind="STYLES") == {'FINISHED'}
assert copy.reference_overrides()["looks"] == {sid: {"hidden": True}}, "styles went back, hiding stays"
assert copy.shape_style(0)["fill"] == src.shape_style(0)["fill"]
copy.set_shape_style(0, fill=(0.0, 0.0, 1.0, 1.0))
assert sky.ops.layer.reference_apply(kind="STYLES") == {'FINISHED'}
assert src.shape_style(0)["fill"] == (0.0, 0.0, 1.0, 1.0) and copy.reference_overrides()["looks"] == {sid: {"hidden": True}}

# --- Deleting bones isn't an edit to a copy --------------------------------------------------------------
# Deleting bones takes their weights off every point, a Same as Original copy's too: no warning, and
# the copy follows its original, which lost them as well.
doc.frame_current = 0
sky.ops.layer.new()
art3 = layers.active
art3.add_curve([(0.2, 3.0), (0.8, 3.0)])
assert sky.ops.layer.new_bone() == {'FINISHED'}
rig3 = layers.active
layers.active = art3
assert sky.ops.layer.move_to_group(group=str(rig3.id)) == {'FINISHED'}
layers.active = rig3
assert sky.ops.armature.bone_add(name="arm", head_x="0", head_y="3", tail_x="1", tail_y="3") == {'FINISHED'}
layers.active = art3
assert sky.ops.layer.make_reference() == {'FINISHED'}
twin3 = layers.active
assert twin3.reference["match_original"] and any(w for w in twin3.weights), twin3.weights
shown = sky.wm.mouse_report()[0]
layers.active = rig3
sky.ops.armature.select_all()
assert sky.ops.armature.bone_delete() == {'FINISHED'}
doc.frame_current = doc.frame_current  # a sync, as the app does every frame
assert sky.wm.mouse_report()[0] == shown, ("no warning", sky.wm.mouse_report())
assert not any(w for w in art3.weights) and not any(w for w in twin3.weights), (art3.weights, twin3.weights)
assert not twin3.reference_overrides()["weights"], "no weight changes of its own"

# --- Parent Bone waits for Own Bones ----------------------------------------------------------------------
# A copy posed Same as Original moves with its original's bones: it takes no parent bone of its own.
doc.frame_current = 0
assert sky.ops.layer.new_bone() == {'FINISHED'}
rig4 = layers.active
assert sky.ops.armature.bone_add(name="hand", head_x="0", head_y="5", tail_x="1", tail_y="5") == {'FINISHED'}
sky.ops.layer.new()
art4 = layers.active
art4.add_curve([(0.2, 5.0), (0.8, 5.0)])
assert sky.ops.layer.move_to_group(group=str(rig4.id)) == {'FINISHED'}
assert sky.ops.layer.make_reference() == {'FINISHED'}
twin4 = layers.active
assert twin4.reference["match_original"]
assert sky.ops.layer.parent_bone_set(bone="hand") == {'CANCELLED'}, "Same as Original: no parent bone"
assert twin4.parent_bone == ""
# Own Bones: now it can follow a bone of its own.
assert sky.ops.layer.reference_set(match_original="0") == {'FINISHED'}
assert sky.ops.layer.parent_bone_set(bone="hand") == {'FINISHED'} and twin4.parent_bone == "hand"
# Back to Same as Original: the setting stays, but the copy doesn't follow it (no double move).
assert sky.ops.layer.reference_set(match_original="1") == {'FINISHED'}
rest = twin4.to_document((0.5, 5.0))
doc.frame_current = 10
rig4.bone_set("hand", position=(0.0, 7.0))
doc.frame_current = 10
assert close(twin4.to_document((0.5, 5.0)), rest), ("its own parent bone waits", twin4.to_document((0.5, 5.0)))
doc.frame_current = 0

# --- A reference of a reference links to the original ---------------------------------------------
import json
import zipfile

sky.ops.layer.new()
base = layers.active
base.add_curve([(0.0, 9.0), (1.0, 9.0), (1.0, 10.0)], closed=True, fill=True)
sky.ops.mesh.select_none()
assert sky.ops.layer.make_reference() == {'FINISHED'}
first = layers.active
assert sky.ops.layer.make_reference() == {'FINISHED'}
second = layers.active
assert second.reference_source == base, "a reference of a reference follows the original"
# Alt D in the Layers editor does the same.
assert sky.wm.event_simulate("D", alt=True, area="Layers")
assert layers.active.reference_source == base and layers.active != second
sky.ops.layer.delete()
layers.active = second
assert second.points == base.points
# Inside a referenced group too: a reference of the reference group links its layers to the originals.
assert sky.ops.layer.new_group() == {'FINISHED'}
grp = layers.active
sky.ops.layer.new()
inner = layers.active
inner.add_curve([(0.0, 12.0), (1.0, 12.0)])
assert sky.ops.layer.move_to_group(group=str(grp.id)) == {'FINISHED'}
layers.active = grp
assert sky.ops.layer.make_reference() == {'FINISHED'}
grp_ref = layers.active
assert sky.ops.layer.make_reference() == {'FINISHED'}
grp_ref2 = layers.active
assert grp_ref2.reference_source == grp
assert [c.reference_source for c in grp_ref2.children] == [inner]

# Files with a chain (made before this) open with it linked to the original.
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_refchain_")) / "chain.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
with zipfile.ZipFile(path) as z:
    entries = {n: z.read(n) for n in z.namelist()}
root = json.loads(entries["document.json"])


def find(nodes, layer_id):
    for n in nodes:
        if n.get("id") == layer_id:
            return n
        found = find(n.get("children", []), layer_id)
        if found:
            return found
    return None


find(root["document"]["layers"], second.id)["reference_source"] = first.id
entries["document.json"] = json.dumps(root).encode("utf-8")
with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
    for n, data in entries.items():
        z.writestr(n, data)
second_id, base_id = second.id, base.id
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
second = next(l for l in sky.data.layers.all() if l.id == second_id)
assert second.reference_source is not None and second.reference_source.id == base_id, "the chain follows to the original"

print("Reference tests passed")
