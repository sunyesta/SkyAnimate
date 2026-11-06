"""A 2 Axis Controller's knob works like a pin inside its box: what hangs from it (bones that Follow Knob, layers
parented to it, points weighted to it) moves and turns with it; R on the knob turns it (keyed), R on the setup frame
turns the whole box, the knob running along it; it traces one motion path, at the knob; nothing connects to it
(converting lets connected children go, Ctrl P and Connected keep the offset, E grows a bone from it with one) and an
IK chain stops at it. Children in files from before ride the box, as they did.

Run with:  OpenAnimate --background --python tests/test_controller_knob.py
"""

import json
import math
import pathlib
import tempfile
import zipfile

import oanim

ops, doc, layers, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm
ops.wm.read_homefile()
doc.auto_key = True
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active


def add(name, head, tail, parent=""):
    ops.armature.select_none()
    assert ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]),
                                 tail_y=str(tail[1]), parent=parent) == {'FINISHED'}


def bone(name):
    doc.frame_current = doc.frame_current
    return next(b for b in rig.bones if b["name"] == name)


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1])


add("root", (-1, -1), (0, 0))
add("stick", (0, 0), (1, 0), parent="root")
add("hand", (1, 0), (2, 0), parent="stick")
add("ride", (0, 1), (0.5, 1), parent="stick")
ops.armature.select_none()
rig.select_bone("hand")
rig.select_bone("stick", extend=True)
assert ops.armature.parent_set(type="CONNECTED") == {'FINISHED'}
assert bone("hand")["connected"]

# --- Converting: connected children let go where they are, and hang from the knob ------------------------------------
ops.armature.select_none()
rig.select_bone("stick")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
# Two sides with actions, so the box has room: Right 40 frames, Up 20.
ops.layer.new()
face = layers.active
ops.layer.move_to_group(group=str(rig.id))
face.add_curve([(8.0, 8.0), (9.0, 8.0)])
for name, frames in (("go_r", 40), ("go_u", 20)):
    assert ops.anim.action_new(name=name) == {'FINISHED'}
    doc.frame_current = frames
    face.set_point(0, (8.0, 9.0))
    doc.frame_current = frames + 1
    doc.action = ""
    doc.frame_current = 0
layers.active = rig
ops.armature.select_none()
rig.select_bone("stick")
for side, action in (("RIGHT", "go_r"), ("UP", "go_u")):
    assert ops.armature.controller_side(side=side, action=action) == {'FINISHED'}
hand = bone("hand")
assert not hand["connected"] and hand["follow_knob"], ("nothing connects to a knob", hand)
assert close(hand["head"], (1, 0)) and close(hand["tail"], (2, 0)), ("it stays put", hand)
assert ops.armature.bone_relations_set(bone="ride", follow_knob="0") == {'FINISHED'}
assert not bone("ride")["follow_knob"]

# A layer parented to the knob, and points weighted to it.
ops.layer.new()
moved = layers.active
ops.layer.move_to_group(group=str(rig.id))
moved.add_curve([(3.0, 3.0), (3.5, 3.0)])
assert ops.layer.parent_bone_set(bone="stick") == {'FINISHED'}
ops.layer.new()
bent = layers.active
ops.layer.move_to_group(group=str(rig.id))
bent.add_curve([(-3.0, -3.0), (-3.5, -3.0)])
ops.mesh.select_all()
layers.active = rig
rig.select_bone("stick")
layers.active = bent
assert ops.weight.assign(weight="1") == {'FINISHED'}
layers.active = rig

rest_knob = bone("stick")["knob_head"]
rest_moved = tuple(moved.shown()[0][0])
rest_bent = tuple(bent.shown()[0][0])

# --- Posed: the knob carries what hangs from it; the box and what rides it stay ---------------------------------------
doc.frame_current = 10
rig.bone_set("stick", knob=(10.0, 5.0))
doc.frame_current = 10
stick = bone("stick")
shift = sub(stick["knob_head"], rest_knob)
assert math.hypot(*shift) > 0.01, ("the knob moved", stick)
assert close(stick["head"], (0, 0)), "the box stays where it rests"
assert close(bone("hand")["head"], (1 + shift[0], shift[1])), ("a child follows the knob", bone("hand"), shift)
assert close(bone("ride")["head"], (0, 1)), ("Follow Knob off: it rides the box", bone("ride"))
assert close(sub(tuple(moved.shown()[0][0]), rest_moved), shift), "a layer parented to it follows the knob"
assert close(sub(tuple(bent.shown()[0][0]), rest_bent), shift), "weighted points follow the knob"

# R turns the knob, keyed, and what hangs from it turns about it.
rig.select_bone("stick")
before = bone("hand")["head"]
assert ops.armature.rotate(angle="90") == {'FINISHED'}
assert rig.keyframes('bones["stick"].angle'), "the knob's turn is keyed"
knob = bone("stick")["knob_head"]
arm = sub(before, knob)
assert close(bone("hand")["head"], (knob[0] - arm[1], knob[1] + arm[0])), ("the child turns about the knob", bone("hand"))
assert close(bone("ride")["head"], (0, 1)), "the box doesn't turn"

# --- One motion path, at the knob -------------------------------------------------------------------------------------
doc.motion_paths = True
ops.armature.select_none()
rig.select_bone("stick")
paths = wm.motion_paths()
assert [p["label"] for p in paths] == ["stick head"], ("one path, at the knob", [p["label"] for p in paths])
pts = paths[0]["points"]
assert close(pts[10 - paths[0]["first"]], rig.to_document(knob)), ("the path goes through the knob", pts[10 - paths[0]["first"]])
doc.motion_paths = False

# --- The setup frame: R turns the whole box, the knob running along it ------------------------------------------------
doc.frame_current = 0
ops.armature.select_none()
rig.select_bone("stick")
assert ops.armature.rotate(angle="90") == {'FINISHED'}
assert close(bone("stick")["controller_box_angle"], math.pi / 2), bone("stick")
doc.frame_current = 20
rig.bone_set("stick", knob=(10.0, 0.0))
doc.frame_current = 20
k = bone("stick")["knob_head"]
assert abs(k[0]) < 1e-4 and k[1] > 0.01, ("right along the box, now standing up", k)
doc.frame_current = 0

# --- Nothing connects to it -------------------------------------------------------------------------------------------
add("other", (4, 0), (5, 0))
ops.armature.select_none()
rig.select_bone("other")
rig.select_bone("stick", extend=True)
assert ops.armature.parent_set(type="CONNECTED") == {'FINISHED'}
assert not bone("other")["connected"] and bone("other")["follow_knob"], ("Ctrl P keeps the offset", bone("other"))
assert close(bone("other")["head"], (4, 0))
assert ops.armature.bone_relations_set(bone="other", connected="1") == {'CANCELLED'}, "nor the Bone tab's Connected"
names = {b["name"] for b in rig.bones}
ops.armature.select_none()
rig.select_bone("stick")
assert ops.armature.extrude() == {'FINISHED'}
grown = next(b for b in rig.bones if b["name"] not in names)
assert not grown["connected"] and close(grown["head"], (0, 0)), ("E grows from the knob, with an offset", grown)
ops.armature.select_none()
rig.select_bone(grown["name"])
assert ops.armature.bone_delete() == {'FINISHED'}

# An IK chain stops at the knob: it doesn't turn it.
add("goal", (-3.0, 4.0), (-2.8, 4.0))  # out of the hand's reach: a longer chain would turn more
doc.frame_current = 30
ops.armature.select_none()
rig.select_bone("hand")
root_tail = bone("root")["tail"]
turn = bone("stick")["knob_angle"]
assert ops.armature.constraint_add(type="IK", bone="hand") == {'FINISHED'}
rig.bone_constraint_set("hand", 0, target="goal", chain=0)  # as long a chain as there is
doc.frame_current = 31
doc.frame_current = 30
assert close(bone("stick")["knob_angle"], turn), "IK leaves the knob alone"
assert close(bone("root")["tail"], root_tail), ("and what it hangs from: the chain stops at it", bone("root"))
rig.bone_constraint_set("hand", 0, enabled=False)
doc.frame_current = 0

# --- Saved; children in files from before ride the box ---------------------------------------------------------------
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_knob_"))
path = str(folder / "knob.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers if l.type == "BONE")
assert bone("hand")["follow_knob"] and not bone("ride")["follow_knob"]
assert close(bone("stick")["controller_box_angle"], math.pi / 2)

old = str(folder / "old.oanim")
with zipfile.ZipFile(path) as zin, zipfile.ZipFile(old, "w") as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename.endswith(".json") and b"follow_knob" in data:
            text = data.decode("utf-8")
            text = text.replace('"follow_knob": true,', '').replace('"follow_knob":true,', '')
            text = text.replace(',"follow_knob":true', '').replace(', "follow_knob": true', '')
            assert "follow_knob" not in text
            data = text.encode("utf-8")
        zout.writestr(item, data)
assert ops.wm.open_mainfile(filepath=old, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers if l.type == "BONE")
assert not bone("hand")["follow_knob"], "from before: it rides the box"
ops.armature.select_none()
rig.select_bone("hand")
rig.select_bone("stick", extend=True)
assert ops.armature.parent_set() == {'FINISHED'}
assert bone("hand")["follow_knob"], "parented again: it hangs from the knob"

print("Controller knob tests passed")
