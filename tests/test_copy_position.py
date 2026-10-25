"""Copy Position goes to where its target's base is (Blender's Copy Location), Factor the way there, whatever
its parents do; Its Move copies the target's move from its setup pose instead (Moho's control bones), which
files saved before keep.

Run with:  OpenAnimate --background --python tests/test_copy_position.py
"""

import json
import math
import pathlib
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="head", head_x="0", head_y="0", tail_x="0", tail_y="2", parent="")
ops.armature.bone_add(name="eye", head_x="0.5", head_y="1", tail_x="0.5", tail_y="1.1", parent="head")
ops.armature.bone_add(name="target", head_x="3", head_y="1", tail_x="3", tail_y="1.2", parent="")
rig.select_bone("eye")
assert ops.armature.constraint_add(type="COPY_POSITION", bone="eye") == {'FINISHED'}
rig.bone_constraint_set("eye", 0, target="target")
c = rig.bone_constraints("eye")[0]
assert not c["follow_move"] and c["factor"] == 1.0, c


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


def close(a, b, eps=1e-4):
    return math.dist(a, b) < eps


doc.frame_current = 10
doc.frame_current = 10
assert close(bone("eye")["head"], bone("target")["head"]), ("it goes to the target", bone("eye")["head"])
# Whatever its parent does, and wherever the target goes.
rig.bone_set("head", angle=0.7)
rig.bone_set("target", position=(2.0, -1.5))
doc.frame_current = 11
doc.frame_current = 10
assert close(bone("eye")["head"], bone("target")["head"]), ("still on it", bone("eye")["head"], bone("target")["head"])
# Factor: part of the way.
rig.bone_constraint_set("eye", 0, enabled=False)
doc.frame_current = 10
own = bone("eye")["head"]
rig.bone_constraint_set("eye", 0, enabled=True, factor=0.5)
doc.frame_current = 10
half = [(a + b) / 2 for a, b in zip(own, bone("target")["head"])]
assert close(bone("eye")["head"], half), ("halfway", bone("eye")["head"], half)
# On the setup frame nothing acts.
doc.frame_current = 0
assert close(bone("eye")["head"], (0.5, 1.0))

# Its Move: the target's move from its setup is added to its own place (Moho's control bones).
rig.bone_constraint_set("eye", 0, factor=1.0, follow_move=True)
doc.frame_current = 20
rig.bone_set("head", angle=bone("head")["setup_angle"])
rig.bone_set("target", position=(3.5, 1.0))
doc.frame_current = 21
doc.frame_current = 20
# The target moved 0.5 from its setup: so does the eye, from its own place (in its parent's axes, as Moho).
assert abs(math.dist(bone("eye")["head"], (0.5, 1.0)) - 0.5) < 1e-4, ("it copies the move", bone("eye")["head"])

# Saved; and files saved before Copy Position could go to its target keep copying the move.
path = str(pathlib.Path(tempfile.mkdtemp()) / "copy_position.oanim")
rig.bone_constraint_set("eye", 0, follow_move=False)
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.type == "BONE")
assert not rig.bone_constraints("eye")[0]["follow_move"]
with zipfile.ZipFile(path) as z:
    data = json.loads(z.read("document.json"))


def strip(value):
    if isinstance(value, dict):
        value.pop("follow_move", None)
        for v in value.values():
            strip(v)
    elif isinstance(value, list):
        for v in value:
            strip(v)


strip(data)
old = path.replace("copy_position.oanim", "old.oanim")
with zipfile.ZipFile(old, "w") as z:
    z.writestr("document.json", json.dumps(data))
assert ops.wm.open_mainfile(filepath=old, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.type == "BONE")
assert rig.bone_constraints("eye")[0]["follow_move"], "an older file keeps copying the move"

print("copy position: ok")
