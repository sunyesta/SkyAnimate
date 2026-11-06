"""Pin bones (BoneType::Pin, Moho's pin bones): a bone type set by Convert To, drawn as a pin; one point that nothing
connects to (children connected to it let go, keeping where they are; Ctrl P and the Bone tab's Connected keep the
offset; E grows a bone from the pin hanging with an offset), an IK chain stops at it, it traces one motion path (at the
pin). Its length is kept, so converting back gives the bone it was. Saved; files from before, where Pin was a shape,
open with the Pin type.

Run with:  OpenAnimate --background --python tests/test_pin_bones.py
"""

import json
import math
import pathlib
import tempfile
import zipfile

import oanim

ops, doc, layers, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm
ops.wm.read_homefile()
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active


def add(name, head, tail, parent=""):
    assert ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]),
                                 tail_y=str(tail[1]), parent=parent) == {'FINISHED'}


def bone(name):
    doc.frame_current = doc.frame_current
    return next(b for b in rig.bones if b["name"] == name)


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def convert(name, to):
    ops.armature.select_none()
    rig.select_bone(name)
    return ops.armature.bone_convert(type=to)


add("arm", (0, 0), (1, 0))
add("hand", (1, 0), (2, 0), parent="arm")
ops.armature.select_none()
rig.select_bone("hand")
rig.select_bone("arm", extend=True)
assert ops.armature.parent_set(type="CONNECTED") == {'FINISHED'}
assert bone("hand")["connected"], "a connected child to start with"
length = bone("arm")["length"]

# --- Convert To Pin: its connected children let go, right where they were ----------------------------------------------
assert convert("arm", "PIN") == {'FINISHED'}
assert bone("arm")["type"] == "PIN" and bone("arm")["shape"] == "BONE", bone("arm")
assert "tail" not in bone("arm") and "length" not in bone("arm"), ("a pin is one point: no tail, no length", bone("arm"))
try:
    rig.bone_set("arm", length=3.0)
    raise AssertionError("a pin has no length to set")
except ValueError:
    pass
assert not bone("hand")["connected"], "nothing connects to a pin"
assert close(bone("hand")["head"], (1, 0)) and close(bone("hand")["tail"], (2, 0)), ("the child stays put", bone("hand"))
try:
    rig.bone_set("arm", shape="PIN")
    raise AssertionError("Pin is no longer a shape")
except ValueError:
    pass

# --- Nothing connects to it -------------------------------------------------------------------------------------------
ops.armature.select_none()
rig.select_bone("hand")
rig.select_bone("arm", extend=True)
assert ops.armature.parent_set(type="CONNECTED") == {'FINISHED'}
assert not bone("hand")["connected"], "Ctrl P onto a pin keeps the offset"
assert close(bone("hand")["head"], (1, 0)), bone("hand")
assert ops.armature.bone_relations_set(bone="hand", connected="1") == {'CANCELLED'}, "nor the Bone tab's Connected"
assert not bone("hand")["connected"]

# E on a pin: a new bone from the pin, hanging from it with an offset.
names = {b["name"] for b in rig.bones}
convert("arm", "PIN")
assert ops.armature.extrude() == {'FINISHED'}
grown = next(b for b in rig.bones if b["name"] not in names)
assert not grown["connected"], ("grown from a pin: not connected", grown)
assert close(grown["head"], (0, 0)), ("it starts at the pin", grown["head"])
ops.armature.select_none()
rig.select_bone(grown["name"])
assert ops.armature.bone_delete() == {'FINISHED'}

# --- One motion path, at the pin ---------------------------------------------------------------------------------------
doc.motion_paths = True
ops.armature.select_none()
rig.select_bone("arm")
labels = sorted(p["label"] for p in wm.motion_paths())
assert labels == ["arm head"], ("a pin traces its pin alone", labels)
rig.select_bone("hand")
assert sorted(p["label"] for p in wm.motion_paths()) == ["hand head", "hand tip"], "a regular bone both"
doc.motion_paths = False

# --- An IK chain stops at a pin ----------------------------------------------------------------------------------------
add("goal", (1.5, 1.0), (1.7, 1.0))
doc.frame_current = 1
rig.select_bone("hand")
assert ops.armature.constraint_add(type="IK", bone="hand") == {'FINISHED'}
rig.bone_constraint_set("hand", 0, target="goal")
assert close(bone("hand")["head"], (1, 0)), ("the pin isn't turned by the chain: the hand stays", bone("hand"))
hand = bone("hand")
assert abs(math.atan2(hand["tail"][1] - hand["head"][1], hand["tail"][0] - hand["head"][0])) > 0.1, "the hand turns"
rig.bone_constraint_set("hand", 0, enabled=False)
doc.frame_current = 0

# --- Back to Regular: the bone it was ----------------------------------------------------------------------------------
assert convert("arm", "REGULAR") == {'FINISHED'}
assert bone("arm")["type"] == "REGULAR" and abs(bone("arm")["length"] - length) < 1e-6
convert("arm", "PIN")

# --- Saved; a file from before with Pin as a shape opens as the Pin type ----------------------------------------------
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_pins_"))
path = str(folder / "pins.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers if l.type == "BONE")
assert bone("arm")["type"] == "PIN" and bone("hand")["type"] == "REGULAR"

old = str(folder / "old.oanim")
with zipfile.ZipFile(path) as zin, zipfile.ZipFile(old, "w") as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename.endswith(".json") and b'"PIN"' in data:
            text = data.decode("utf-8").replace('"type": "PIN"', '"shape": "PIN"').replace('"type":"PIN"', '"shape":"PIN"')
            assert '"shape"' in text and '"PIN"' in text
            data = text.encode("utf-8")
        zout.writestr(item, data)
assert ops.wm.open_mainfile(filepath=old, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers if l.type == "BONE")
assert bone("arm")["type"] == "PIN" and bone("arm")["shape"] == "BONE", ("the old Pin shape is the Pin type", bone("arm"))

print("Pin bone tests passed")
