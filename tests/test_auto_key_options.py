"""Auto-keying's options (the dropdown beside the record button): Blender's Add & Replace and Replace
modes, and Moho's Auto-Freeze Keys. Saved with the project.

Run with:  OpenAnimate --background --python tests/test_auto_key_options.py
"""

import os
import shutil
import tempfile

import oanim

doc = oanim.data.document
assert doc.auto_key and doc.auto_key_mode == "ADD_REPLACE" and not doc.auto_freeze_keys, "the defaults"

doc.frame_current = 0
oanim.ops.layer.new_bone()
rig = oanim.data.layers.active
rig.key_whole_drawing = False  # keyed point by point here
oanim.ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
oanim.ops.armature.bone_add(name="b", head_x="0", head_y="1", tail_x="1", tail_y="1", parent="")
oanim.ops.armature.bone_add(name="c", head_x="0", head_y="2", tail_x="1", tail_y="2", parent="")


def frames(bone, channel="angle"):
    return [k["frame"] for k in rig.keyframes('bones["%s"].%s' % (bone, channel))]


def pose(frame, bone, angle):
    """Turn `bone` on `frame`, then leave the frame (which records it, as auto-key does)."""
    doc.frame_current = frame
    rig.bone_set(bone, angle=angle)
    doc.frame_current = frame + 1


# Add & Replace: an edit keys what it changes, a new key where there was none.
pose(10, "a", 0.5)
assert frames("a") == [0, 10] and frames("b") == [], (frames("a"), frames("b"))

# Replace: only keys already on the frame change; elsewhere the change isn't keyed.
doc.auto_key_mode = "REPLACE"
pose(20, "a", 0.8)
assert frames("a") == [0, 10], ("no key added on frame 20", frames("a"))
pose(10, "a", 0.6)
assert frames("a") == [0, 10]
doc.frame_current = 10
assert abs([b for b in rig.bones if b["name"] == "a"][0]["angle"] - 0.6) < 1e-5, "the key on 10 changed"
pose(30, "b", 0.4)
assert frames("b") == [], "an unanimated bone isn't keyed in Replace"
doc.auto_key_mode = "ADD_REPLACE"

# Auto-Freeze Keys: keying one bone keys the others shown in its bone layer on that frame too.
doc.auto_freeze_keys = True
pose(40, "a", 0.2)
assert 40 in frames("a") and 40 in frames("b") and 40 in frames("c"), (frames("a"), frames("b"), frames("c"))
assert 40 in frames("b", "position") and 40 in frames("c", "scale"), "angle, position and scale all held"
# The frozen bones keep the pose they had there.
doc.frame_current = 40
b_angle = [b for b in rig.bones if b["name"] == "b"][0]["angle"]
pose(50, "b", 1.0)
doc.frame_current = 40
assert abs([b for b in rig.bones if b["name"] == "b"][0]["angle"] - b_angle) < 1e-5, "frame 40 holds"
# Not on the setup frame, and nothing when no bone was keyed.
before = frames("c")
doc.frame_current = 60
doc.frame_current = 61
assert frames("c") == before, "no edit, no freeze"
# Not with Replace (it adds no keys).
doc.auto_key_mode = "REPLACE"
pose(10, "a", 0.7)
assert 10 not in frames("c"), "Replace never adds keys, frozen or not"
doc.auto_key_mode = "ADD_REPLACE"

# A point keyed keys every point of its drawing on that frame, so the whole shape holds.
oanim.ops.layer.new()
drawing = oanim.data.layers.active
drawing.key_whole_drawing = False  # keyed point by point here
drawing.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
oanim.ops.mesh.select_none()
doc.frame_current = 70
oanim.ops.sculpt.stroke(brush="GRAB", path="0,0;0,0.5", radius=0.3, strength=1.0, falloff="CONSTANT")
doc.frame_current = 71
point_keys = lambda i, what="pos": [k["frame"] for k in drawing.keyframes("points[%d].%s" % (i, what))]
assert 70 in point_keys(0), point_keys(0)
assert 70 in point_keys(1) and 70 in point_keys(2), ("the others are frozen", point_keys(1), point_keys(2))
assert 70 in point_keys(2, "radius"), "widths too"
doc.frame_current = 70
assert abs(drawing.points[2][1]) < 1e-6, "a frozen point keeps where it was"
# Without Auto-Freeze, only the point that moved.
doc.auto_freeze_keys = False
doc.frame_current = 80
oanim.ops.sculpt.stroke(brush="GRAB", path="0,0.5;0,0.8", radius=0.3, strength=1.0, falloff="CONSTANT")
doc.frame_current = 81
assert 80 in point_keys(0) and 80 not in point_keys(2), (point_keys(0), point_keys(2))
doc.auto_freeze_keys = True

# Saved with the project.
doc.auto_key_mode = "REPLACE"
path = os.path.join(tempfile.mkdtemp(prefix="oanim_autokey_"), "keying.oanim")
oanim.ops.wm.save_as_mainfile(filepath=path)
doc.auto_key_mode = "ADD_REPLACE"
doc.auto_freeze_keys = False
oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
doc = oanim.data.document
assert doc.auto_key_mode == "REPLACE" and doc.auto_freeze_keys, (doc.auto_key_mode, doc.auto_freeze_keys)

shutil.rmtree(os.path.dirname(path), ignore_errors=True)
print("auto key options: ok")
