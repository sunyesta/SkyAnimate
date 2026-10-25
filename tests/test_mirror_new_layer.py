"""X mirror editing for new layers: a layer (or group) made inside a sided group (Arm_L) while X is on
makes its twin in the partner group (Arm_R), at the same place, the two named for their sides (Layer 3_L,
Layer 3_R) so they pair; reordering a sided group's layers reorders the partner's to match. Off, or in a
group without a partner, just the one.

Run with:  OpenAnimate --background --python tests/test_mirror_new_layer.py
"""

import oanim

layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def child_names(group):
    return [l.name for l in group.children]


def group(name):
    assert ops.layer.new_group() == {'FINISHED'}
    g = layers.active
    g.name = name
    return g


arm_r = group("Arm_R")
arm_l = group("Arm_L")
ops.layer.new()
hand = layers.active
hand.name = "Hand_L"
assert ops.layer.move_to_group(group=str(arm_l.id)) == {'FINISHED'}
hand = next(l for l in layers.all() if l.name == "Hand_L")
layers.active = hand

# On: one in each, named for their sides; the new one in Arm_L is active.
wm.set_mirror_editing(x=True)
assert ops.layer.new() == {'FINISHED'}
made = layers.active
assert made.name.endswith("_L") and made.parent.id == arm_l.id, (made.name, made.parent)
made_name = made.name
twin = made_name[:-2] + "_R"
assert child_names(arm_r) == [twin], child_names(arm_r)
assert child_names(arm_l)[0] == made.name, child_names(arm_l)

# A group too.
assert ops.layer.new_group() == {'FINISHED'}
g = layers.active
assert g.name.endswith("_L") and g.name[:-2] + "_R" in child_names(arm_r), (g.name, child_names(arm_r))
ops.ed.undo()
ops.ed.undo()
assert child_names(arm_r) == [] and made_name not in child_names(arm_l)

# Off: just the one, as before.
wm.set_mirror_editing(x=False)
layers.active = hand
assert ops.layer.new() == {'FINISHED'}
assert child_names(arm_r) == [] and not layers.active.name.endswith("_L")
ops.ed.undo()

# Order: two twins made, then the lower one raised in Arm_L: Arm_R's follow.
wm.set_mirror_editing(x=True)
layers.active = hand
assert ops.layer.new() == {'FINISHED'}
first = layers.active.name
assert ops.layer.new() == {'FINISHED'}
second = layers.active.name
assert child_names(arm_l)[:2] == [second, first], child_names(arm_l)
assert child_names(arm_r) == [second[:-2] + "_R", first[:-2] + "_R"], child_names(arm_r)
layers.active = next(l for l in layers.all() if l.name == first)
assert ops.layer.move_up() == {'FINISHED'}
assert child_names(arm_l)[:2] == [first, second], child_names(arm_l)
assert child_names(arm_r) == [first[:-2] + "_R", second[:-2] + "_R"], child_names(arm_r)
# Off: only the one side.
wm.set_mirror_editing(x=False)
assert ops.layer.move_down() == {'FINISHED'}
assert child_names(arm_r) == [first[:-2] + "_R", second[:-2] + "_R"], child_names(arm_r)
wm.set_mirror_editing(x=True)

# Renaming: Ear_L's partner takes the other side of the new name (on the step the rename makes).
def push():
    layers.active = hand
    assert ops.mesh.select_none() == {'FINISHED'}


layer_l = next(l for l in layers.all() if l.name == first)
layer_l.name = "Lobe_L"
push()
assert "Lobe_R" in child_names(arm_r) and first[:-2] + "_R" not in child_names(arm_r), child_names(arm_r)
arm_l.name = "Leg.L"
push()
assert arm_r.name == "Leg.R", arm_r.name
# A new name without a side leaves the partner be.
layer_l.name = "Lobe"
push()
assert "Lobe_R" in child_names(arm_r)
# Off: just the one.
wm.set_mirror_editing(x=False)
layer_l.name = "Ear_L"
push()
wm.set_mirror_editing(x=True)
assert "Lobe_R" in child_names(arm_r)

# A group without a side, or without its partner: just the one.
wm.set_mirror_editing(x=True)
body = group("Body")
ops.layer.new()
inner = layers.active
assert ops.layer.move_to_group(group=str(body.id)) == {'FINISHED'}
inner = next(l for l in layers.all() if l.id == inner.id)
layers.active = inner
count = len(layers.all())
assert ops.layer.new() == {'FINISHED'}
assert len(layers.all()) == count + 1

print("mirror_new_layer: ok")
