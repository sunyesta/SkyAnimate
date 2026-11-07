"""Insert Keyframe on the setup frame, over a rig whose controller links a dial (an action bone action in one of its
sides): the dial's key is the knob's, there at once, in the step that made it. It stayed on the dial and went to the
knob only when another frame was shown, after the step was stored, so the timeline showed a key that went by itself.

Run with:  OpenAnimate --background --python tests/test_controller_dial_setup_key.py
"""

import math

import oanim

ops, doc, layers, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm
doc.auto_key = True

ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="turn", head_x="5.0", head_y="1.0", tail_x="6.0", tail_y="1.0")
rig.bone_set("turn", strength=0.0)
ops.layer.new()
face = layers.active
face.add_curve([(0.0, 0.0), (1.0, 0.0)])
ops.layer.move_to_group(group=str(rig.id))
layers.active = rig


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


# "turn" turns a quarter by frame 90 of its action bone action, taking the face's point along.
rig.select_bone("turn")
assert ops.anim.action_new(name="turn", action_bone="1") == {'FINISHED'}
doc.frame_current = 90
rig.bone_set("turn", angle=bone("turn")["setup_angle"] + math.pi / 2.0)
face.set_point(0, (1.0, 1.0))
doc.frame_current = 91
doc.action = ""
doc.frame_current = 0
ops.armature.select_none()
ops.armature.bone_add(name="joy", head_x="-3.0", head_y="3.0", tail_x="-2.5", tail_y="3.0")
rig.select_bone("joy")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="turn") == {'FINISHED'}
assert bone("turn")["controller"] == "joy"

doc.frame_current = 0
ops.armature.select_none()
steps = len(wm.undo_history()[0])
assert ops.anim.keyframe_insert() == {'FINISHED'}
assert len(wm.undo_history()[0]) == steps + 1
assert not rig.keyframes('bones["turn"].angle'), ("the dial's key is the knob's", rig.keyframes('bones["turn"].angle'))
assert [k["frame"] for k in rig.keyframes('bones["joy"].position')] == [0], rig.keyframes('bones["joy"].position')
# Nothing left to move when another frame is shown.
keyed = (rig.keyframes('bones["turn"].angle'), rig.keyframes('bones["joy"].position'))
doc.frame_current = 30
doc.frame_current = 0
assert (rig.keyframes('bones["turn"].angle'), rig.keyframes('bones["joy"].position')) == keyed

# A key put on the dial itself there (its keyframe button): the knob's too, in that same step (the knob, keyed there
# already, is left as it was).
assert ops.anim.keyframe_toggle(layer=str(rig.id), paths='bones["turn"].angle') == {'FINISHED'}
assert not rig.keyframes('bones["turn"].angle'), ("the dial's own key is the knob's", rig.keyframes('bones["turn"].angle'))

# On a posed frame the same: Insert Keyframe keys the knob where it is, never the dial from its own value (nothing,
# the knob's turn being an action's amount), which moved onto the knob took it back to rest.
doc.frame_current = 40
rig.bone_set("joy", knob=(45.0, 0.0))
doc.frame_current = 41
doc.frame_current = 40
assert ops.anim.keyframe_insert() == {'FINISHED'}
at = next(k for k in rig.keyframes('bones["joy"].position') if k["frame"] == 40)["value"]
assert math.dist(at, (45.0, 0.0)) < 1e-3, ("I keeps the knob's key", at)
assert not rig.keyframes('bones["turn"].angle'), rig.keyframes('bones["turn"].angle')
doc.frame_current = 41
doc.frame_current = 40
assert math.dist(bone("joy")["knob"], (45.0, 0.0)) < 1e-3, bone("joy")["knob"]

print("test_controller_dial_setup_key: all passed")
