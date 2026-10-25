"""Keep Animation on a weight change covers every action the drawing plays in, not just the one being
edited: changed from the mainline, an action's keyed frames still show the drawing where they did.

Run with:  OpenAnimate --background --python tests/test_weights_keep_actions.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="b", head_x="0", head_y="1", tail_x="1", tail_y="1", parent="")
ops.layer.new()
art = layers.active
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
art = next(l for l in layers.all() if l.id == art.id)
art.add_curve([(0.2, 0.2), (0.5, 0.5), (0.8, 0.8)])
layers.active = art
ops.mesh.select_all()
rig.select_bone("a")
rig.select_bone("b", extend=True)
assert ops.weight.automatic() == {'FINISHED'}

# Mainline: a turned on frame 10. Action "wave": b turned on 10, a on 20.
doc.frame_current = 10
rig.bone_set("a", angle=0.7)
doc.frame_current = 11
doc.frame_current = 0
layers.active = rig
assert ops.anim.action_new(name="wave") == {'FINISHED'} and doc.action == "wave"
doc.frame_current = 10
rig.bone_set("b", angle=-0.8)
doc.frame_current = 20
rig.bone_set("a", angle=0.4)
doc.frame_current = 21


def shown(action, frame):
    if doc.action != action:
        assert ops.anim.action_set(name=action) == {'FINISHED'}
    doc.frame_current = frame
    return art.shown()[0]


looks = {(a, f): shown(a, f) for a, f in (("", 10), ("wave", 10), ("wave", 20))}
assert not all(abs(p[0] - q[0]) < 1e-4 for p, q in zip(looks[("wave", 10)], looks[("", 10)])), "the actions differ"

# From the mainline, on the setup frame: every point given wholly to a, keeping the animation.
shown("", 0)
layers.active = art
for l in layers.all():
    l.select = l.id == art.id
rig.select_bone("a")
assert ops.mesh.select_all() == {'FINISHED'}
assert ops.weight.assign(weight="1.0", keep_animation="1") == {'FINISHED'}
assert all(set(w) == {"a"} or all(v == 0 for b, v in w.items() if b != "a") for w in art.weights), art.weights

for (action, frame), before in looks.items():
    now = shown(action, frame)
    worst = max(math.hypot(p[0] - q[0], p[1] - q[1]) for p, q in zip(now, before))
    assert worst < 1e-4, ("kept in %s frame %d" % (action or "the mainline", frame), worst)
assert doc.action == "wave"
ops.anim.action_set(name="")

# The Weight Paint brush too: a dab with Keep Animation from inside the action keeps the mainline as well.
shown("wave", 0)
looks = {(a, f): shown(a, f) for a, f in (("", 10), ("wave", 10), ("wave", 20))}
shown("wave", 0)
assert art.weight_paint((0.5, 0.5), 0.6, "b", weight=1.0, keep_animation=True) > 0
for (action, frame), before in looks.items():
    now = shown(action, frame)
    worst = max(math.hypot(p[0] - q[0], p[1] - q[1]) for p, q in zip(now, before))
    assert worst < 1e-4, ("painted: kept in %s frame %d" % (action or "the mainline", frame), worst)
ops.anim.action_set(name="")

print("weights keep actions: ok")
