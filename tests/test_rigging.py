"""Rigging and modifiers: bone layers, bones, weights (automatic and assigned), Parent to Bone, posing on
frames, the Armature modifier every layer in a bone layer gets, the Mirror modifier, and
mirrored halves following their own side's bones (_L / _R), with Symmetrize.

Run with:  SkyAnimate --background --python tests/test_rigging.py
"""

import math
import pathlib
import tempfile

import sky

doc = sky.data.document
layers = sky.data.layers


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


# --- A bone layer with a vector layer inside -----------------------------------------------------
art = layers.active
assert sky.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
assert rig.type == "BONE" and rig.name.startswith("Bones")
layers.active = art
assert art.modifiers == [], "outside a bone layer: no Armature modifier"
assert sky.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
assert art.parent == rig
assert [m["type"] for m in art.modifiers] == ["ARMATURE"], "inside: the Armature modifier comes with it"
assert sky.ops.object.modifier_remove(index="0") == {'CANCELLED'}, "and can't be removed"

# --- Bones: only on the setup frame -------------------------------------------------------------
layers.active = rig
doc.frame_current = 5
assert sky.ops.armature.bone_add(head_x="0", head_y="0", tail_x="0.5", tail_y="0") == {'CANCELLED'}
doc.frame_current = 0
assert sky.ops.armature.bone_add(name="spine", head_x="0", head_y="0", tail_x="0", tail_y="0.5") == {'FINISHED'}
assert sky.ops.armature.bone_add(name="arm_L", head_x="0", head_y="0.5", tail_x="0.5", tail_y="0.5") == {'FINISHED'}
bones = {b["name"]: b for b in rig.bones}
assert bones["arm_L"]["parent"] == "spine", "a new bone hangs from the active bone"
assert close(bones["arm_L"]["head"], (0, 0.5)) and close(bones["arm_L"]["tail"], (0.5, 0.5))
assert abs(bones["spine"]["angle"] - math.pi / 2) < 1e-5

# --- Symmetrize: the other side by name --------------------------------------------------------
rig.select_bone("")
assert sky.ops.armature.symmetrize() == {'FINISHED'}
bones = {b["name"]: b for b in rig.bones}
assert "arm_R" in bones and bones["arm_R"]["parent"] == "spine"
assert close(bones["arm_R"]["head"], (0, 0.5)) and close(bones["arm_R"]["tail"], (-0.5, 0.5)), bones["arm_R"]
assert sky.ops.armature.symmetrize() == {'FINISHED'} and len(rig.bones) == 3, "again: updates, no duplicates"

# --- Mirror modifier ----------------------------------------------------------------------------
layers.active = art
# Half an outline: from the axis out to the right arm and back to the axis (both ends at x = 0).
half = [(0.0, 0.2), (0.4, 0.45), (0.4, 0.55), (0.0, 0.8)]
art.add_curve(half, closed=False, fill=True)
assert sky.ops.object.modifier_add(type="MIRROR") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["MIRROR", "ARMATURE"], "new modifiers go above Armature"
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
# New points got automatic weights. Assign the arm's points wholly to arm_L; the mirrored ones then
# follow arm_R.
assert all(art.weights), "every point has weights"
art.select_points([1, 2])
rig.select_bone("arm_L")
assert sky.ops.weight.assign(weight="1") == {'FINISHED'}
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
assert sky.ops.object.modifier_move(index="1", direction="-1") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["ARMATURE", "MIRROR"]
assert sky.ops.object.modifier_move(index="0", direction="1") == {'FINISHED'}
# Dragging a modifier in the Modifiers tab moves it to a place in the stack.
assert sky.ops.object.modifier_move_to(index="1", to="0") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["ARMATURE", "MIRROR"]
assert sky.ops.object.modifier_move_to(index="0", to="1") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["MIRROR", "ARMATURE"]

# --- Parent to Bone ---------------------------------------------------------------------------------
doc.frame_current = 0
assert sky.ops.layer.new() == {'FINISHED'}
hat = layers.active
assert sky.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
assert sky.ops.layer.parent_bone_set(bone="spine") == {'FINISHED'} and hat.parent_bone == "spine"
# Giving its points weights takes it off the parent bone.
hat.add_curve([(0.0, 1.0), (0.1, 1.1)])
assert sky.ops.weight.automatic() == {'FINISHED'}
assert hat.parent_bone == "" and all(hat.weights)
sky.ops.ed.undo()
assert hat.parent_bone == "spine"
doc.frame_current = 20
rig.bone_set("spine", position=(0.25, 0.0))
doc.frame_current = 20
assert abs(hat.location_x) < 1e-6, "the layer's own transform is untouched"
points, _, _ = art.evaluated()

# --- Saved with the document ------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_rig_")) / "rig.sky")
before_bones = rig.bones
before_eval = art.evaluated()
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
doc = sky.data.document
rig = next(l for l in sky.data.layers if l.type == "BONE")
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
sky.data.layers.active = rig
before = {b["name"]: b for b in rig.bones}
assert sky.ops.armature.bone_parent_set(bone="arm_R", parent="") == {'FINISHED'}
after = {b["name"]: b for b in rig.bones}
assert after["arm_R"]["parent"] == "" and close(after["arm_R"]["head"], before["arm_R"]["head"])
assert close(after["arm_R"]["tail"], before["arm_R"]["tail"])
assert sky.ops.armature.bone_parent_set(bone="spine", parent="arm_L") == {'CANCELLED'}, "no loops"
assert sky.ops.armature.bone_parent_set(bone="arm_R", parent="spine") == {'FINISHED'}

# X deletes bones over the viewport when a bone layer is active (any frame).
doc.frame_current = 7
rig.select_bone("arm_R")
assert sky.wm.event_simulate("X", area="Viewport")
assert "arm_R" not in [b["name"] for b in rig.bones]
sky.ops.ed.undo()
assert "arm_R" in [b["name"] for b in rig.bones]
doc.frame_current = 0

# Deleting a bone: points that followed only it stay where they are (no new weights).
doc.frame_current = 0
sky.data.layers.active = rig
rig.select_bone("arm_L")
assert sky.ops.armature.bone_delete() == {'FINISHED'}
assert art.point_bones[1:3] == ["", ""] and art.weights[1] == {}
sky.ops.armature.select_none()  # an undo step: new points would get weights here
assert art.weights[1] == {}, "and they don't get automatic weights again"

# Converting between groups and bone groups.
sky.data.layers.active = rig
assert sky.ops.layer.convert_to_group() == {'FINISHED'}
assert rig.type == "GROUP" and [m["type"] for m in art.modifiers] == ["MIRROR"], "no bones, no Armature"
assert sky.ops.layer.convert_to_bone() == {'FINISHED'}
assert rig.type == "BONE" and [m["type"] for m in art.modifiers] == ["MIRROR", "ARMATURE"]

# A layer moved out of the bone layer loses its Armature modifier.
sky.data.layers.active = art
assert sky.ops.layer.move_to_group(group="0") == {'FINISHED'}
assert [m["type"] for m in art.modifiers] == ["MIRROR"]

print("Rigging tests passed")
