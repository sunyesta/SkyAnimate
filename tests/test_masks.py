"""Masks that name layers (After Effects' track mattes, Blender Grease Pencil's layer masks): each
layer's masks show it only inside, or only outside, any of the layers each mask names, where every
mask that's on holds. Adding, changing, turning off and removing them; Mask With Layer Below
(Ctrl Alt G); copies keeping their pairs; references of groups masking themselves; saving; and
files from before, whose Moho masking groups become masks.

Run with:  OpenAnimate --background --python tests/test_masks.py
"""

import json
import pathlib
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_masks_"))


def find(name):
    return next(l for l in layers.all() if l.name == name)


OLD_KEYS = ("outside", "layers", "enabled", "render", "expand", "fills_only", "filled", "extend")


def rows(layer):
    """Its mask rows as the keys from before rows (outside: a Subtract row; expand: grown)."""
    return [{k: m[k] for k in OLD_KEYS} for m in layer.masks]


def square(name, x0=0.0, x1=1.0):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(x0, 0.0), (x1, 0.0), (x1, 1.0), (x0, 1.0)], closed=True, fill=True)
    return l


head = layers.active
head.name = "head"
head.add_curve([(0.0, 0.0), (2.0, 0.0), (2.0, 2.0), (0.0, 2.0)], closed=True, fill=True)
hole = square("hole", 0.5, 1.5)
teeth = square("teeth", 0.4, 1.6)
tongue = square("tongue", 0.6, 1.4)

# --- Adding, reading, changing, removing --------------------------------------------------------------
layers.active = head
assert oanim.ops.mask.add(sources="hole", outside="1") == {'FINISHED'}
assert rows(head) == [{"outside": True, "layers": ["hole"], "enabled": True, "render": True, "expand": False, "fills_only": False, "filled": False, "extend": "NONE"}]
assert hole.masking == ["head"]
assert oanim.wm.undo_history()[0][-1] == "Mask head: Subtract hole", "the undo step says what happened"
assert oanim.ops.mask.add(sources="hole", outside="1") == {'CANCELLED'}, "the same mask twice is one mask"
assert oanim.ops.mask.add(sources="head") == {'CANCELLED'}, "a layer can't be its own mask"
assert oanim.ops.mask.add(sources="nothing here") == {'CANCELLED'}

# One mask can name several layers: inside any of them.
layers.active = tongue
assert oanim.ops.mask.add(sources="head|teeth") == {'FINISHED'}
assert tongue.masks[0]["layers"] == ["head", "teeth"]
assert sorted(head.masking) == ["tongue"] and teeth.masking == ["tongue"]

# Turn off and on, Inside/Outside, change its layers.
assert oanim.ops.mask.set(index="0", toggle="1") == {'FINISHED'}
assert tongue.masks[0]["enabled"] is False
assert oanim.ops.mask.set(index="0", enabled="1", outside="1", sources="teeth") == {'FINISHED'}
assert rows(tongue) == [{"outside": True, "layers": ["teeth"], "enabled": True, "render": True, "expand": False, "fills_only": False, "filled": False, "extend": "NONE"}]
oanim.ops.ed.undo()
assert rows(tongue)[0] == {"outside": False, "layers": ["head", "teeth"], "enabled": False, "render": True, "expand": False, "fills_only": False, "filled": False, "extend": "NONE"}
assert oanim.ops.mask.remove(index="0") == {'FINISHED'} and tongue.masks == []

# The active and picked layers together; remove all.
layers.active = teeth
tongue.select = True
assert oanim.ops.mask.add(sources="head") == {'FINISHED'}
assert teeth.masks[0]["layers"] == ["head"] and tongue.masks[0]["layers"] == ["head"]
assert oanim.ops.mask.remove() == {'FINISHED'} and teeth.masks == [] and tongue.masks == []
tongue.select = False

# --- Clip to Below (Ctrl Alt G) ----------------------------------------------------------------------
layers.active = tongue
assert oanim.wm.event_simulate("G", ctrl=True, alt=True, area="Viewport")
assert tongue.clip_to_below and tongue.masks == [] and teeth.masking == ["tongue"]
oanim.ops.mask.clip_to_below()
assert not tongue.clip_to_below

# Picking the mask layer by clicking is asked of the editors.
assert oanim.ops.mask.pick(mode="OUTSIDE") == {'FINISHED'}

# --- A layer and its mask copied together stay a pair -----------------------------------------------
layers.active = teeth
oanim.ops.mask.add(sources="hole")
hole.select = True
assert oanim.ops.layer.duplicate() == {'FINISHED'}
copy = layers.active
assert copy.name != "teeth" and copy.masks[0]["layers"][0] != "hole", copy.masks
copied_hole = copy.masks[0]["layers"][0]
assert find(copied_hole).id != hole.id
assert teeth.masks[0]["layers"] == ["hole"], "the original keeps its own"
# Copied alone, a layer keeps naming the same mask layer.
for l in layers.all():
    l.select = False
layers.active = teeth
oanim.ops.layer.duplicate()
assert layers.active.masks[0]["layers"] == ["hole"]

# --- A reference of a group masks itself with its own copies ----------------------------------------
oanim.ops.layer.new_group()
face = layers.active
face.name = "Face"
skin, freckles = square("skin", 0.0, 3.0), square("freckles", 1.0, 2.0)
for l in (skin, freckles):
    layers.active = l
    oanim.ops.layer.move_to_group(group=str(face.id))
layers.active = freckles
oanim.ops.mask.add(sources="skin")
layers.active = face
assert oanim.ops.layer.make_reference() == {'FINISHED'}
ref_face = layers.active
ref_freckles = next(c for c in ref_face.children if c.reference_source == freckles)
ref_skin = next(c for c in ref_face.children if c.reference_source == skin)
assert oanim.ops.mask.add.poll()
assert [l.name for l in ref_face.children] == [l.name for l in face.children]
assert ref_freckles.masks[0]["layers"] == ["skin"]
assert ref_skin.masking == ["freckles"] and skin.masking == ["freckles"], "each names its own skin"

# --- Saved and opened again --------------------------------------------------------------------------
layers.active = head
oanim.ops.mask.set(index="0", enabled="0")
path = str(folder / "masks.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layers = oanim.data.layers
assert rows(find("head")) == [{"outside": True, "layers": ["hole"], "enabled": False, "render": True, "expand": False, "fills_only": False, "filled": False, "extend": "NONE"}]
assert find("teeth").masks[0]["layers"] == ["hole"]

# --- An older file's Moho masking groups become masks --------------------------------------------------
# The mouth rig as it was: in a Hide-all group, bottom up: head (clear, add, invisible), the inside of
# the mouth (masked, one tooth not), the hole (subtract, invisible), the head with its hole (masked),
# the mouth line's shape (clear, add, invisible), the mouth line (masked).
def vec(i, name, mode="MASKED"):
    return {"id": i, "name": name, "type": "VECTOR", "mask_mode": mode,
            "drawing": {"points": [{"pos": [0, 0]}, {"pos": [1, 0]}, {"pos": [1, 1]}],
                        "curves": [{"points": [{"point": 0}, {"point": 1}, {"point": 2}], "closed": True}]}}


old = {
    "format": "SkyAnimate", "version": 1,
    "document": {"layers": [{
        "id": 1, "name": "Mouth", "type": "GROUP", "group_mask": "HIDE_ALL", "children": [
            vec(2, "line", "MASKED"),
            vec(3, "line_shape", "CLEAR_ADD_INVISIBLE"),
            vec(4, "head_art", "MASKED"),
            vec(5, "hole_cut", "SUBTRACT_INVISIBLE"),
            {"id": 6, "name": "inside", "type": "GROUP", "mask_mode": "MASKED", "children": [
                vec(7, "teeth_top"), vec(8, "teeth_bottom", "NOT_MASKED")]},
            vec(9, "head_shape", "CLEAR_ADD_INVISIBLE"),
        ]}]},
}
old_path = folder / "old.oanim"
with zipfile.ZipFile(old_path, "w") as archive:
    archive.writestr("document.json", json.dumps(old))
assert oanim.ops.wm.open_mainfile(filepath=str(old_path), confirmed="1") == {'FINISHED'}
layers = oanim.data.layers
assert rows(find("inside")) == [{"outside": False, "layers": ["head_shape"], "enabled": True, "render": True, "expand": False, "fills_only": False, "filled": False, "extend": "NONE"}]
assert rows(find("head_art")) == [{"outside": False, "layers": ["head_shape"], "enabled": True, "render": True, "expand": False, "fills_only": False, "filled": False, "extend": "NONE"},
                                  {"outside": True, "layers": ["hole_cut"], "enabled": True, "render": True, "expand": False, "fills_only": False, "filled": False, "extend": "NONE"}]
assert rows(find("line")) == [{"outside": False, "layers": ["line_shape"], "enabled": True, "render": True, "expand": False, "fills_only": False, "filled": False, "extend": "NONE"}]
assert find("teeth_bottom").masks == [] and find("teeth_top").masks == [], "the group carries the mask"
assert not find("head_shape").visible and not find("hole_cut").visible and not find("line_shape").visible, \
    "masking-only layers become hidden layers, which still mask"
assert find("head_art").visible and find("Mouth").masks == []

# The two options were the mask layer's before; the masks naming it take them.
old["document"]["layers"][0]["children"][5]["mask_expand"] = True          # head_shape
old["document"]["layers"][0]["children"][3]["mask_exclude_strokes"] = True  # hole_cut
with zipfile.ZipFile(old_path, "w") as archive:
    archive.writestr("document.json", json.dumps(old))
assert oanim.ops.wm.open_mainfile(filepath=str(old_path), confirmed="1") == {'FINISHED'}
layers = oanim.data.layers
head_art = find("head_art").masks
assert (head_art[0]["expand"], head_art[0]["fills_only"]) == (True, False), head_art
assert (head_art[1]["expand"], head_art[1]["fills_only"]) == (False, True), head_art
assert find("inside").masks[0]["expand"] and not find("line").masks[0]["expand"]

# --- Each mask's own options, viewport and render switches, and order -------------------------------
art = find("head_art")
layers.active = art
assert oanim.ops.mask.set(index="1", expand="1", fills_only="0", render="0") == {'FINISHED'}
m = art.masks[1]
assert (m["expand"], m["fills_only"], m["render"], m["enabled"]) == (True, False, False, True)
assert oanim.ops.mask.move(index="1", to="0") == {'FINISHED'}
assert art.masks[0]["layers"] == ["hole_cut"] and art.masks[1]["layers"] == ["head_shape"]
assert oanim.ops.mask.add(sources="line_shape", expand="1", fills_only="1") == {'FINISHED'}
assert (art.masks[2]["expand"], art.masks[2]["fills_only"]) == (True, True)
path = str(folder / "options.oanim")
oanim.ops.wm.save_as_mainfile(filepath=path)
oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
layers = oanim.data.layers
assert rows(find("head_art"))[0] == {"outside": True, "layers": ["hole_cut"], "enabled": True, "render": False,
                                     "expand": True, "fills_only": False, "filled": False, "extend": "NONE"}

# Mask With: As Drawn, Fills Only, Filled Shape (an outline-only shape masks with its inside too),
# Filled Shape, No Stroke.
art = find("head_art")
layers.active = art
for use, filled, fills in [("FILLED", True, False), ("FILLED_NO_STROKE", True, True), ("FILLS_ONLY", False, True),
                           ("AS_DRAWN", False, False)]:
    assert oanim.ops.mask.set(index="0", use=use) == {'FINISHED'}
    assert (art.masks[0]["filled"], art.masks[0]["fills_only"]) == (filled, fills), use
assert oanim.ops.mask.set(index="0", use="SIDEWAYS") == {'CANCELLED'}
# Shape (As Drawn, Fills Only, Strokes Only) and Fill Every Shape are set apart: one leaves the other.
assert oanim.ops.mask.set(index="0", filled="1") == {'FINISHED'}
for shape, fills, strokes in [("STROKES_ONLY", False, True), ("FILLS_ONLY", True, False), ("AS_DRAWN", False, False)]:
    assert oanim.ops.mask.set(index="0", shape=shape) == {'FINISHED'}
    m = art.masks[0]
    assert (m["fills_only"], m["strokes_only"], m["shape"], m["filled"]) == (fills, strokes, shape, True), (shape, m)
assert oanim.ops.mask.set(index="0", shape="SIDEWAYS") == {'CANCELLED'}
assert oanim.ops.mask.set(index="0", strokes_only="1") == {'FINISHED'} and art.masks[0]["shape"] == "STROKES_ONLY"
assert oanim.ops.mask.set(index="0", fills_only="1") == {'FINISHED'}
assert (art.masks[0]["fills_only"], art.masks[0]["strokes_only"]) == (True, False), "never both"
assert oanim.ops.mask.set(index="0", use="STROKES_ONLY") == {'FINISHED'}
assert (art.masks[0]["shape"], art.masks[0]["filled"]) == ("STROKES_ONLY", False), "use= sets both, as before"
assert oanim.ops.mask.set(index="0", use="AS_DRAWN") == {'FINISHED'}
assert oanim.ops.mask.add(sources="line", use="FILLED") == {'FINISHED'} and art.masks[-1]["filled"]
path = str(folder / "filled.oanim")
oanim.ops.wm.save_as_mainfile(filepath=path)
oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
layers = oanim.data.layers
assert find("head_art").masks[-1]["filled"] and not find("head_art").masks[0]["filled"]
art = find("head_art")
layers.active = art
assert oanim.ops.mask.set(index="0", shape="STROKES_ONLY") == {'FINISHED'}
assert oanim.ops.mask.add(sources="line", shape="FILLS_ONLY", inside_stroke="1") == {'FINISHED'}
assert art.masks[-1]["inside_stroke"]
path = str(folder / "strokes.oanim")
oanim.ops.wm.save_as_mainfile(filepath=path)
oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
layers = oanim.data.layers
assert find("head_art").masks[0]["shape"] == "STROKES_ONLY", "Strokes Only is saved"
assert find("head_art").masks[-1]["inside_stroke"], "Inside the Stroke is saved"
art = find("head_art")
layers.active = art
assert oanim.ops.mask.set(index="0", shape="AS_DRAWN") == {'FINISHED'}

# Extend: the shapes stretched to the edge one way ("Outside the mouth line and above").
art = find("head_art")
layers.active = art
assert oanim.ops.mask.set(index="0", extend="UP") == {'FINISHED'} and art.masks[0]["extend"] == "UP"
assert oanim.wm.undo_history()[0][-1].endswith(" and above"), oanim.wm.undo_history()[0][-1]
assert oanim.ops.mask.set(index="0", extend="SIDEWAYS") == {'CANCELLED'}
assert oanim.ops.mask.add(sources="line_shape", outside="1", extend="DOWN") == {'FINISHED'}
assert art.masks[-1]["extend"] == "DOWN"
path = str(folder / "extend.oanim")
oanim.ops.wm.save_as_mainfile(filepath=path)
oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
layers = oanim.data.layers
assert find("head_art").masks[0]["extend"] == "UP" and find("head_art").masks[-1]["extend"] == "DOWN"

# --- Rows: read top to bottom, each combining its shape with the rows above ----------------------------
assert oanim.ops.wm.read_homefile() == {'FINISHED'}
layers = oanim.data.layers
base = layers.active
base.name = "base"
base.add_curve([(0.0, 0.0), (3.0, 0.0), (3.0, 3.0), (0.0, 3.0)], closed=True, fill=True)
head = square("head", 0.0, 2.0)
mouth = square("mouth", 0.5, 1.5)
tongue = square("tongue", 0.8, 1.2)
jaw = square("jaw", 0.0, 1.0)
face = square("face", 0.0, 3.0)
layers.active = face
assert oanim.ops.mask.add(sources="head") == {'FINISHED'} and face.masks[0]["mode"] == "ADD", "a first row adds"
assert oanim.ops.mask.add(sources="mouth", mode="SUBTRACT") == {'FINISHED'}
assert oanim.ops.mask.add(sources="tongue", mode="ADD") == {'FINISHED'}
assert [m["mode"] for m in face.masks] == ["ADD", "SUBTRACT", "ADD"]
assert face.mask_summary == "Shows: inside head, minus mouth, plus tongue", face.mask_summary
assert oanim.ops.mask.add(sources="jaw") == {'FINISHED'} and face.masks[3]["mode"] == "INTERSECT", \
    "without a mode, a later row must hold too (as masks did before rows)"
assert face.mask_summary.endswith(", within jaw"), face.mask_summary
assert oanim.ops.mask.add(sources="jaw", mode="SIDEWAYS") == {'CANCELLED'}

# Several layers in one row make one shape.
assert oanim.ops.mask.set(index="0", sources="head|jaw") == {'FINISHED'}
assert face.masks[0]["layers"] == ["head", "jaw"] and face.mask_summary.startswith("Shows: inside head + jaw")

# Invert, Grow/Shrink, Difference; outside= and expand= as before rows.
assert oanim.ops.mask.set(index="3", mode="DIFFERENCE", invert="1", grow="-2.5") == {'FINISHED'}
m = face.masks[3]
assert (m["mode"], m["invert"], m["grow"], m["expand"]) == ("DIFFERENCE", True, -2.5, False), m
assert face.mask_summary.endswith(", flipped by outside jaw"), face.mask_summary
assert oanim.ops.mask.set(index="3", grow="100") == {'FINISHED'} and face.masks[3]["grow"] == 64.0
assert oanim.ops.mask.set(index="3", outside="1") == {'FINISHED'} and face.masks[3]["mode"] == "SUBTRACT"
assert oanim.ops.mask.set(index="3", outside="0", expand="1") == {'FINISHED'}
assert (face.masks[3]["mode"], face.masks[3]["grow"]) == ("INTERSECT", 1.0)
assert oanim.ops.mask.set(index="0", outside="0") == {'FINISHED'} and face.masks[0]["mode"] == "ADD"
assert oanim.ops.mask.set(index="3", invert="0", grow="-3") == {'FINISHED'}

# A first Subtract starts from everything.
oanim.ops.layer.new()
cut = layers.active
cut.name = "cut"
assert oanim.ops.mask.add(sources="mouth", mode="SUBTRACT") == {'FINISHED'}
assert cut.mask_summary == "Shows: everything but mouth", cut.mask_summary
assert cut.masks[0]["outside"]

# Reordering changes what shows: rows apply in order.
layers.active = face
assert oanim.ops.mask.move(index="2", to="0") == {'FINISHED'}
assert [m["layers"][0] for m in face.masks] == ["tongue", "head", "mouth", "jaw"]
assert oanim.wm.undo_history()[0][-1] == "Move Mask Row"
assert face.mask_summary == "Shows: inside tongue, plus head + jaw, minus mouth, within jaw", face.mask_summary
oanim.ops.ed.undo()
assert [m["layers"][0] for m in face.masks] == ["head", "mouth", "tongue", "jaw"]

# --- Clip to Below (Photoshop's clipping mask) ----------------------------------------------------------
# Several layers in a row clip to the first layer below them that doesn't clip.
layers.active = mouth
assert oanim.ops.mask.clip_to_below() == {'FINISHED'} and mouth.clip_to_below
layers.active = tongue
assert oanim.wm.event_simulate("G", ctrl=True, alt=True, area="Viewport")
assert tongue.clip_to_below, "Ctrl Alt G"
assert sorted(head.masking) == ["face", "mouth", "tongue"], head.masking
assert tongue.mask_summary == "Shows: inside head", tongue.mask_summary
assert oanim.wm.event_simulate("G", ctrl=True, alt=True, area="Viewport")
assert not tongue.clip_to_below, "Ctrl Alt G again releases it"
assert oanim.ops.mask.clip_to_below(set="1") == {'FINISHED'}
assert oanim.ops.mask.clip_to_below(set="1") == {'CANCELLED'}, "already on"
layers.active = base
assert oanim.ops.mask.clip_to_below() == {'CANCELLED'}, "nothing below the bottom layer"
assert not base.clip_to_below
# Exclude Strokes: clipped to the base's fills only; setting it alone leaves the clip as it is.
layers.active = mouth
assert oanim.ops.mask.clip_to_below(exclude_strokes="1") == {'FINISHED'}
assert mouth.clip_exclude_strokes and mouth.clip_to_below
assert oanim.wm.undo_history()[0][-1] == "Clip Excluding Strokes"
assert oanim.ops.mask.clip_to_below(exclude_strokes="1") == {'CANCELLED'}, "already so"
oanim.ops.ed.undo()
assert not mouth.clip_exclude_strokes and mouth.clip_to_below
assert oanim.ops.mask.clip_to_below(layer="mouth", exclude_strokes="1") == {'FINISHED'}

# --- Saved: modes, Invert, Grow and Clip to Below; files from before rows convert exactly --------------
path = str(folder / "rows.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layers = oanim.data.layers
face = find("face")
assert [(m["mode"], m["invert"], m["grow"]) for m in face.masks] == \
    [("ADD", False, 0.0), ("SUBTRACT", False, 0.0), ("ADD", False, 0.0), ("INTERSECT", False, -3.0)], face.masks
assert find("mouth").clip_to_below and find("tongue").clip_to_below and not find("head").clip_to_below
assert find("mouth").clip_exclude_strokes and not find("tongue").clip_exclude_strokes, ("Exclude Strokes is saved", find("mouth").clip_exclude_strokes, find("tongue").clip_exclude_strokes)

# Before rows a file said Inside or Outside, and every mask had to hold: an Inside first row becomes
# Add, a later Inside Intersect, every Outside Subtract (a first Subtract starts from everything);
# Expand grows a pixel.
with zipfile.ZipFile(path) as archive:
    stored = json.loads(archive.read("document.json"))


def strip(layer_list):
    for l in layer_list:
        if l.get("name") == "face":
            for m, outside in zip(l["masks"], (True, False, False, True)):
                del m["mode"], m["invert"], m["grow"]
                m["outside"] = outside
            l["masks"][1]["expand"] = True
        strip(l.get("children", []))


strip(stored["document"]["layers"])
with zipfile.ZipFile(old_path, "w") as archive:
    archive.writestr("document.json", json.dumps(stored))
assert oanim.ops.wm.open_mainfile(filepath=str(old_path), confirmed="1") == {'FINISHED'}
layers = oanim.data.layers
assert [(m["mode"], m["grow"]) for m in find("face").masks] == \
    [("SUBTRACT", 0.0), ("INTERSECT", 1.0), ("INTERSECT", 0.0), ("SUBTRACT", 0.0)], find("face").masks

# A file from before mask rows lent shapes as drawn opens (with a note naming layers whose masks
# named a layer with masks of its own); saving marks it.
with zipfile.ZipFile(path) as archive:
    stored = json.loads(archive.read("document.json"))
assert stored["document"].get("mask_sources") == "shapes"
del stored["document"]["mask_sources"]
older = folder / "older_masks.oanim"
with zipfile.ZipFile(older, "w") as archive:
    archive.writestr("document.json", json.dumps(stored))
assert oanim.ops.wm.open_mainfile(filepath=str(older), confirmed="1") == {'FINISHED'}

print("test_masks: OK")
