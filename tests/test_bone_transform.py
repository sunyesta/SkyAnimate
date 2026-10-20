"""Bone keys hold Transform and Angle, the change from the setup pose (frame 0), as Blender's pose
bones do: moving a bone on frame 0 moves every pose with it; going through frames adds no keys;
saving keeps it; files from before (format 1, the bones' own values in their keys, as the Moho
converter writes) open converted.

Run with:  SkyAnimate --background --python tests/test_bone_transform.py
"""

import json
import math
import pathlib
import tempfile
import zipfile

import sky

doc = sky.data.document
layers = sky.data.layers
doc.frame_current = 0
doc.auto_key = True

sky.ops.layer.new_bone()
rig = layers.active
assert sky.ops.armature.bone_add(name="jaw", head_x="1", head_y="0", tail_x="2", tail_y="0", parent="") == {'FINISHED'}


def bone():
    return next(b for b in rig.bones if b["name"] == "jaw")


def keys(channel):
    return [(k["frame"], k["value"]) for k in rig.keyframes('bones["jaw"].' + channel)]


def rotate(v, a):
    c, s = math.cos(a), math.sin(a)
    return (v[0] * c - v[1] * s, v[0] * s + v[1] * c)


def close(a, b, eps=1e-5):
    if isinstance(a, (tuple, list)):
        return all(abs(x - y) < eps for x, y in zip(a, b))
    return abs(a - b) < eps


# --- Keys hold the change from the setup pose ----------------------------------------------------
rig.bone_set("jaw", angle=0.3)
doc.frame_current = 1
doc.frame_current = 0
assert close(bone()["setup_angle"], 0.3) and close(bone()["setup_position"], (1.0, 0.0))
doc.frame_current = 10
rig.bone_set("jaw", angle=0.8, position=(1.5, 0.25))
doc.frame_current = 11
angle_keys, position_keys = keys("angle"), keys("position")
assert [f for f, v in angle_keys] == [0, 10] and close(angle_keys[0][1], 0.0) and close(angle_keys[1][1], 0.5), angle_keys
# Transform is measured along the bone's rest axes (x along it as it rests, y across), as Blender's.
moved = rotate((0.5, 0.25), -0.3)
assert close(position_keys[0][1], (0.0, 0.0)) and close(position_keys[1][1], moved), position_keys
doc.frame_current = 10
assert close(bone()["angle"], 0.8) and close(bone()["position"], (1.5, 0.25))

# --- Moving the setup pose moves every pose with it -----------------------------------------------
doc.frame_current = 0
rig.bone_set("jaw", angle=0.5, position=(2.0, 0.0))
doc.frame_current = 10
assert close(bone()["angle"], 1.0), ("turned with the setup", bone()["angle"])
turned = rotate(moved, 0.5)
assert close(bone()["position"], (2.0 + turned[0], turned[1])), ("moved and turned with the setup", bone()["position"])
assert close(keys("angle")[1][1], 0.5), "the keys stayed the change"

# --- Going through frames adds no keys (no rounding becomes an edit) --------------------------------
for f in (3, 7, 10, 0, 13, 5, 0, 10):
    doc.frame_current = f
assert [f for f, v in keys("angle")] == [0, 10] and [f for f, v in keys("position")] == [0, 10]
assert close(keys("angle")[0][1], 0.0)

# --- Saved and opened ------------------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_bone_transform_")) / "rig.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.type == "BONE")
assert close(bone()["setup_angle"], 0.5) and close(keys("angle")[1][1], 0.5)
doc.frame_current = 10
assert close(bone()["angle"], 1.0) and close(bone()["position"], (2.0 + turned[0], turned[1]))

# --- Format 1 files (the bones' own values in their keys) open converted ---------------------------
with zipfile.ZipFile(path) as z:
    entries = {n: z.read(n) for n in z.namelist()}
root = json.loads(entries["document.json"])
root["version"] = 1


def bones_of(nodes):
    for n in nodes:
        yield from n.get("bones", [])
        yield from bones_of(n.get("children", []))


for b in bones_of(root["document"]["layers"]):
    b.pop("setup_position", None)
    b.pop("setup_angle", None)
    # As Moho keeps them: the angle itself, frame 0's key the setup.
    b["angle_keys"] = [dict(k, value=v) for k, v in zip(b["angle_keys"], (0.25, 1.25))]
    b["position_keys"] = [dict(k, value=v) for k, v in zip(b["position_keys"], ([3.0, 0.0], [3.5, 1.0]))]
    b["angle"] = 0.25
    b["position"] = [3.0, 0.0]
root["document"]["frame_current"] = 0
entries["document.json"] = json.dumps(root).encode("utf-8")
with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
    for n, data in entries.items():
        z.writestr(n, data)
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.type == "BONE")
assert close(bone()["setup_angle"], 0.25) and close(bone()["setup_position"], (3.0, 0.0))
assert close(keys("angle")[0][1], 0.0) and close(keys("angle")[1][1], 1.0), keys("angle")
assert close(keys("position")[1][1], rotate((0.5, 1.0), -0.25)), keys("position")
doc.frame_current = 10
assert close(bone()["angle"], 1.25) and close(bone()["position"], (3.5, 1.0)), "it plays as it did"

# --- Along the bone: a bone pointing up, moved up, moved along itself (Transform x) -----------------
doc.frame_current = 0
assert sky.ops.armature.bone_add(name="neck", head_x="0", head_y="5", tail_x="0", tail_y="6", parent="") == {'FINISHED'}
doc.frame_current = 10
neck = next(b for b in rig.bones if b["name"] == "neck")
rig.bone_set("neck", position=(neck["position"][0], neck["position"][1] + 0.2))
doc.frame_current = 11
along = rig.keyframes('bones["neck"].position')[-1]["value"]
assert close(along, (0.2, 0.0)), ("x runs along the bone", along)

print("Bone transform tests passed")
