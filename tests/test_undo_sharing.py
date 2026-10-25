"""Undo steps share the layers that didn't change (each step stores changed layers only): undo and redo must
still give back exactly each state, through nested groups, edits to one layer, moves between groups and
deletions, and the X-mirror pass that reads the step before.

Run with:  OpenAnimate --background --python tests/test_undo_sharing.py
"""

import oanim

layers = oanim.data.layers
ed = oanim.ops.ed


def snapshot():
    """Every layer's name, parent, type and point positions, in tree order."""
    out = []
    for l in layers.all():
        pts = [tuple(round(c, 5) for c in p[:2]) for p in l.points] if l.type == "VECTOR" else []
        out.append((l.name, l.parent.name if l.parent else "", l.type, tuple(pts)))
    return out


# A group holding two drawings, another drawing beside it.
a = layers.active
a.name = "A"
a.add_curve([(0.0, 0.0), (1.0, 0.0)])
oanim.ops.layer.new()
b = layers.active
b.name = "B"
b.add_curve([(0.0, 1.0), (1.0, 1.0)])
layers.active = a
oanim.ops.layer.group()
group = layers.active
group.name = "G"
layers.active = b
assert oanim.ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
oanim.ops.layer.new()
c = layers.active
c.name = "C"
c.add_curve([(0.0, 2.0), (1.0, 2.0), (2.0, 2.0)])
# A's points selected (its own undo step), then the states to come back to.
layers.active = a
oanim.ops.mesh.select_all()
states = [snapshot()]

# Edit one drawing inside the group: the others are shared with the step before.
assert oanim.ops.mesh.translate(move_x="0.5", move_y="0") == {'FINISHED'}
states.append(snapshot())
assert states[-1] != states[-2]

# Move C into the group.
layers.active = c
assert oanim.ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
states.append(snapshot())

# Delete B.
layers.active = b
assert oanim.ops.layer.delete() == {'FINISHED'}
states.append(snapshot())
assert not any(name == "B" for name, *_ in states[-1])

# Back through every step, then forward again: each state exactly as it was.
for i in range(len(states) - 1, 0, -1):
    assert snapshot() == states[i], (i, snapshot(), states[i])
    ed.undo()
assert snapshot() == states[0], (snapshot(), states[0])
for i in range(1, len(states)):
    ed.redo()
    assert snapshot() == states[i], (i, snapshot(), states[i])

# An edit after undoing drops the redo steps; undo still gives back the state before it.
ed.undo()
target = next(l for l in layers.all() if l.type == "VECTOR")
layers.active = target
target.select_points(range(len(target.points)))
before = snapshot()
oanim.ops.mesh.translate(move_x="0", move_y="0.25")
assert snapshot() != before
ed.undo()
assert snapshot() == before, (snapshot(), before)

# X mirror editing reads the step before as a whole document: a new layer in a sided group gets its twin,
# and undo takes both away.
oanim.ops.layer.new_group()
arm_r = layers.active
arm_r.name = "Arm_R"
oanim.ops.layer.new_group()
arm_l = layers.active
arm_l.name = "Arm_L"
oanim.ops.layer.new()
hand = layers.active
hand.name = "Hand_L"
oanim.ops.layer.move_to_group(group=str(arm_l.id))
layers.active = next(l for l in layers.all() if l.name == "Hand_L")
before = snapshot()
oanim.wm.set_mirror_editing(x=True)
assert oanim.ops.layer.new() == {'FINISHED'}
assert len(snapshot()) == len(before) + 2, (snapshot(), before)
ed.undo()
assert snapshot() == before, (snapshot(), before)
oanim.wm.set_mirror_editing(x=False)
print("undo sharing: ok")
