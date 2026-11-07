"""A frame shows the same whatever frame was shown before it. A 2 Axis Controller with Look At (in its Eye Shape)
plays its actions from where the look puts its knob, and that look is worked out after action bone actions are played:
they are played once more from where it went, else they played from where the knob was on the frame shown before.

Run with:  OpenAnimate --background --python tests/test_frame_visit_order.py
"""

import math

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers
ops.wm.read_homefile()
doc.auto_key = True
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


def go(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


assert ops.armature.bone_add(name="eye", head_x="0", head_y="0", tail_x="0.1", tail_y="0") == {'FINISHED'}
ops.armature.select_none()
assert ops.armature.bone_add(name="look", head_x="0", head_y="0", tail_x="0", tail_y="0.1") == {'FINISHED'}
ops.armature.select_none()
ops.layer.new()
pupil = layers.active
ops.layer.move_to_group(group=str(rig.id))
pupil.add_curve([(0.0, 0.0), (0.2, 0.0)])
assert ops.layer.parent_bone_set(bone="eye") == {'FINISHED'}
for name, to in (("eye_r", (0.3, 0.1)), ("eye_l", (0.1, -0.1))):
    layers.active = pupil
    assert ops.anim.action_new(name=name) == {'FINISHED'}
    doc.frame_current = 40
    pupil.set_point(1, to)
    go(40)
    doc.action = ""
    go(0)
# The eye's white, its Eye Shape.
ops.layer.new()
white = layers.active
ops.layer.move_to_group(group=str(rig.id))
white.add_curve([(0.4 * math.cos(2 * math.pi * k / 16), 0.25 * math.sin(2 * math.pi * k / 16)) for k in range(16)],
                closed=True, fill=True)
layers.active = rig
ops.armature.select_none()
rig.select_bone("eye")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="eye_r") == {'FINISHED'}
assert ops.armature.controller_side(side="LEFT", action="eye_l") == {'FINISHED'}
assert ops.armature.constraint_add(type="LOOK_AT", bone="eye") == {'FINISHED'}
rig.bone_constraint_set("eye", 0, target="look", shape_layer=white.name, fit="WHOLE")

# The look goes right, then left.
for frame, x in ((10, 0.6), (20, -0.5), (30, 0.2)):
    doc.frame_current = frame
    rig.bone_set("look", position=(x, 0.0))
    go(frame)
doc.frame_current = 31


def pose():
    return [tuple(p[:2]) for p in pupil.points], tuple(bone("eye")["knob_shown"])


for frame in (15, 25):
    seen = {}
    for before in (frame - 1, frame + 1, 0, 3, 10, 20, 30, 40, frame):
        doc.frame_current = before
        doc.frame_current = frame
        seen[before] = pose()
    first = seen[frame - 1]
    for before, got in seen.items():
        worst = max(math.dist(p, q) for p, q in zip(got[0], first[0]))
        assert worst < 1e-5, (f"frame {frame} reached from {before} shows the pupil {worst:.5f} off", got, first)
        assert math.dist(got[1], first[1]) < 1e-5, (frame, before, got[1], first[1])

print("test_frame_visit_order: all passed")
