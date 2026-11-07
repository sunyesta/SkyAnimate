"""Operators that change nothing leave nothing behind: no undo step (the undo stack skips a step equal to the one
before), and a refused one (CANCELLED) changes nothing at all. Undo leaves the timeline's switches (Auto Keying, Time
Together) as they are, as it leaves the frame shown.

Run with:  OpenAnimate --background --python tests/test_noop_ops.py
"""

import oanim

ops, doc, layers, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm


def steps():
    return wm.undo_history()[1]


ops.wm.read_homefile()
doc.auto_key = True
ops.layer.new()
lay = layers.active
lay.add_curve([(0.0, 0.0), (1.0, 0.0)])
for f, x in ((0, 0.0), (10, 1.0), (20, 2.0)):
    doc.frame_current = f
    lay.location_x = x
doc.frame_current = 30
assert ops.anim.keys_select(frame_start="10", frame_end="20") == {'FINISHED'}

# --- Changing nothing: no undo step ------------------------------------------------------------------------------------
for name, op in (("Scale Keys by 1", lambda: ops.anim.keys_scale(factor="1")),
                 ("Duplicate Keys by 0 frames", lambda: ops.anim.keys_duplicate(frames="0")),
                 ("an interpolation the keys have", lambda: ops.anim.keys_interpolation_set(
                     interp=lay.keyframes("position")[1]["interp"]))):
    was, frames = steps(), [k["frame"] for k in lay.keyframes("position")]
    op()
    assert steps() == was, (name, "made an undo step that changes nothing", wm.undo_history()[0][-1])
    assert [k["frame"] for k in lay.keyframes("position")] == frames, name
# Something changed still makes one.
was = steps()
assert ops.anim.keys_scale(factor="2") == {'FINISHED'}
assert steps() == was + 1
assert ops.ed.undo() == {'FINISHED'}

# --- Refused: nothing changes ---------------------------------------------------------------------------------------
# Delete Keyframe on a frame with no keys leaves a channel keyed only on the setup frame as it is.
ops.layer.new()
lone = layers.active
lone.add_curve([(0.0, 0.0), (1.0, 0.0)])
doc.frame_current = 0
lone.location_x = 0.5
assert ops.anim.keyframe_insert() == {'FINISHED'}
assert [k["frame"] for k in lone.keyframes("position")] == [0]
doc.frame_current = 7
was = steps()
assert ops.anim.keyframe_delete() == {'CANCELLED'}
assert [k["frame"] for k in lone.keyframes("position")] == [0], ("a refused delete took the setup key",
                                                               lone.keyframes("position"))
assert steps() == was

# Bake Dynamics with no spring on bakes nothing, and leaves the Dynamics constraint on.
doc.frame_current = 0
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
assert ops.armature.bone_add(name="tail", head_x="0", head_y="0", tail_x="0", tail_y="1", parent="") == {'FINISHED'}
rig.select_bone("tail")
assert ops.armature.constraint_add(type="DYNAMICS", bone="tail") == {'FINISHED'}
rig.bone_constraint_set("tail", 0, angle_spring={"enabled": False}, position_spring={"enabled": False},
                        scale_spring={"enabled": False})
was = steps()
assert ops.armature.dynamics_bake() == {'CANCELLED'}
assert rig.bone_constraints("tail")[0]["enabled"], "a refused bake switched the dynamics off"
assert steps() == was

# --- Undo leaves the timeline's switches ------------------------------------------------------------------------------
layers.active = lay
doc.frame_current = 25
lay.location_x = 3.0
assert ops.anim.keyframe_insert() == {'FINISHED'}
doc.auto_key = False
doc.time_together = True
assert ops.ed.undo() == {'FINISHED'}
assert doc.auto_key is False and doc.time_together is True, (doc.auto_key, doc.time_together)
assert ops.ed.redo() == {'FINISHED'}
assert doc.auto_key is False and doc.time_together is True

# --- Python: a bone renamed through bone_set ---------------------------------------------------------------------------
rig.bone_set("tail", name="wag")
assert [b["name"] for b in rig.bones] == ["wag"], rig.bones

print("test_noop_ops: all passed")
