"""Bone collections, as Blender's (Armature tab > Bone Collections): named sets of a bone layer's
bones, and of the drawings inside it marked Editable in Bone Group. A bone or drawing can be in
several. Every bone layer has All first: every bone and editable drawing, nothing assigned to it or
removed from it, and it can't be removed. As in Blender, a bone (or drawing) shows while any
collection holding it shows (the eye, or, while any is soloed, the star), so while All shows,
everything does; hiding hides bones and drawings' points and wires (their artwork still renders). M
and Shift M move or assign the selection.

Run with:  OpenAnimate --background --python tests/test_bone_collections.py
"""

import json
import pathlib
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops.armature
doc.frame_current = 0
oanim.ops.layer.new_bone()
rig = layers.active
for name, x in (("jaw", 0), ("lip_L", 1), ("lip_R", 2)):
    ops.bone_add(name=name, head_x=str(x), head_y="0", tail_x=str(x), tail_y="1", parent="")
oanim.ops.layer.new()
lips = layers.active
lips.name = "lips"
lips.add_curve([(0.0, 2.0), (1.0, 2.0)])
oanim.ops.layer.move_to_group(group=str(rig.id))
layers.active = rig


def colls():
    return {c["name"]: c for c in rig.bone_collections}


def names():
    return [c["name"] for c in rig.bone_collections]


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


# --- All: always there, holding every bone ----------------------------------------------------------
assert names() == ["All"] and colls()["All"]["fixed"] and colls()["All"]["active"]
assert colls()["All"]["bones"] == ["jaw", "lip_L", "lip_R"]
assert ops.collection_remove(collection="All") == {'CANCELLED'}, "All can't be removed"
rig.select_bone("jaw")
assert ops.collection_assign(collection="All") == {'CANCELLED'}, "nothing is assigned to All"
assert ops.collection_unassign(collection="All") == {'CANCELLED'}, "or removed from it"
assert not lips.edited_with_bones and colls()["All"]["layers"] == []

# A drawing joins the bone layer with Editable in Bone Group (its Layer tab): it is then in All.
lips.edit_with_bones = True
assert lips.edited_with_bones and colls()["All"]["layers"] == ["lips"] and lips.collections == []

# --- Adding, naming, ordering ---------------------------------------------------------------------------
assert ops.collection_add(name="Main") == {'FINISHED'}
assert ops.collection_add(name="Main") == {'FINISHED'}, "a second one gets a unique name"
assert names() == ["All", "Main", "Main.001"]
assert colls()["Main.001"]["active"], "a new collection is the active one"
assert ops.collection_add(name="All") == {'FINISHED'} and "All.001" in names(), "All's name is taken"
assert ops.collection_remove(collection="All.001") == {'FINISHED'}
assert ops.collection_set(collection="Main.001", name="Mouth") == {'FINISHED'}
assert ops.collection_set(collection="Mouth", active="1") == {'FINISHED'}
assert ops.collection_move(direction="UP") == {'FINISHED'}
assert names() == ["All", "Mouth", "Main"]

# --- Assign / Remove: selected bones, and drawings (picked, or with points selected) -------------------
ops.select_none()
rig.select_bone("lip_L")
rig.select_bone("lip_R", extend=True)
lips.select = True
assert ops.collection_assign(collection="Mouth") == {'FINISHED'}
assert colls()["Mouth"]["bones"] == ["lip_L", "lip_R"] and colls()["Mouth"]["layers"] == ["lips"]
assert bone("lip_L")["collections"] == ["Mouth"] and lips.collections == ["Mouth"]
lips.select = False
ops.select_none()
rig.select_bone("lip_R")
assert ops.collection_unassign(collection="Mouth") == {'FINISHED'}
assert colls()["Mouth"]["bones"] == ["lip_L"]
assert ops.collection_unassign(collection="Mouth") == {'CANCELLED'}, "it isn't in it any more"

# A bone can be in several collections (Shift M: assign keeps the others).
ops.select_none()
rig.select_bone("lip_L")
assert ops.assign_to_collection(collection="Main") == {'FINISHED'}
assert bone("lip_L")["collections"] == ["Mouth", "Main"]
# M: move leaves the others.
assert ops.move_to_collection(collection="Main") == {'FINISHED'}
assert bone("lip_L")["collections"] == ["Main"]
assert ops.assign_to_collection(collection="Mouth") == {'FINISHED'}
# New Collection... from the menus.
rig.select_bone("jaw")
assert ops.move_to_collection(new="1") == {'FINISHED'}
assert bone("jaw")["collections"] == ["Collection"] and colls()["Collection"]["active"]
# The keys open the menus.
assert oanim.wm.event_simulate("M", area="Viewport") and oanim.wm.event_simulate("M", shift=True, area="Viewport")

# --- Showing and hiding: as in Blender, shown while any collection holding it shows ----------------------
# lip_L: Main + Mouth; jaw: Collection; lip_R: only All; lips: Mouth.
ops.select_none()
# All holds everything: while it shows, hiding another collection hides nothing.
assert ops.collection_set(collection="Mouth", visible="0") == {'FINISHED'}
ops.select_all()
assert all(b["selected"] for b in rig.bones), "All shows: every bone shows"
assert lips.edited_with_bones, "and every editable drawing"
# With All hidden, the others' eyes decide.
assert ops.collection_set(collection="All", visible="0") == {'FINISHED'}
assert not bone("lip_R")["selected"], "hiding deselects bones that no longer show (lip_R is only in All)"
assert bone("lip_L")["selected"] and bone("jaw")["selected"], "lip_L shows through Main, jaw through Collection"
assert not lips.edited_with_bones, "lips is only in Mouth, hidden"
assert ops.collection_set(collection="Main", visible="0") == {'FINISHED'}
assert not bone("lip_L")["selected"], "Main and Mouth both hidden now"
ops.select_all()
assert not bone("lip_L")["selected"] and not bone("lip_R")["selected"] and bone("jaw")["selected"]
assert ops.collection_set(collection="Mouth", visible="1") == {'FINISHED'}
ops.select_all()
assert bone("lip_L")["selected"] and lips.edited_with_bones, "Mouth shows again"
assert not colls()["All"]["shown"] and colls()["Mouth"]["shown"] and not colls()["Main"]["shown"]
# Solo: only soloed collections show, whatever their eyes say; soloing All shows everything.
assert ops.collection_show_all() == {'FINISHED'}
assert ops.collection_set(collection="Mouth", solo="1") == {'FINISHED'}
assert colls()["Mouth"]["shown"] and not colls()["Main"]["shown"] and not colls()["All"]["shown"]
assert lips.edited_with_bones
ops.select_all()
assert not bone("jaw")["selected"] and bone("lip_L")["selected"] and not bone("lip_R")["selected"]
assert ops.collection_set(collection="All", solo="1") == {'FINISHED'}
ops.select_all()
assert all(b["selected"] for b in rig.bones), "All soloed: everything shows"
assert ops.collection_unsolo_all() == {'FINISHED'}
assert all(c["shown"] for c in rig.bone_collections)

# --- Select / Deselect a collection's bones (and its drawings' points) -------------------------------------
ops.select_none()
assert ops.collection_select(collection="Mouth") == {'FINISHED'}
assert bone("lip_L")["selected"] and not bone("jaw")["selected"]
assert all(p[2] for p in lips.points), "and the points of its drawings"
assert ops.collection_deselect(collection="Mouth") == {'FINISHED'}
assert not bone("lip_L")["selected"] and not any(p[2] for p in lips.points)
assert ops.collection_select(collection="All") == {'FINISHED'}
assert all(b["selected"] for b in rig.bones), "All: every bone"
ops.select_none()
oanim.ops.mesh.select_none()

# --- New bones join the active collection (none for All), as in Blender ------------------------------------
assert ops.collection_set(collection="Main", active="1") == {'FINISHED'}
ops.bone_add(name="chin", head_x="0", head_y="-1", tail_x="0", tail_y="-2", parent="")
assert bone("chin")["collections"] == ["Main"]
assert ops.collection_set(collection="All", active="1") == {'FINISHED'}
ops.bone_add(name="nose", head_x="0", head_y="3", tail_x="0", tail_y="4", parent="")
assert bone("nose")["collections"] == []

# --- A drawing's points selected from the bone layer: M, Shift M and Remove take it --------------------------
ops.select_none()
lips.select_points([0])
assert ops.move_to_collection(collection="Main") == {'FINISHED'}
assert lips.collections == ["Main"], "its points are selected: it moves"
assert ops.assign_to_collection(collection="Mouth") == {'FINISHED'}
assert lips.collections == ["Main", "Mouth"]
assert ops.collection_unassign(collection="Mouth") == {'FINISHED'}
assert lips.collections == ["Main"]
oanim.ops.mesh.select_none()

# --- M and Shift M with a drawing active: the active and picked drawings, not the bones ---------------------
oanim.ops.layer.new()
teeth = layers.active
teeth.name = "teeth"
teeth.add_curve([(0.0, 3.0), (1.0, 3.0)])
oanim.ops.layer.move_to_group(group=str(rig.id))
assert ops.move_to_collection(collection="Mouth") == {'CANCELLED'}, "not Editable in Bone Group yet"
teeth.edit_with_bones = True
layers.active = rig
rig.select_bone("chin")  # a selected bone: left alone while a drawing is active
layers.active = teeth
lips.select = True  # picked as well
assert oanim.wm.event_simulate("M", area="Viewport") and oanim.wm.event_simulate("M", area="Layers"), "the menus open"
assert ops.move_to_collection(collection="Mouth") == {'FINISHED'}
assert teeth.collections == ["Mouth"] and lips.collections == ["Mouth"], "M: only Mouth now"
assert bone("chin")["collections"] == ["Main"], "the bones aren't touched"
assert ops.assign_to_collection(collection="Main") == {'FINISHED'}
assert teeth.collections == ["Mouth", "Main"], "Shift M keeps the others"
lips.select = False
layers.active = teeth
assert oanim.ops.layer.delete() == {'FINISHED'}
layers.active = rig

# --- Select Layers of Selected Points (the point menu): go to the layer of points edited from elsewhere -------
layers.active = rig
ops.select_none()
oanim.ops.mesh.select_none()
lips.select_points([1])
assert lips.edited_with_bones and layers.active == rig
assert oanim.ops.mesh.select_point_layers() == {'FINISHED'}
assert layers.active == lips, "the layer holding the selected point is active now"
assert lips.points[1][2], "and its point stays selected"
oanim.ops.mesh.select_none()
layers.active = rig

# --- Removing ------------------------------------------------------------------------------------------------
assert ops.collection_add(name="Spare") == {'FINISHED'}
assert ops.collection_remove_unused() == {'FINISHED'}
assert "Spare" not in colls(), "an empty collection goes"
assert ops.collection_remove(collection="Main") == {'FINISHED'}
assert bone("chin")["collections"] == [] and lips.collections == ["Mouth"], "its members just leave it"
assert colls()["All"]["bones"][-1] == "nose" and lips.edited_with_bones, "still in All"

# --- Saved, and files from before ----------------------------------------------------------------------------
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_collections_"))
path = str(folder / "collections.oanim")
assert ops.collection_set(collection="All", solo="1") == {'FINISHED'}
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in oanim.data.layers if l.type == "BONE")
lips = next(l for l in rig.children if l.name == "lips")
assert names() == ["All", "Mouth", "Collection"] and colls()["All"]["solo"]
assert bone("lip_L")["collections"] == ["Mouth"] and lips.collections == ["Mouth"] and lips.edited_with_bones

# A file saved while drawings joined collections without the checkbox: in one, it is editable.
with zipfile.ZipFile(path) as z:
    data = json.loads(z.read("document.json"))


def drop_flag(layers_json):
    for l in layers_json:
        l.pop("edit_with_bones", None)
        drop_flag(l.get("children", []))


drop_flag(data["document"]["layers"])
between = str(folder / "between.oanim")
with zipfile.ZipFile(between, "w") as z:
    z.writestr("document.json", json.dumps(data))
assert oanim.ops.wm.open_mainfile(filepath=between, confirmed="1") == {'FINISHED'}
rig = next(l for l in oanim.data.layers if l.type == "BONE")
lips = next(l for l in rig.children if l.name == "lips")
assert lips.edit_with_bones and lips.collections == ["Mouth"]

print("Bone collection tests passed")
