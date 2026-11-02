"""Move, Rotate and Scale as operators the ViewportActionPanel can adjust (G, R and S end with them):
points (mesh.translate / rotate / resize), bones (armature.translate / rotate / resize) and the
Transform Bones tool (armature.transform). Changing an amount redoes the move from where it began.
Posing, Move Only Bones keeps the drawings where they are: the points the bones bend, and layers
following a bone, are keyed to undo the bend; on frame 0 bones bend nothing anyway.

Run with:  OpenAnimate --background --python tests/test_transform_panel.py
"""

import math

import _oanim
import oanim

doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm
doc.frame_current = 0


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def shown(layer):
    points, _, _ = layer.evaluated()
    return [layer.to_document(p[:2]) for p in points]


# --- Points: Move, then the amount changed in the panel -------------------------------------------------
art = layers.active
art.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0)])
oanim.ops.mesh.select_all()  # an undo step: the redo goes back to here
assert oanim.ops.mesh.translate(move_x="0.5", move_y="0.25") == {'FINISHED'}
assert close(art.points[0][:2], (0.5, 0.25)) and close(art.points[2][:2], (1.5, 1.25))
assert wm.undo_history()[0][-1] == "Move"
last = wm.last_operation()
assert last and last[0] == "mesh.translate" and last[1]["move_x"] == "0.5", last
assert wm.redo_last(move_x="1.0", move_y="0") == "FINISHED"
assert close(art.points[0][:2], (1.0, 0.0)), ("redone from where it began, not added", art.points[0])
assert oanim.ops.mesh.rotate(angle="90") == {'FINISHED'}
assert oanim.ops.mesh.resize(scale_x="2", scale_y="1") == {'FINISHED'}
oanim.ops.ed.undo()
oanim.ops.ed.undo()

# --- Bones: Move Only Bones while posing -----------------------------------------------------------------
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
oanim.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
oanim.ops.layer.new()
skin = layers.active
skin.add_curve([(0.2, 0.1), (0.6, 0.2), (0.9, -0.1)])
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
skin = next(l for l in layers.all() if l.id == skin.id)
oanim.ops.layer.new()
badge = layers.active
badge.add_curve([(0.5, 0.5), (0.7, 0.5), (0.6, 0.7)])
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
badge = next(l for l in layers.all() if l.id == badge.id)
layers.active = badge
assert oanim.ops.layer.parent_bone_set(bone="arm") == {'FINISHED'}
layers.active = skin
assert oanim.ops.weight.automatic() == {'FINISHED'}

layers.active = rig
doc.frame_current = 10
oanim.ops.armature.select_all()  # an undo step: the redo goes back to here
skin_before, badge_before = shown(skin), shown(badge)


def arm():
    return next(b for b in rig.bones if b["name"] == "arm")


assert oanim.ops.armature.translate(move_x="0.3", move_y="0.1", only_bones="1", posing="1") == {'FINISHED'}
assert close(arm()["position"], (0.3, 0.1)), arm()["position"]
assert all(close(a, b) for a, b in zip(shown(skin), skin_before)), ("the drawing stays", shown(skin), skin_before)
assert all(close(a, b) for a, b in zip(shown(badge), badge_before)), "a layer following the bone stays too"
assert wm.undo_history()[0][-1] == "Move Bones"
# Off in the panel: the same move, the drawings now follow the bone.
assert wm.redo_last(move_x="0.3", move_y="0.1", only_bones="0", posing="1") == "FINISHED"
assert close(arm()["position"], (0.3, 0.1))
assert close(shown(skin)[0], (skin_before[0][0] + 0.3, skin_before[0][1] + 0.1)), shown(skin)[0]
# The panel shows the checkbox for a move made posing.
last = wm.last_operation()
assert last[0] == "armature.translate" and last[1]["posing"] == "1"
oanim.ops.ed.undo()

# Turning keeps them too; and so do the other frames' poses, as they were.
assert oanim.ops.armature.rotate(angle="30", only_bones="1", posing="1") == {'FINISHED'}
assert abs(math.degrees(arm()["angle"]) - 30.0) < 1e-3, arm()["angle"]
assert all(close(a, b) for a, b in zip(shown(skin), skin_before)), "turned, the drawing stays"
assert all(close(a, b) for a, b in zip(shown(badge), badge_before))
oanim.ops.ed.undo()
assert oanim.ops.armature.resize(scale="1.5", only_bones="1", posing="1") == {'FINISHED'}
assert all(close(a, b) for a, b in zip(shown(skin), skin_before)), "scaled, the drawing stays"
oanim.ops.ed.undo()

# The Transform Bones tool's move (and its header).
assert oanim.ops.armature.transform(origin_x="0", origin_y="0", move_x="0.2", move_y="0", scale_x="1",
                                    scale_y="1", angle="45", only_bones="1", posing="1") == {'FINISHED'}
assert all(close(a, b) for a, b in zip(shown(skin), skin_before)), "the gizmo's move keeps it too"
assert wm.undo_history()[0][-1] == "Transform Bones"
assert wm.redo_last(origin_x="0", origin_y="0", move_x="0.2", move_y="0", scale_x="1", scale_y="1", angle="45",
                    only_bones="0", posing="1") == "FINISHED"
assert not close(shown(skin)[0], skin_before[0]), "off: the drawing follows"
oanim.ops.ed.undo()

# Points the move doesn't bend get no keys.
oanim.ops.layer.new()
loose = layers.active
loose.add_curve([(3.0, 3.0), (4.0, 3.0)])
layers.active = rig
oanim.ops.armature.select_all()
loose_before = list(loose.points)
assert oanim.ops.armature.translate(move_x="0.3", only_bones="1", posing="1") == {'FINISHED'}
assert list(loose.points) == loose_before

# On frame 0 bones bend nothing: the checkbox doesn't show, and the drawing is as it was.
doc.frame_current = 0
before = shown(skin)
assert oanim.ops.armature.translate(move_x="0.1", only_bones="1", posing="0") == {'FINISHED'}
assert all(close(a, b) for a, b in zip(shown(skin), before))
assert wm.last_operation()[1]["posing"] == "0"

print("transform_panel: ok")
