"""The left/right name rule (oanim.utils.flip_name, as bpy.utils.flip_name) that bone Symmetrize and
mirror editing pair bones by: suffixes, prefixes, Left/Right, a trailing number kept; and Symmetrize
on bones named with prefixes.

Run with:  OpenAnimate --background --python tests/test_flip_name.py
"""

import _oanim
import oanim

flip = oanim.utils.flip_name
pairs = {
    # Suffixes.
    "arm_L": "arm_R", "arm.L": "arm.R", "arm-L": "arm-R", "arm L": "arm R", "arm_l": "arm_r",
    # Prefixes.
    "L_foot": "R_foot", "l_lower_leg": "r_lower_leg", "L.hand": "R.hand", "L-hip": "R-hip",
    "L arm": "R arm", "l upper leg": "r upper leg", "L arm 2": "R arm 2",
    "L_upper_arm_middle_2": "R_upper_arm_middle_2",
    # Left / Right anywhere.
    "LeftArm": "RightArm", "Left_arm": "Right_arm", "upper_left_leg": "upper_right_leg",
    # A trailing number stays: after a dot, a space or an underscore.
    "arm_L.001": "arm_R.001", "Eye_Lash_L 2": "Eye_Lash_R 2", "Eye_Line_L 3": "Eye_Line_R 3",
    "Toe_1_L": "Toe_1_R", "Brow_1_L": "Brow_1_R",
}
for name, other in pairs.items():
    assert flip(name) == other, (name, flip(name), other)
    assert flip(other) == name, (other, flip(other), name)
# No side: as it is.
for name in ("Head", "Head_X", "B13", "Temp", "shoulder", "Brow_Line_1", "Layer 2", "Lever", "Loop_2"):
    assert flip(name) == name, (name, flip(name))

# Bone Symmetrize makes the other side of prefixed bones.
doc = oanim.data.document
doc.frame_current = 0
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = oanim.data.layers.active
assert oanim.ops.armature.bone_primitive_add() == {'FINISHED'}
first = rig.bones[0]["name"]
# (bone_set's own first argument is called name, so the rename goes straight to _oanim.)
_oanim.layer_bone_set(rig.id, first, {"name": "L_arm"})
rig.bone_set("L_arm", position=(0.5, 0.2))
rig.select_bone("L_arm")
assert oanim.ops.armature.symmetrize() == {'FINISHED'}
bones = {b["name"]: b for b in rig.bones}
assert "R_arm" in bones, sorted(bones)
assert abs(bones["R_arm"]["head"][0] + bones["L_arm"]["head"][0]) < 1e-4, (bones["R_arm"]["head"], bones["L_arm"]["head"])

print("flip_name: ok")
