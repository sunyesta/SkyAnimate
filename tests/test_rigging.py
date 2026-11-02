"""Rigging and modifiers: bone layers, bones, weights (automatic and assigned), Parent to Bone, posing on
frames, the Armature modifier every layer in a bone layer gets, the Mirror modifier, and
mirrored halves following their own side's bones (_L / _R), with Symmetrize.

Run with:  OpenAnimate --background --python tests/test_rigging.py
"""

import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


# --- A bone layer with a vector layer inside -----------------------------------------------------
art = layers.active
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
assert rig.type == "BONE" and rig.name.startswith("Bones")
layers.active = art
assert art.modifiers == [], "outside a bone layer: no Armature modifier"
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
assert art.parent == rig
assert [m["type"] for m in art.modifiers] == ["ARMATURE"], "inside: the Armature modifier comes with it"
assert oanim.ops.object.modifier_remove(index="0") == {'CANCELLED'}, "and can't be removed"

# --- Bones: only on the setup frame -------------------------------------------------------------
layers.active = rig
doc.frame_current = 5
assert oanim.ops.armature.bone_add(head_x="0", head_y="0", tail_x="0.5", tail_y="0") == {'CANCELLED'}
doc.frame_current = 0
assert oanim.ops.armature.bone_add(name="spine", head_x="0", head_y="0", tail_x="0", tail_y="0.5") == {'FINISHED'}
assert oanim.ops.armature.bone_add(name="arm_L", head_x="0", head_y="0.5", tail_x="0.5", tail_y="0.5") == {'FINISHED'}
bones = {b["name"]: b for b in rig.bones}
assert bones["arm_L"]["parent"] == "spine", "a new bone hangs from the active bone"
assert close(bones["arm_L"]["head"], (0, 0.5)) and close(bones["arm_L"]["tail"], (0.5, 0.5))
assert abs(bones["spine"]["angle"] - math.pi / 2) < 1e-5

# --- Symmetrize: the other side by name --------------------------------------------------------
rig.select_bone("")
assert oanim.ops.armature.symmetrize() == {'FINISHED'}
bones = {b["name"]: b for b in rig.bones}
assert "arm_R" in bones and bones["arm_R"]["parent"] == "spine"
assert close(bones["arm_R"]["head"], (0, 0.5)) and close(bones["arm_R"]["tail"], (-0.5, 0.5)), bones["arm_R"]
assert oanim.ops.armature.symmetrize() == {'FINISHED'} and len(rig.bones) == 3, "again: updates, no duplicates"

# --- Mirror modifier ----------------------------------------------------------------------------
layers.active = art
# Half an outline: from the axis out to the right arm and back to the axis (both ends at x = 0).
half = [(0.0, 0.2), (0.4, 0.45), (0.4, 0.55), (0.0, 0.8)]
art.add_curve(half, closed=False, fill=True)
assert oanim.ops.object.modifier_add(type="MIRROR") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["ARMATURE", "MIRROR"], "new modifiers go at the bottom"
# Above the Armature modifier, it mirrors the setup drawing and each half follows its own side's bones.
assert oanim.ops.object.modifier_move_to(index="1", to="0") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["MIRROR", "ARMATURE"]
points, curves, shapes = art.evaluated()
assert len(points) == 6, "the two axis points are shared, the two others reflected"
assert curves == 1 and len(shapes) == 1 and shapes[0][1] and shapes[0][2], \
    "the half outline and its reflection join into one closed shape"
assert (-0.4, 0.45) in [tuple(round(c, 5) for c in p) for p in points]

art.modifier_set(0, merge=False)
points, curves, shapes = art.evaluated()
assert len(points) == 8 and curves == 2 and len(shapes) == 2, "without merging: a separate mirrored copy"
art.modifier_set(0, merge=True)

# --- Weights and posing --------------------------------------------------------------------------
# Automatic Weights gives a new drawing its weights. Assign the arm's points wholly to arm_L; the mirrored
# ones then follow arm_R.
assert not any(art.weights), "connected to nothing, new points have none"
oanim.ops.mesh.select_none()
assert oanim.ops.weight.automatic() == {'FINISHED'}
assert all(art.weights), "every point has weights"
art.select_points([1, 2])
rig.select_bone("arm_L")
assert oanim.ops.weight.assign(weight="1") == {'FINISHED'}
assert art.point_bones[1:3] == ["arm_L", "arm_L"] and art.weights[1] == {"arm_L": 1.0}

doc.frame_current = 10
# Angles are in the parent's frame, and the spine points up: 0 turns both arms straight up.
assert abs(bones["arm_L"]["angle"] + math.pi / 2) < 1e-5 and abs(bones["arm_R"]["angle"] - math.pi / 2) < 1e-5
rig.bone_set("arm_L", angle=0.0)
rig.bone_set("arm_R", angle=0.0)
doc.frame_current = 11
assert [k["frame"] for k in rig.keyframes('bones["arm_L"].angle')] == [0, 10], "posing keys the bone"
doc.frame_current = 10
points, _, _ = art.evaluated()
# arm_L turned 90 degrees about its head (0, 0.5): (0.4, 0.45) -> (0.05, 0.9).
assert close(points[1], (0.05, 0.9)), points[1]
# The reflection of that point (bound to arm_R, which turned the other way) mirrors it.
mirrored = [p for p in points[4:]]
assert any(close(p, (-0.05, 0.9)) for p in mirrored), mirrored
# Points on the axis have automatic weights, mostly spine's: spine is still, so they barely move.
assert close(points[0], (0.0, 0.2), 0.05)

art.modifier_set(0, mirror_bones=False)
points, _, _ = art.evaluated()
assert not any(close(p, (-0.05, 0.9)) for p in points[4:]), "without mirror_bones both halves follow arm_L"
art.modifier_set(0, mirror_bones=True)

doc.frame_current = 0
points, _, _ = art.evaluated()
assert close(points[1], (0.4, 0.45)), "at the setup frame nothing moves"

# Turning the Armature modifier off in the viewport leaves the drawing in its setup shape.
doc.frame_current = 10
art.modifier_set(1, show_viewport=False)
points, _, _ = art.evaluated()
assert close(points[1], (0.4, 0.45))
art.modifier_set(1, show_viewport=True)

# Modifier order matters: Armature above Mirror mirrors the posed drawing instead.
assert oanim.ops.object.modifier_move(index="1", direction="-1") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["ARMATURE", "MIRROR"]
assert oanim.ops.object.modifier_move(index="0", direction="1") == {'FINISHED'}
# Dragging a modifier in the Modifiers tab moves it to a place in the stack.
assert oanim.ops.object.modifier_move_to(index="1", to="0") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["ARMATURE", "MIRROR"]
assert oanim.ops.object.modifier_move_to(index="0", to="1") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["MIRROR", "ARMATURE"]

# --- Parent to Bone ---------------------------------------------------------------------------------
doc.frame_current = 0
assert oanim.ops.layer.new() == {'FINISHED'}
hat = layers.active
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
assert oanim.ops.layer.parent_bone_set(bone="spine") == {'FINISHED'} and hat.parent_bone == "spine"
# Giving its points weights takes it off the parent bone.
hat.add_curve([(0.0, 1.0), (0.1, 1.1)])
assert oanim.ops.weight.automatic() == {'FINISHED'}
assert hat.parent_bone == "" and all(hat.weights)
oanim.ops.ed.undo()
assert hat.parent_bone == "spine"
doc.frame_current = 20
rig.bone_set("spine", position=(0.25, 0.0))
doc.frame_current = 20
assert abs(hat.location_x) < 1e-6, "the layer's own transform is untouched"
points, _, _ = art.evaluated()

# --- Saved with the document ------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_rig_")) / "rig.oanim")
before_bones = rig.bones
before_eval = art.evaluated()
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
doc = oanim.data.document
rig = next(l for l in oanim.data.layers if l.type == "BONE")
art = next(l for l in rig.children if l.point_count > 0)
assert [b["name"] for b in rig.bones] == [b["name"] for b in before_bones]
assert art.point_bones[1:3] == ["arm_L", "arm_L"]
assert [m["type"] for m in art.modifiers] == ["MIRROR", "ARMATURE"]
after_eval = art.evaluated()
assert len(after_eval[0]) == len(before_eval[0])
for a, b in zip(after_eval[0], before_eval[0]):
    assert close(a, b), (a, b)

# Moving a bone on the setup frame rigs: the artwork stays where it is, even mid-drag.
doc.frame_current = 0
before_setup = art.evaluated()[0]
rig.bone_set("spine", angle=0.3)  # not recorded yet, as during a drag
assert all(close(a, b) for a, b in zip(art.evaluated()[0], before_setup))
rig.bone_set("spine", angle=math.pi / 2)

# Reparenting keeps a bone where it is.
oanim.data.layers.active = rig
before = {b["name"]: b for b in rig.bones}
assert oanim.ops.armature.bone_parent_set(bone="arm_R", parent="") == {'FINISHED'}
after = {b["name"]: b for b in rig.bones}
assert after["arm_R"]["parent"] == "" and close(after["arm_R"]["head"], before["arm_R"]["head"])
assert close(after["arm_R"]["tail"], before["arm_R"]["tail"])
assert oanim.ops.armature.bone_parent_set(bone="spine", parent="arm_L") == {'CANCELLED'}, "no loops"
assert oanim.ops.armature.bone_parent_set(bone="arm_R", parent="spine") == {'FINISHED'}

# X deletes bones over the viewport when a bone layer is active (any frame).
doc.frame_current = 7
rig.select_bone("arm_R")
assert oanim.wm.event_simulate("X", area="Viewport")
assert "arm_R" not in [b["name"] for b in rig.bones]
oanim.ops.ed.undo()
assert "arm_R" in [b["name"] for b in rig.bones]
doc.frame_current = 0

# Deleting a bone: points that followed only it stay where they are (no new weights).
doc.frame_current = 0
oanim.data.layers.active = rig
rig.select_bone("arm_L")
assert oanim.ops.armature.bone_delete() == {'FINISHED'}
assert art.point_bones[1:3] == ["", ""] and art.weights[1] == {}
oanim.ops.armature.select_none()  # an undo step: new points would get weights here
assert art.weights[1] == {}, "and they don't get automatic weights again"

# Editable in Bone Group: with the bone layer active, that layer's points can be picked and moved.
oanim.data.layers.active = rig
assert oanim.ops.mesh.select_all() == {'CANCELLED'}, "points of the layers inside aren't editable by default"
art.edit_with_bones = True
assert art.edited_with_bones
assert oanim.ops.mesh.select_all() == {'FINISHED'}
assert all(p[2] for p in art.points), "the layer's points were selected from the bone layer"
oanim.ops.armature.select_none()
assert not any(p[2] for p in art.points), "Alt A deselects them with the bones"
assert oanim.ops.mesh.select_all() == {'FINISHED'}
assert oanim.wm.event_simulate("G", area="Viewport"), "G moves them, with no bone picked"
oanim.ops.mesh.select_none()
# Hiding the All collection hides them too: no longer edited with the bones.
assert oanim.ops.armature.collection_set(collection="All", visible="0") == {'FINISHED'}
assert not art.edited_with_bones and oanim.ops.mesh.select_all() == {'CANCELLED'}
assert oanim.ops.armature.collection_set(collection="All", visible="1") == {'FINISHED'}
art.edit_with_bones = False

# Converting between groups and bone groups.
oanim.data.layers.active = rig
assert oanim.ops.layer.convert_to_group() == {'FINISHED'}
assert rig.type == "GROUP" and [m["type"] for m in art.modifiers] == ["MIRROR"], "no bones, no Armature"
assert oanim.ops.layer.convert_to_bone() == {'FINISHED'}
assert rig.type == "BONE" and [m["type"] for m in art.modifiers] == ["MIRROR", "ARMATURE"]

# A layer moved out of the bone layer loses its Armature modifier.
oanim.data.layers.active = art
assert oanim.ops.layer.move_to_group(group="0") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["MIRROR"]

# --- A group's Parent Bone moves everything inside it, overriding their own --------------------------
doc = oanim.data.document
doc.frame_current = 0
oanim.ops.layer.new_bone()
grig = oanim.data.layers.active
oanim.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
oanim.ops.armature.bone_add(name="other", head_x="0", head_y="2", tail_x="1", tail_y="2", parent="")
oanim.ops.layer.new()
inner = oanim.data.layers.active
inner.add_curve([(0.5, 0.5), (0.8, 0.5)])
oanim.ops.layer.group()
grp = oanim.data.layers.active
oanim.ops.layer.move_to_group(group=str(grig.id))
assert grp.parent == grig and inner.parent == grp
oanim.data.layers.active = inner
assert oanim.ops.layer.parent_bone_set(bone="other", deform="LAYER") == {'FINISHED'}  # its own, to be overridden
oanim.data.layers.active = grp
assert oanim.ops.layer.parent_bone_set(bone="arm") == {'FINISHED'}
assert inner.parent_bone_group == grp.name and grp.parent_bone_group == ""
doc.frame_current = 10
grig.bone_set("arm", angle=1.5707963)
grig.bone_set("other", position=(5.0, 2.0))
doc.frame_current = doc.frame_current
# It moves once, as a whole, with the group's bone: not bent by its weights, not by "other".
assert all(abs(a - b) < 1e-4 for a, b in zip(inner.evaluated()[0][0], (0.5, 0.5))), inner.evaluated()[0]
shown = inner.to_document((0.5, 0.5))
assert abs(shown[0] + 0.5) < 1e-3 and abs(shown[1] - 0.5) < 1e-3, ("turned a quarter about the bone", shown)
# Out of the group, its own Parent Bone takes over again.
doc.frame_current = 0
oanim.data.layers.active = grp
assert oanim.ops.layer.parent_bone_set(bone="") == {'FINISHED'}
assert inner.parent_bone_group == ""
doc.frame_current = 10
shown = inner.to_document((0.5, 0.5))
assert abs(shown[0] - 5.5) < 1e-3, ("follows its own bone again", shown)
doc.frame_current = 0

# Ctrl I inverts the bone selection, over the viewport (Select > Invert).
rig_layer = next(l for l in oanim.data.layers.all() if l.type == "BONE")
oanim.data.layers.active = rig_layer
names = [b["name"] for b in rig_layer.bones]
assert len(names) >= 2, names
oanim.ops.armature.select_none()
rig_layer.select_bone(names[0])
assert oanim.wm.event_simulate("I", ctrl=True, area="Viewport")
picked = {b["name"]: b["selected"] for b in rig_layer.bones}
assert not picked[names[0]] and all(picked[n] for n in names[1:]), picked
assert oanim.ops.armature.select_invert() == {'FINISHED'}
picked = {b["name"]: b["selected"] for b in rig_layer.bones}
assert picked[names[0]] and not any(picked[n] for n in names[1:]), picked

print("Rigging tests passed")
