"""Keep Animation in the weight tools: a weight change on the setup frame (Assign, Remove, Normalize,
Automatic Weights, a brush stroke) leaves the drawing where it shows on every keyed frame, the points the
change bends differently keyed back there. Off, the poses follow the new weights.

Run with:  OpenAnimate --background --python tests/test_weight_keep_animation.py
"""

import oanim

layers = oanim.data.layers
doc = oanim.data.document
ops = oanim.ops
doc.frame_current = 0


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="leg", head_x="0", head_y="-1", tail_x="1", tail_y="-1", parent="")
ops.layer.new()
art = layers.active
art.add_curve([(0.2, 0.1), (0.6, 0.2), (0.9, -0.2), (0.7, -0.9)])
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
art = next(l for l in layers.all() if l.id == art.id)
layers.active = art
assert ops.weight.automatic() == {'FINISHED'}

# Posed on frame 10 (the bones), and point 2 keyed on frame 6.
doc.frame_current = 10
rig.bone_set("arm", angle=0.8)
rig.bone_set("leg", angle=-0.5, position=(0.2, -1.1))
layers.active = rig
ops.armature.select_all()
ops.anim.keyframe_insert()
doc.frame_current = 6
layers.active = art
art.select_points(range(art.point_count), False)
art.select_points([2])
ops.mesh.translate(move_x="0.1", move_y="0.05")
doc.frame_current = 0


def shown():
    out = {}
    for f in (0, 6, 10, 20):
        doc.frame_current = f
        out[f] = art.evaluated()[0]
    doc.frame_current = 0
    return out


was = shown()
layers.active = art
ops.wm.tool_weight_select()
rig.select_bone("leg")
art.select_points(range(art.point_count))


def same(now):
    return all(close(a, b) for f in was for a, b in zip(now[f], was[f]))


# Assign: every point all to the leg, keeping the animation: nothing moves on any frame.
assert ops.weight.assign(weight="1", keep_animation="1") == {'FINISHED'}
assert art.bone_weights("leg") == [1.0] * art.point_count, art.bone_weights("leg")
now = shown()
assert same(now), ("the drawing stays on every frame", now, was)
ops.ed.undo()
assert same(shown())

# Off: the poses follow the new weights.
assert ops.weight.assign(weight="1", keep_animation="0") == {'FINISHED'}
now = shown()
assert close(now[0][0], was[0][0]), "the setup frame is the same either way"
assert not all(close(a, b) for a, b in zip(now[10], was[10])), "off: frame 10 follows the leg"
ops.ed.undo()

# Remove (the points out of the arm) and Automatic Weights too.
rig.select_bone("arm")
assert any(w > 0 for w in art.bone_weights("arm")), art.bone_weights("arm")
assert ops.weight.remove(keep_animation="1") == {'FINISHED'}
assert same(shown())
assert ops.weight.automatic(keep_animation="1") == {'FINISHED'}
assert same(shown())

# A tool that then changes nothing leaves an edit not keyed yet (auto-key off: it shows till the frame changes) as it
# is: its look through every action is on a copy.
doc.auto_key = False
layers.active = rig
ops.armature.select_none()
ops.armature.bone_add(name="spare", head_x="5", head_y="5", tail_x="6", tail_y="5", parent="")
doc.frame_current = 10


def arm_angle():
    return next(b for b in rig.bones if b["name"] == "arm")["angle"]


ops.armature.select_none()
rig.select_bone("arm")
assert ops.armature.rotate(angle="30") == {'FINISHED'}
turned = arm_angle()
layers.active = art
art.select_points(range(art.point_count))
rig.select_bone("spare")
assert ops.weight.remove(keep_animation="1") == {'CANCELLED'}, "nothing weighted to spare"
assert abs(arm_angle() - turned) < 1e-5, ("the turn not keyed stays", turned, arm_angle())
doc.auto_key = True

print("weight_keep_animation: ok")
