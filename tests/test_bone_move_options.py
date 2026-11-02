"""The bone moves' ViewportActionPanel checkboxes: Move Without Children (G, R and S move only the picked
bones, their unpicked children staying where they are; a connected child keeps its direction on its
parent's tip), and how Keep Poses, Move Only Bones and Move Without Children stay as last set for the
next moves until the frame or the action changes. With Keep Poses, children keep their poses too.

Run with:  OpenAnimate --background --python tests/test_bone_move_options.py
"""

import math

import oanim

layers = oanim.data.layers
doc = oanim.data.document
ops = oanim.ops
doc.frame_current = 0


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
# The hand grows from the arm's tip, connected to it.
rig.select_bone("arm", part="TAIL")
assert ops.armature.extrude() == {'FINISHED'}
assert rig.move_bone_ends((0.6, 0.0))
HAND = "arm.001"
ops.armature.bone_add(name="prop", head_x="0.5", head_y="0.5", tail_x="0.5", tail_y="1", parent="arm")


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


def pick(name):
    layers.active = rig
    ops.armature.select_none()
    rig.select_bone(name)


assert bone(HAND)["connected"] and not bone("prop")["connected"], [(b["name"], b["connected"]) for b in rig.bones]
assert oanim.wm.move_options() == {"keep_poses": False, "only_bones": False, "without_children": False}

# --- On the setup frame bones move without their children anyway (Blender's edit mode) -------------
prop_was = bone("prop")
pick("arm")
assert ops.armature.rotate(angle="90", posing="0") == {'FINISHED'}
assert close(bone("arm")["tail"], (0, 1)) and close(bone("prop")["head"], prop_was["head"]), bone("prop")
ops.ed.undo()

# --- Posing ------------------------------------------------------------------------------------------
doc.frame_current = 10
pick("arm")
prop_was = bone("prop")
assert ops.armature.rotate(angle="45", posing="1", without_children="1") == {'FINISHED'}
assert close(bone("prop")["head"], prop_was["head"]) and close(bone("prop")["tail"], prop_was["tail"]), bone("prop")
hand = bone(HAND)
s = math.sqrt(0.5)
assert close(hand["head"], (s, s)) and close(hand["tail"], (s + 0.6, s)), ("on the tip, pointing right", hand)
assert oanim.wm.move_options()["without_children"], "kept for the next moves"
ops.ed.undo()
# Without it the children turn with the arm.
pick("arm")
assert ops.armature.rotate(angle="90", posing="1", without_children="0") == {'FINISHED'}
assert close(bone(HAND)["tail"], (0, 1.6)) and close(bone("prop")["head"], (-0.5, 0.5)), (bone(HAND), bone("prop"))
assert not oanim.wm.move_options()["without_children"]
ops.ed.undo()
# Moved: the connected child's base goes with the arm's tip, its tip as far along as it was.
pick("arm")
assert ops.armature.translate(move_x="0.5", move_y="0", posing="1", without_children="1") == {'FINISHED'}
assert close(bone("prop")["head"], prop_was["head"]), bone("prop")
assert close(bone(HAND)["head"], (1.5, 0)) and close(bone(HAND)["tail"], (2.1, 0)), bone(HAND)
ops.ed.undo()

# --- Kept until the frame or the action changes -------------------------------------------------------
pick("arm")
assert ops.armature.rotate(angle="10", posing="1", only_bones="1", without_children="1") == {'FINISHED'}
assert oanim.wm.move_options()["only_bones"] and oanim.wm.move_options()["without_children"]
doc.frame_current = 11
assert oanim.wm.move_options() == {"keep_poses": False, "only_bones": False, "without_children": False}, \
    "a new frame: off again"
pick("arm")
assert ops.armature.rotate(angle="10", posing="1", only_bones="1", without_children="1") == {'FINISHED'}
assert ops.anim.action_new(name="wave") == {'FINISHED'}
assert not oanim.wm.move_options()["only_bones"] and not oanim.wm.move_options()["without_children"], \
    "another action: off again"
assert ops.anim.action_set(name="") == {'FINISHED'}

# Keep Poses too.
doc.frame_current = 0
pick("arm")
assert ops.armature.rotate(angle="5", posing="0", keep_poses="1") == {'FINISHED'}
assert oanim.wm.move_options()["keep_poses"]
pick("arm")
assert ops.armature.rotate(angle="-5", posing="0") == {'FINISHED'}, "a move not saying keeps it as it was"
assert oanim.wm.move_options()["keep_poses"]
doc.frame_current = 10
doc.frame_current = 0
assert not oanim.wm.move_options()["keep_poses"]

# --- Keep Poses with the children: they keep their poses too ------------------------------------------
doc.frame_current = 20
pick("arm")
ops.armature.select_all()
rig.bone_set("arm", angle=0.4)
rig.bone_set(HAND, angle=-0.6)
rig.bone_set("prop", angle=0.3, position=(0.6, 0.4))
ops.anim.keyframe_insert()
doc.frame_current = 21
doc.frame_current = 20
posed = {b["name"]: (b["head"], b["tail"]) for b in rig.bones}
for without in ("0",):
    doc.frame_current = 0
    pick("arm")
    assert ops.armature.rotate(angle="30", posing="0", keep_poses="1", without_children=without) == {'FINISHED'}
    assert ops.armature.translate(move_x="0.2", move_y="-0.1", posing="0", keep_poses="1",
                                  without_children=without) == {'FINISHED'}
    doc.frame_current = 20
    for name, (head, tail) in posed.items():
        b = bone(name)
        assert close(b["head"], head, 1e-3) and close(b["tail"], tail, 1e-3), \
            ("frame 20 keeps every bone's pose", without, name, b["head"], b["tail"], head, tail)
    ops.ed.undo()
    ops.ed.undo()

# --- Selection to Cursor (and to Grid, to Active) take both options, shown while posing ----------------------
doc.frame_current = 14
oanim.wm.set_cursor((0.4, -0.6))
pick("arm")
# An undo step holding the pick (the panel's redo undoes back to it).
ops.anim.keyframe_insert()
prop_was = bone("prop")
arm_was = bone("arm")
assert ops.view.snap_selected_to_cursor(without_children="1", only_bones="0") == {'FINISHED'}
assert not close(bone("arm")["head"], arm_was["head"]), "the arm moved"
assert close(bone("prop")["head"], prop_was["head"]) and close(bone("prop")["tail"], prop_was["tail"]),     ("its child stays", bone("prop"), prop_was)
idname, args, _ = oanim.wm.last_operation()
assert idname == "view.snap_selected_to_cursor" and args["posing"] == "1" and args["without_children"] == "1", args
# Unticked in the panel: the child comes along.
r = oanim.wm.redo_last(without_children=False)
assert r == "FINISHED", (r, oanim.wm.last_operation())
assert not close(bone("prop")["head"], prop_was["head"]), "off: the child moves with it"
ops.ed.undo()
# Left out, they are as the bone moves last had them (kept until the frame changes).
assert ops.armature.translate(move_x="0", move_y="0.1", posing="1", without_children="1") == {'FINISHED'}
ops.ed.undo()
pick("arm")
assert ops.view.snap_selected_to_cursor() == {'FINISHED'}
assert close(bone("prop")["head"], prop_was["head"]), "kept from the last move"
assert oanim.wm.last_operation()[1]["without_children"] == "1"
ops.ed.undo()
# --- Move Only Bones on a dynamic bone: the drawing stays where it showed, as the swing it has now --------
ops.wm.read_homefile(confirmed="1")
layers = oanim.data.layers
doc = oanim.data.document
doc.frame_current = 0
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="head", head_x="0", head_y="0", tail_x="0", tail_y="1", parent="")
ops.armature.bone_add(name="hair", head_x="0", head_y="1", tail_x="1.5", tail_y="1", parent="head")
rig.select_bone("hair")
assert ops.armature.constraint_add(type="DYNAMICS") == {'FINISHED'}
ops.layer.new()
strand = layers.active
strand.add_curve([(0.3, 1.0), (0.8, 1.05), (1.4, 1.0)])
ops.layer.move_to_group(group=str(rig.id))
strand = next(l for l in layers.all() if l.id == strand.id)
layers.active = strand
assert ops.weight.automatic() == {'FINISHED'}
# The head turns quickly, so on frame 12 the hair still swings behind it.
for frame, angle in ((1, 0.0), (8, 1.2)):
    doc.frame_current = frame
    layers.active = rig
    rig.bone_set("head", angle=angle)
    rig.select_bone("head")
    ops.anim.keyframe_insert()
doc.frame_current = 12
was = strand.evaluated()[0]
layers.active = rig
rig.select_bone("hair")
assert ops.armature.rotate(angle="35", posing="1", only_bones="1") == {'FINISHED'}
now = strand.evaluated()[0]
assert all(close(a, b, 2e-3) for a, b in zip(now, was)), ("the strand stays", now, was)
ops.ed.undo()
assert ops.armature.rotate(angle="35", posing="1", only_bones="0") == {'FINISHED'}
assert not all(close(a, b, 2e-3) for a, b in zip(strand.evaluated()[0], was)), "off: it follows the hair"

ops.ed.undo()

# Move Only Bones: the strand the hair bends stays where it shows.
was = strand.evaluated()[0]
layers.active = rig
ops.armature.select_none()
rig.select_bone("hair")
ops.anim.keyframe_insert()
was = strand.evaluated()[0]
assert ops.view.snap_selected_to_cursor(only_bones="1") == {'FINISHED'}
assert all(close(a, b, 2e-3) for a, b in zip(strand.evaluated()[0], was)), "the strand stays"
assert oanim.wm.redo_last(only_bones=False) == "FINISHED"
assert not all(close(a, b, 2e-3) for a, b in zip(strand.evaluated()[0], was)), "off: it follows the hair"
ops.ed.undo()
# On the setup frame they aren't offered (bones move without children there anyway).
doc.frame_current = 0
pick("head")
assert ops.view.snap_selected_to_cursor() == {'FINISHED'}
assert oanim.wm.last_operation()[1]["posing"] == "0"
ops.ed.undo()

print("bone_move_options: ok")
