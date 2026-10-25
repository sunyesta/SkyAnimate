"""Bone Relations (Blender's): Connected, Inherit Rotation, Inherit Scale (Full, Uniform, None) and Local
Location, set through armature.bone_relations_set; Keep Animation keeps every keyed frame looking as it
did; turning a bone that doesn't inherit lands where it is turned; saved with the file.

Run with:  OpenAnimate --background --python tests/test_bone_relations.py
"""

import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="hand", head_x="1", head_y="0", tail_x="1.5", tail_y="0", parent="arm")


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


def world_angle(name):
    b = bone(name)
    return math.atan2(b["tail"][1] - b["head"][1], b["tail"][0] - b["head"][0])


def length(name):
    b = bone(name)
    return math.dist(b["head"], b["tail"])


def at(frame, **bones):
    doc.frame_current = frame
    for name, values in bones.items():
        rig.bone_set(name, **values)
    doc.frame_current = frame + 1
    doc.frame_current = frame


def relations(**values):
    args = {k: (("1" if v else "0") if isinstance(v, bool) else str(v)) for k, v in values.items()}
    return ops.armature.bone_relations_set(bone="hand", **args)


h = bone("hand")
assert h["inherit_rotation"] and h["inherit_scale"] == "FULL" and h["local_location"] and not h["connected"], h

# Connected (setup frame): already on the parent's tail; moved off first, connecting puts it back.
ops.armature.bone_parent_set(bone="hand", parent="")
at(0, hand=dict(position=(1.2, 0.3)))
ops.armature.bone_parent_set(bone="hand", parent="arm")
assert relations(connected=True) == {'FINISHED'}
assert bone("hand")["connected"] and math.dist(bone("hand")["head"], bone("arm")["tail"]) < 1e-5, bone("hand")["head"]
relations(connected=False)
at(0, hand=dict(position=(1.0, 0.0), angle=0.0, length=0.5))
assert math.dist(bone("hand")["tail"], (1.5, 0.0)) < 1e-5, bone("hand")["tail"]

# The arm turned a quarter turn: the hand turns with it, and without Inherit Rotation keeps its own angle
# in the picture (its head still where the arm takes it).
arm_rest = bone("arm")["setup_angle"]
at(10, arm=dict(angle=arm_rest + math.pi / 2))
assert abs(world_angle("hand") - math.pi / 2) < 1e-4, world_angle("hand")
assert relations(inherit_rotation=False) == {'FINISHED'}
assert abs(world_angle("hand")) < 1e-4, ("keeps its own angle", world_angle("hand"))
assert math.dist(bone("hand")["head"], (0.0, 1.0)) < 1e-4, bone("hand")["head"]
# Turned by hand there, it turns by as much in the picture.
rig.select_bone("hand")
ops.armature.rotate(angle="20")
assert abs(world_angle("hand") - math.radians(20)) < 1e-3, world_angle("hand")
assert math.dist(bone("hand")["head"], (0.0, 1.0)) < 1e-4, "its head stays"
relations(inherit_rotation=True)

# Inherit Scale: the arm stretched to twice its length (and kept its thickness).
at(20, arm=dict(angle=arm_rest, scale=(1.0, 2.0)), hand=dict(angle=0.0))
assert abs(length("hand") - 1.0) < 1e-4 and math.dist(bone("hand")["head"], (2.0, 0.0)) < 1e-4, (length("hand"), bone("hand")["head"])
relations(inherit_scale="NONE")
assert abs(length("hand") - 0.5) < 1e-4 and math.dist(bone("hand")["head"], (2.0, 0.0)) < 1e-4, ("its own size", length("hand"))
relations(inherit_scale="UNIFORM")
assert abs(length("hand") - 0.5 * math.sqrt(2.0)) < 1e-3, ("its overall size", length("hand"))
assert ops.armature.bone_relations_set(bone="hand", inherit_scale="SIDEWAYS") == {'CANCELLED'}
relations(inherit_scale="FULL")

# Keep Animation: the arm keyed turning on frames 10 and 30; switching Inherit Rotation off keeps the hand
# where it was there (its angle keyed back). (On a frame where the parent is turned and stretched too, the
# child is skewed by Full scale either way: kept as near as an angle and a scale can.)
at(30, arm=dict(angle=arm_rest + 0.7, scale=(1.0, 1.0)))
at(10, arm=dict(scale=(1.0, 1.0)))
doc.frame_current = 10
before_10 = world_angle("hand")
doc.frame_current = 30
before = world_angle("hand")
assert relations(inherit_rotation=False, keep_animation=True) == {'FINISHED'}
doc.frame_current = 30
assert abs(world_angle("hand") - before) < 1e-3, ("kept on frame 30", world_angle("hand"), before)
assert 30 in [k["frame"] for k in rig.keyframes('bones["hand"].angle')]
doc.frame_current = 10
assert abs(world_angle("hand") - before_10) < 1e-3, ("kept on frame 10 too", world_angle("hand"), before_10)

# Local Location only changes how the Transform reads, not the pose or the keys.
head_before = bone("hand")["head"]
assert relations(local_location=False) == {'FINISHED'}
assert not bone("hand")["local_location"] and math.dist(bone("hand")["head"], head_before) < 1e-6

# Saved with the file.
path = str(pathlib.Path(tempfile.mkdtemp()) / "relations.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.type == "BONE")
h = bone("hand")
assert not h["inherit_rotation"] and h["inherit_scale"] == "FULL" and not h["local_location"], h
doc.frame_current = 30
assert abs(world_angle("hand") - before) < 1e-3

print("bone relations: ok")
