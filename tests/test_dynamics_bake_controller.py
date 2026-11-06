"""Bake Dynamics on a 2 Axis Controller: the springs move its knob (pulled, as its constraints pull it, the box staying),
so the baked keys are the knob's own: it goes and turns where the simulation had it (it baked the box's pose as the
knob's keys, sending it elsewhere and spinning it). An angle spring turns the knob, its box resting (it turned the box,
which no key can).

Run with:  OpenAnimate --background --python tests/test_dynamics_bake_controller.py
"""

import math

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


def track():
    out = {}
    for f in range(1, 21):
        doc.frame_current = f
        b = bone("knob")
        out[f] = (b["knob_head"], b["knob_angle"], b["tail"])
    return out


for position, angle in ((True, False), (True, True), (False, True)):
    ops.wm.read_homefile()
    doc.auto_key = True
    doc.frame_end = 20
    doc.frame_current = 0
    ops.layer.new_bone()
    rig = layers.active
    assert ops.armature.bone_add(name="root", head_x="0", head_y="0", tail_x="0", tail_y="1", parent="") == {'FINISHED'}
    assert ops.armature.bone_add(name="knob", head_x="0", head_y="1", tail_x="0.2", tail_y="1", parent="root") == {'FINISHED'}
    ops.armature.select_none()
    rig.select_bone("knob")
    assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
    rig.bone_set("knob", contain_knob=False)
    # The knob is keyed across its box and back, and the root turns: the springs lag it behind.
    for f, knob, turn in ((1, (0.0, 0.0), 0.0), (6, (40.0, 20.0), 0.6), (12, (-30.0, -10.0), -0.4), (20, (0.0, 0.0), 0.0)):
        doc.frame_current = f
        rig.bone_set("knob", knob=knob)
        rig.bone_set("root", angle=bone("root")["setup_angle"] + turn)
    doc.frame_current = 0
    ops.armature.select_none()
    rig.select_bone("knob")
    assert ops.armature.constraint_add(type="DYNAMICS", bone="knob") == {'FINISHED'}
    rig.bone_constraint_set("knob", 0, position_spring={"enabled": position}, angle_spring={"enabled": angle})
    live = track()
    rig.bone_constraint_set("knob", 0, enabled=False)
    keyed = track()
    rig.bone_constraint_set("knob", 0, enabled=True)
    assert live == track()
    if position:
        assert max(math.dist(live[f][0], keyed[f][0]) for f in live) > 0.02, "the springs lag the knob"
    if angle:
        assert max(abs(live[f][1] - keyed[f][1]) for f in live) > 0.01, "the angle spring turns the knob"
    assert max(math.dist(live[f][2], keyed[f][2]) for f in live) < 1e-5, "its box rests"
    assert ops.armature.dynamics_bake() == {'FINISHED'}, (position, angle)
    assert not rig.bone_constraints("knob")[0]["enabled"]
    baked = track()
    for f in live:
        assert math.dist(live[f][0], baked[f][0]) < 2e-3, ("the knob where the simulation had it", position, angle, f,
                                                         live[f][0], baked[f][0])
        assert abs(live[f][1] - baked[f][1]) < 1e-3, ("the knob turned as it was", position, angle, f, live[f][1],
                                                      baked[f][1])

print("test_dynamics_bake_controller: all passed")
