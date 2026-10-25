"""Dynamics Squash & Stretch: Max Stretch and Max Squash, each with its own checkbox and soft limit,
never shorter than a tenth; animatable, saved, and old files' one limit held both ways.

Run with:  OpenAnimate --background --python tests/test_dynamics_squash.py
"""

import json
import math
import pathlib
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0

oanim.ops.layer.new_bone()
rig = layers.active


def add(name, head, tail, parent=""):
    assert oanim.ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]),
                                     tail_y=str(tail[1]), parent=parent) == {'FINISHED'}


def key_bone(name, frame, **values):
    doc.frame_current = frame
    rig.select_bone(name)
    rig.bone_set(name, **values)
    assert oanim.ops.anim.keyframe_insert() == {'FINISHED'}


def lengths(name, frames=range(1, 30)):
    out = []
    for f in frames:
        doc.frame_current = f
        b = next(b for b in rig.bones if b["name"] == name)
        out.append(math.dist(b["head"], b["tail"]))
    return out


add("pusher", (20, 4), (21, 4))
add("spring", (21, 4), (22, 4), parent="pusher")
assert oanim.ops.armature.constraint_add(bone="spring", type="DYNAMICS") == {'FINISHED'}
rig.bone_constraint_set("spring", 0, angle_spring={"enabled": False}, scale_spring={"enabled": True, "stretch": 1.0})
key_bone("pusher", 1, position=(20.0, 4.0))
key_bone("pusher", 6, position=(23.0, 4.0))

spring = rig.bone_constraints("spring")[0]["scale_spring"]
assert not spring["use_squash_limit"] and abs(spring["squash_limit"] - 0.3) < 1e-6, "Max Squash off, 0.7x ready"
assert abs(spring["limit"] - 0.5) < 1e-6, "Max Stretch 1.5x ready"
free = lengths("spring")
assert min(free) < 0.95 and max(free) > 1.01, ("it squashes, then stretches past", min(free), max(free))

# Max Squash alone: the squash is held, the stretch is not.
rig.bone_constraint_set("spring", 0, scale_spring={"use_squash_limit": True, "squash_limit": 0.02})
held = lengths("spring")
assert min(held) >= 0.98 - 1e-4, ("squashed no shorter than 0.98x", min(held))
assert max(held) > 1.01, ("the stretch has its own limit (off)", max(held))

# Max Stretch alone: the other way round.
rig.bone_constraint_set("spring", 0, scale_spring={"use_squash_limit": False, "use_limit": True, "limit": 0.005})
held = lengths("spring")
assert max(held) <= 1.005 + 1e-4, ("stretched no longer than 1.005x", max(held))
assert min(held) < 0.95, ("the squash is its own", min(held))
rig.bone_constraint_set("spring", 0, scale_spring={"use_limit": False})

# Never shorter than a tenth, however hard it is pushed with no limit.
doc.frame_current = 0
add("shover", (40, 4), (41, 4))
add("squashy", (41, 4), (42, 4), parent="shover")
assert oanim.ops.armature.constraint_add(bone="squashy", type="DYNAMICS") == {'FINISHED'}
rig.bone_constraint_set("squashy", 0, angle_spring={"enabled": False},
                        scale_spring={"enabled": True, "stretch": 2.0, "bounce": 0.9})
key_bone("shover", 1, position=(40.0, 4.0))
key_bone("shover", 2, position=(70.0, 4.0))
assert min(lengths("squashy", range(1, 12))) >= 0.1 - 1e-4, "a tenth at the least"

# Animatable: Max Squash keys, named so in the timeline.
path = 'bones["spring"].constraints[0].scale_spring.squash_limit'
doc.frame_current = 1
rig.bone_constraint_set("spring", 0, scale_spring={"use_squash_limit": True, "squash_limit": 0.2})
assert oanim.ops.anim.keyframe_toggle(layer=str(rig.id), paths=path) == {'FINISHED'}
doc.frame_current = 10
rig.bone_constraint_set("spring", 0, scale_spring={"squash_limit": 0.4})
doc.frame_current = 11
assert [(k["frame"], round(k["value"], 2)) for k in rig.keyframes(path)] == [(0, 0.2), (1, 0.2), (10, 0.4)]

# Saved and opened again.
tmp = pathlib.Path(tempfile.mkdtemp())
saved = str(tmp / "squash.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=saved) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=saved, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers if l.type == "BONE")
spring = rig.bone_constraints("spring")[0]["scale_spring"]
assert spring["use_squash_limit"] and not spring["use_limit"], spring
assert [(k["frame"], round(k["value"], 2)) for k in rig.keyframes(path)] == [(0, 0.2), (1, 0.2), (10, 0.4)]


# Saved before Max Squash: the one limit holds both ways.
def before_squash(value):
    if isinstance(value, dict):
        if "squash_limit" in value:
            for k in ("use_squash_limit", "squash_limit", "squash_limit_keys"):
                value.pop(k, None)
            if "limit" in value:
                value["use_limit"], value["limit"] = True, 0.25
        for v in value.values():
            before_squash(v)
    elif isinstance(value, list):
        for v in value:
            before_squash(v)


with zipfile.ZipFile(saved) as z:
    data = json.loads(z.read("document.json"))
before_squash(data)
old = str(tmp / "old.oanim")
with zipfile.ZipFile(old, "w") as z:
    z.writestr("document.json", json.dumps(data))
assert oanim.ops.wm.open_mainfile(filepath=old, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers if l.type == "BONE")
spring = rig.bone_constraints("spring")[0]["scale_spring"]
assert spring["use_limit"] and spring["use_squash_limit"], spring
assert abs(spring["limit"] - 0.25) < 1e-6 and abs(spring["squash_limit"] - 0.25) < 1e-6, spring

print("dynamics squash: ok")
