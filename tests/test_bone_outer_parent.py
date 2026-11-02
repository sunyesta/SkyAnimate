"""A bone in a nested bone group can hang from a bone of a bone group around it, staying where it rests: it then moves
and turns with that bone, what follows it goes along, and it is moved like any child. It lets go by Clear Parent, or by
itself when the parent is deleted or its bone group leaves. Saved; copied along with the groups.

Run with:  OpenAnimate --background --python tests/test_bone_outer_parent.py
"""
import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0


def find(layer):
    return next(l for l in layers.all() if l.id == layer.id)


def bone(rig, name):
    return next(b for b in find(rig).bones if b["name"] == name)


def close(a, b, eps=1e-3):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def head(rig, name):
    rig = find(rig)
    return rig.to_document(rig.bone_pose(name)[:2])


def settle(frame=None):
    now = doc.frame_current if frame is None else frame
    doc.frame_current = now + 1
    doc.frame_current = now


assert ops.layer.new_bone() == {'FINISHED'}
outer = layers.active
outer.name = "Outer"
ops.armature.bone_add(name="hip", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="chest", head_x="0", head_y="1", tail_x="1", tail_y="1", parent="")
assert ops.layer.new_bone() == {'FINISHED'}
inner = layers.active
inner.name = "Inner"
assert ops.layer.move_to_group(group=str(outer.id)) == {'FINISHED'}
inner = find(inner)
layers.active = inner
ops.armature.bone_add(name="thigh", head_x="2", head_y="0", tail_x="2", tail_y="-1", parent="")
ops.armature.bone_add(name="shin", head_x="2", head_y="-1", tail_x="2", tail_y="-2", parent="thigh")
ops.layer.new()
art = layers.active
art.add_curve([(2.0, -0.5), (2.0, -1.5)])
assert ops.layer.move_to_group(group=str(inner.id)) == {'FINISHED'}
art = find(art)
layers.active = art
assert ops.weight.automatic() == {'FINISHED'}
settle(0)

# --- Hanging it: it stays where it rests -------------------------------------------------------------------------------
layers.active = find(inner)
rest = head(inner, "thigh"), head(inner, "shin")
find(inner).select_bone("thigh")
assert ops.armature.bone_parent_set(bone="thigh", parent="hip", parent_layer=str(outer.id)) == {'FINISHED'}
settle(0)
assert bone(inner, "thigh")["outer_parent"] == ("Outer", "hip"), bone(inner, "thigh")["outer_parent"]
assert bone(inner, "thigh")["parent"] == ""
assert close(head(inner, "thigh"), rest[0]) and close(head(inner, "shin"), rest[1]), "stays where it is"

# --- It follows ----------------------------------------------------------------------------------------------------------
# The hip turned a quarter about (0, 0): the thigh's head goes from (2, 0) to (0, 2), its child with it.
doc.frame_current = 10
find(outer).bone_set("hip", angle=math.pi / 2)
settle()
assert close(head(inner, "thigh"), (0.0, 2.0)), head(inner, "thigh")
assert close(head(inner, "shin"), (1.0, 2.0)), head(inner, "shin")
assert close(head(outer, "chest"), (0.0, 1.0)), "another bone stays"
# What follows the bones goes along.
points = find(art).evaluated()[0]
assert close(find(art).to_document(points[0]), (0.5, 2.0), 0.05), points
# Moved by a key of its own, along its own frame, on top.
find(inner).bone_set("thigh", angle=bone(inner, "thigh")["setup_angle"])
settle()

# Moving it in the viewport's way, the move is where the mouse went, whatever the hip's turn.
was = head(inner, "thigh")
layers.active = find(inner)
ops.armature.select_none()
find(inner).select_bone("thigh")
assert ops.armature.translate(move_x="0.25", move_y="0.5", posing="1") == {'FINISHED'}
settle()
now = head(inner, "thigh")
assert close((now[0] - was[0], now[1] - was[1]), (0.25, 0.5)), (was, now)
ops.ed.undo()
settle()
assert close(head(inner, "thigh"), was), "undone"

# Moved together with its parent, it moves once.
doc.frame_current = 12
settle()
was = head(inner, "thigh")
layers.active = find(outer)
ops.armature.select_none()
find(outer).select_bone("hip")
layers.active = find(inner)
find(inner).select_bone("thigh")
layers.active = find(outer)
assert ops.armature.translate(move_x="0.25", move_y="0.5", posing="1") == {'FINISHED'}
settle()
now = head(inner, "thigh")
assert close((now[0] - was[0], now[1] - was[1]), (0.25, 0.5)), ("once", was, now)
ops.ed.undo()
# A turn too: it turns with its parent, by as much.
settle()


def way(rig, name):
    pose = find(rig).bone_pose(name)
    return math.atan2(pose[3] - pose[1], pose[2] - pose[0])


hip_was, thigh_was = way(outer, "hip"), way(inner, "thigh")
layers.active = find(outer)
ops.armature.select_none()
find(outer).select_bone("hip")
layers.active = find(inner)
find(inner).select_bone("thigh")
layers.active = find(outer)
assert ops.armature.rotate(angle="30", posing="1") == {'FINISHED'}
settle()
hip_turn, thigh_turn = way(outer, "hip") - hip_was, way(inner, "thigh") - thigh_was
assert abs(hip_turn) > 0.1 and abs(math.remainder(hip_turn - thigh_turn, 2 * math.pi)) < 1e-3, (hip_turn, thigh_turn)
ops.ed.undo()
doc.frame_current = 10
settle()
was = head(inner, "thigh")

# Its bone group moved: it hangs from the hip, not from the group.
layers.active = find(inner)
find(inner).location_x = 0.5
settle()
assert close(head(inner, "thigh"), was), (head(inner, "thigh"), was)
find(inner).location_x = 0.0
settle()

# --- Ctrl P, with the parent's bone group active --------------------------------------------------------------------
doc.frame_current = 0
settle(0)
layers.active = find(inner)
assert ops.armature.bone_parent_set(bone="thigh", parent="") == {'FINISHED'}
settle(0)
assert bone(inner, "thigh")["outer_parent"] is None
assert close(head(inner, "thigh"), rest[0]), "it let go where it rests"
doc.frame_current = 10
settle()
assert close(head(inner, "thigh"), rest[0]), "and doesn't follow any more"
doc.frame_current = 0
settle(0)
layers.active = find(inner)
ops.armature.select_none()
find(inner).select_bone("thigh")
layers.active = find(outer)
find(outer).select_bone("chest")
assert ops.armature.parent_set() == {'FINISHED'}
settle(0)
assert bone(inner, "thigh")["outer_parent"] == ("Outer", "chest"), bone(inner, "thigh")["outer_parent"]
assert close(head(inner, "thigh"), rest[0])
# Alt P lets go.
assert ops.armature.parent_clear() == {'FINISHED'}
settle(0)
assert bone(inner, "thigh")["outer_parent"] is None and close(head(inner, "thigh"), rest[0])
# With the nested bone group staying active, the parent Shift-clicked in the one around it.
layers.active = find(inner)
ops.armature.select_none()
find(inner).select_bone("thigh")
find(outer).select_bone("hip")
layers.active = find(inner)
assert ops.armature.parent_set() == {'FINISHED'}
settle(0)
assert bone(inner, "thigh")["outer_parent"] == ("Outer", "hip"), bone(inner, "thigh")["outer_parent"]
assert ops.armature.parent_clear() == {'FINISHED'}
settle(0)
# Only one around it: a bone group inside isn't a parent's, nor one beside it.
layers.active = find(outer)
assert ops.armature.bone_parent_set(bone="chest", parent="thigh", parent_layer=str(inner.id)) == {'CANCELLED'}
layers.active = find(inner)
assert ops.armature.bone_parent_set(bone="thigh", parent="hip", parent_layer=str(outer.id)) == {'FINISHED'}
settle(0)

# --- Saved ---------------------------------------------------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_outer_parent_")) / "outer.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
outer = next(l for l in layers.all() if l.name == "Outer")
inner = next(l for l in layers.all() if l.name == "Inner")
assert bone(inner, "thigh")["outer_parent"] == ("Outer", "hip")
settle(10)
assert close(head(inner, "thigh"), (0.0, 2.0)), head(inner, "thigh")
doc.frame_current = 0
settle(0)

# --- Copied with the bone groups: it hangs from the copy ------------------------------------------------------------------
layers.active = find(outer)
assert ops.layer.duplicate() == {'FINISHED'}
copies = [l for l in layers.all() if l.type == "BONE" and l.id not in (outer.id, inner.id)]
copy_outer = next(l for l in copies if any(b["name"] == "hip" for b in l.bones))
copy_inner = next(l for l in copies if any(b["name"] == "thigh" for b in l.bones))
settle(0)
assert bone(copy_inner, "thigh")["outer_parent"] == (copy_outer.name, "hip"), bone(copy_inner, "thigh")["outer_parent"]
oanim.ops.ed.undo()
settle(0)

# --- Letting go by itself: the parent deleted, or its bone group leaving ----------------------------------------------------
layers.active = find(outer)
ops.armature.select_none()
find(outer).select_bone("hip")
assert ops.armature.bone_delete() == {'FINISHED'}
settle(0)
assert bone(inner, "thigh")["outer_parent"] is None, bone(inner, "thigh")["outer_parent"]
assert close(head(inner, "thigh"), rest[0]), head(inner, "thigh")
ops.ed.undo()
settle(0)
assert bone(inner, "thigh")["outer_parent"] == ("Outer", "hip"), "undone"

layers.active = find(inner)
assert ops.layer.move_to_group(group="0") == {'FINISHED'}
settle(0)
assert bone(inner, "thigh")["outer_parent"] is None
assert close(head(inner, "thigh"), rest[0]), head(inner, "thigh")

print("Outer parent tests passed")
