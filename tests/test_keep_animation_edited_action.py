"""Keep Animation looks through every action a drawing plays in and comes back: it must come back to the action being
edited (whoever's it is, even when another bone layer has one of the same name) and leave its keys as they were, frame
0's too. A Keep Animation edit that does nothing (CANCELLED) changes nothing at all.

Run with:  OpenAnimate --background --python tests/test_keep_animation_edited_action.py
"""

import json
import os
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
tmp = tempfile.mkdtemp()
doc.frame_current = 0


def bone_rig(name, bone, art_points):
    assert ops.layer.new_bone() == {'FINISHED'}
    rig = layers.active
    rig.name = name
    ops.armature.bone_add(name=bone, head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
    ops.layer.new()
    art = layers.active
    assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
    art = next(l for l in layers.all() if l.id == art.id)
    art.add_curve(art_points)
    return rig, art


rig_a, art = bone_rig("A", "a", [(0.2, 0.2), (0.5, 0.5), (0.8, 0.8)])
rig_b, mark = bone_rig("B", "b", [(0.2, -0.2), (0.5, -0.5), (0.8, -0.8)])
rig_a = next(l for l in layers.all() if l.id == rig_a.id)
rig_b = next(l for l in layers.all() if l.id == rig_b.id)


def angle(rig, name):
    rig = next(l for l in layers.all() if l.id == rig.id)
    return next(b for b in rig.bones if b["name"] == name)["angle"]


def snap():
    p = os.path.join(tmp, "s.oanim")
    assert ops.wm.save_copy(filepath=p) == {'FINISHED'}
    return json.loads(zipfile.ZipFile(p).read("document.json"))["document"]


def layer_json(d, lid):
    stack = list(d["layers"])
    while stack:
        l = stack.pop()
        if l.get("id") == lid:
            return l
        stack += l.get("children", [])


# On the mainline B's bone turns the other way on frame 10. Both bone layers have an action called "wave", each turning
# its own bone on frame 10.
doc.frame_current = 10
rig_b.bone_set("b", angle=-0.2)
doc.frame_current = 11
for rig, bone, turn in ((rig_a, "a", 0.3), (rig_b, "b", 0.5)):
    doc.frame_current = 0
    layers.active = rig
    assert ops.anim.action_new(name="wave") == {'FINISHED'} and doc.action == "wave"
    doc.frame_current = 10
    rig.bone_set(bone, angle=turn)
    doc.frame_current = 11
doc.frame_current = 0
rest_b = angle(rig_b, "b")

# Editing B's wave, on frame 0: keys put on B's drawing there alone (nothing keyed after frame 0).
doc.frame_current = 0
layers.active = mark
ops.mesh.select_none()
mark.select_points([1, 2])
assert ops.anim.keyframe_insert() == {'FINISHED'}
doc.frame_current = 10
assert abs(angle(rig_b, "b") - rest_b - 0.5) < 1e-4, "B's wave is being edited"
doc.frame_current = 0
keyed = layer_json(snap(), mark.id)["drawing"]["curves"][0]["points"][1]
assert "curvature_keys" in keyed, keyed

# Remove Weights from A's drawing, whose points have none: nothing to do, so nothing changes.
layers.active = art
ops.mesh.select_none()
art.select_points([0, 1])
before = snap()
r = ops.weight.remove(keep_animation="1")
assert r == {'CANCELLED'}, r
assert doc.action == "wave"
doc.frame_current = 10
assert abs(angle(rig_b, "b") - rest_b - 0.5) < 1e-4, "still in B's wave, not A's: " + str(angle(rig_b, "b"))
doc.frame_current = 0
after = snap()
assert layer_json(after, mark.id) == layer_json(before, mark.id), "B's drawing kept its frame 0 keys"
assert after == before, "a cancelled Keep Animation edit changed nothing"

# Parent to Bone keeping the animation, refused (b is B's bone, not A's): nothing changes either.
r = ops.layer.parent_bone_set(bone="b", keep_animation="1")
assert r == {'CANCELLED'}, r
doc.frame_current = 10
assert abs(angle(rig_b, "b") - rest_b - 0.5) < 1e-4, "still in B's wave after Parent to Bone"
doc.frame_current = 0
assert snap() == before, "a cancelled Parent to Bone changed nothing"

# One that does something (A's drawing given to bone a): B's action and its frame 0 keys stay as they were too.
rig_a = next(l for l in layers.all() if l.id == rig_a.id)
rig_a.select_bone("a")
layers.active = art
art.select_points([0, 1, 2])
r = ops.weight.assign(weight="1.0", keep_animation="1")
assert r == {'FINISHED'}, r
assert doc.action == "wave"
doc.frame_current = 10
assert abs(angle(rig_b, "b") - rest_b - 0.5) < 1e-4, "still in B's wave after a Keep Animation edit"
doc.frame_current = 0
assert layer_json(snap(), mark.id) == layer_json(before, mark.id), "B's drawing kept its frame 0 keys"

print("test_keep_animation_edited_action: ok")
