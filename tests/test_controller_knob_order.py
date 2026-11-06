"""An independent 2 Axis Controller knob riding its Knob Parent plays the frames it is shown at: actions that move the
box or the Knob Parent (a head turn, and a fix of the head turn) play first, then the knob picks its frames where they
put it, so what plays matches where the knob sits in its box.

Run with:  OpenAnimate --background --python tests/test_controller_knob_order.py
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


def close(a, b, eps=1e-4):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def add(name, head, tail):
    ops.armature.select_none()
    assert ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]),
                                 tail_y=str(tail[1])) == {'FINISHED'}


add("eye", (0, 0), (0.1, 0))
add("center", (0, 0.5), (0.1, 0.5))
add("dial", (5, 5), (6, 5))
add("nod", (5, 7), (6, 7))
add("tilt", (5, 9), (6, 9))

# The eye: a controller whose Right action squashes a pupil parented to its knob.
ops.layer.new()
pupil = layers.active
ops.layer.move_to_group(group=str(rig.id))
pupil.add_curve([(0.0, 0.0), (0.2, 0.0), (0.2, 0.2)])
assert ops.layer.parent_bone_set(bone="eye") == {'FINISHED'}
assert ops.anim.action_new(name="eye_r") == {'FINISHED'}
doc.frame_current = 40
pupil.set_point(2, (0.4, 0.3))
go(40)
doc.action = ""
go(0)
layers.active = rig
ops.armature.select_none()
rig.select_bone("eye")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="eye_r") == {'FINISHED'}
assert ops.armature.bone_relations_set(bone="eye", knob_parent="center") == {'FINISHED'}
size = bone("eye")["controller_frame_size"]

# A head turn: turning the dial moves the Knob Parent 10 frames right; turning both dial and nod, a fix moves it 10 more.
ops.armature.select_none()
rig.select_bone("dial")
assert ops.anim.action_new(name="dial", action_bone="1") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("dial", angle=1.0)
rig.bone_set("center", position=(10 * size, 0.0))
go(10)
assert ops.anim.action_set(name="") == {'FINISHED'}
go(0)
ops.armature.select_none()
rig.select_bone("nod")
assert ops.anim.action_new(name="nod", action_bone="1") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("nod", angle=1.0)
go(10)
assert ops.anim.action_set(name="") == {'FINISHED'}
go(0)
ops.armature.select_none()
rig.select_bone("tilt")
assert ops.anim.action_new(name="tilt", action_bone="1") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("tilt", angle=1.0)
go(10)
assert ops.anim.action_set(name="") == {'FINISHED'}
go(0)


def shape():
    """The pupil as the knob carries it, less where the knob is: what the eye's actions made of it."""
    k = bone("eye")["knob_head"]
    return [(p[0] - k[0], p[1] - k[1]) for p in pupil.shown()[0]]


def matches_shown(frame, why):
    """On `frame`, the pupil is what the eye plays where its knob is shown (the same knob place with nothing moved)."""
    go(frame)
    shown = bone("eye")["knob_shown"]
    now = shape()
    doc.frame_current = 1
    rig.bone_set("eye", knob=tuple(shown))
    go(1)
    there = shape()
    rig.keyframe_delete('bones["eye"].position', 1)
    assert max(math.hypot(a[0] - b[0], a[1] - b[1]) for a, b in zip(now, there)) < 1e-5, (why, shown, now, there)
    return shown


# Keyed at rest on frame 20 and turned on 30: before 20 the head rests, where the knob is compared.
for name in ("dial", "nod", "tilt"):
    rig.keyframe_insert(f'bones["{name}"].angle', 20)
doc.frame_current = 30
rig.bone_set("dial", angle=1.0)
go(30)
shown = matches_shown(30, "a head turn plays before the knob picks its frames")
assert close(shown, (10, 0), 1e-2), ("the head turn moved the knob 10 frames", shown)

# The fix of the head turn plays before the knob picks its frames too. The knob keyed 10 frames back, so the head turn
# brings it to rest: the fix is the head's alone (dial and nod).
doc.frame_current = 30
rig.bone_set("eye", knob=(-10.0, 0.0))
rig.bone_set("nod", angle=1.0)
go(30)
assert bone("eye")["knob_shown"][0] < 1e-3, bone("eye")["knob_shown"]
assert ops.anim.fix_action_new() == {'FINISHED'}
assert rig.fix_actions[0]["drivers"] == ["dial", "nod"], rig.fix_actions[0]["drivers"]
rig.bone_set("center", position=(20 * size, 0.0))
go(rig.fix_actions[0]["poses"][0]["frame"])
assert ops.anim.action_set(name="") == {'FINISHED'}
shown = matches_shown(30, "the head turn's fix plays before the knob picks its frames")
assert close(shown, (10, 0), 1e-2), ("the head turn and its fix moved the knob 20 frames", shown)

# A fix moving the Knob Parent when neither of its actions does (nod and tilt move nothing): it plays first too.
doc.frame_current = 60
rig.bone_set("dial", angle=0.0)
rig.bone_set("eye", knob=(0.0, 0.0))
rig.bone_set("tilt", angle=1.0)
go(60)
assert bone("eye")["knob_shown"][0] < 1e-3, bone("eye")["knob_shown"]
assert ops.anim.fix_action_new() == {'FINISHED'}
assert doc.action == "nod × tilt", doc.action
rig.bone_set("center", position=(10 * size, 0.0))
go(rig.fix_actions[1]["poses"][0]["frame"])
assert ops.anim.action_set(name="") == {'FINISHED'}
# The knob already playing its action (5 frames in), so it has to wait for the fix rather than start after it.
doc.frame_current = 60
rig.bone_set("eye", knob=(5.0, 0.0))
shown = matches_shown(60, "a fix moving the Knob Parent plays before the knob picks its frames")
assert close(shown, (15, 0), 1e-2), ("the fix moved the knob 10 frames", shown)

print("Knob order tests passed")
