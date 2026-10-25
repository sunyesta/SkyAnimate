"""Animated draw order: each layer's Draw Order (Normal, In Front Of a layer, Behind one), keyed on
frames other than 0 and the setup order on frame 0; rules across groups and bone layers; a rule on a
group moves the whole group; a faded group moves as one; Bring Forward / Send Backward step past the
next layer that overlaps; Bring to Front / Send to Back; back to Normal where the Layers editor puts it;
missing targets; saving.

Run with:  OpenAnimate --background --python tests/test_draw_order.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers


def find(name):
    found = []

    def walk(ls):
        for l in ls:
            if l.name == name:
                found.append(l)
            walk(l.children)

    walk(list(layers))
    return found[0]


def square(layer, x0, x1):
    layer.add_curve([(x0, 0.0), (x1, 0.0), (x1, 1.0), (x0, 1.0)], closed=True, fill=True)


def new_layer(name, x0=0.0, x1=1.0, group=None):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    square(l, x0, x1)
    if group is not None:
        oanim.ops.layer.move_to_group(group=str(group.id))
    return l


def order():
    return oanim.data.draw_order


# --- A rule keyed on frame 10; frame 0 keeps the setup order ------------------------------------------
back = layers.active
back.name = "Back"
square(back, 0.0, 1.0)
torso = new_layer("Torso")
hand = new_layer("Hand")
assert order() == ["Back", "Torso", "Hand"], order()

layers.active = hand
doc.frame_current = 10
assert oanim.ops.layer.draw_order_set(mode="BEHIND", target="Back") == {'FINISHED'}
assert hand.draw_order == ("BEHIND", "Back")
assert order() == ["Hand", "Back", "Torso"]
assert [k["frame"] for k in hand.keyframes("draw_order")] == [0, 10], "keyed, the setup keeps Normal"
assert all(k["interp"] == "STEP" for k in hand.keyframes("draw_order")), "draw order keys hold"
assert oanim.wm.undo_history()[0][-1] == "Hand Behind Back", "the undo step says what happened"
doc.frame_current = 0
assert order() == ["Back", "Torso", "Hand"] and hand.draw_order == ("NORMAL", "")
doc.frame_current = 30
assert order() == ["Hand", "Back", "Torso"], "held after the last key"

# Bring Forward from behind Back: in front of Back (the next one up that it overlaps).
assert oanim.ops.layer.draw_order_forward() == {'FINISHED'}
assert hand.draw_order == ("FRONT", "Back") and order() == ["Back", "Hand", "Torso"]
assert [k["frame"] for k in hand.keyframes("draw_order")] == [0, 10, 30]
# Once more: past Torso, where the Layers editor has it anyway: Normal, no rule needed.
assert oanim.ops.layer.draw_order_forward() == {'FINISHED'}
assert hand.draw_order == ("NORMAL", "") and order() == ["Back", "Torso", "Hand"]
assert oanim.ops.layer.draw_order_forward() == {'CANCELLED'}, "already in front of everything"

# Ctrl [ and Ctrl ], as Photoshop and Figma arrange.
assert oanim.wm.event_simulate("LEFT_BRACKET", ctrl=True, area="Viewport")
assert hand.draw_order[0] == "BEHIND"
assert oanim.wm.event_simulate("RIGHT_BRACKET", ctrl=True, area="Viewport")
assert hand.draw_order == ("NORMAL", "")

# Send to Back and Bring to Front.
assert oanim.ops.layer.draw_order_back() == {'FINISHED'}
assert order()[0] == "Hand"
assert oanim.ops.layer.draw_order_front() == {'FINISHED'}
assert order()[-1] == "Hand"

# --- Setup order on frame 0: no key ----------------------------------------------------------------------
layers.active = torso
doc.frame_current = 0
oanim.ops.layer.draw_order_set(mode="BEHIND", target="Back")
assert torso.keyframes("draw_order") == [] and torso.draw_order == ("BEHIND", "Back")
assert order()[0] == "Torso"
doc.frame_current = 50
assert order()[0] == "Torso", "the setup order holds everywhere"
doc.frame_current = 0
oanim.ops.layer.draw_order_set(mode="NORMAL")
assert torso.draw_order == ("NORMAL", "")

# --- Step past the next layer that overlaps (so each press shows) ----------------------------------------
doc.frame_current = 0
far = new_layer("Far", 5.0, 6.0)  # above everything, overlapping nothing
low = new_layer("Low", 0.2, 0.8)
layers.active = low
oanim.ops.layer.draw_order_back()
assert order()[0] == "Low"
o = order()
assert oanim.ops.layer.draw_order_forward() == {'FINISHED'}
assert low.draw_order == ("FRONT", o[1]), "one step: past the next one up, which overlaps it"
# Above everything that overlaps it, the next step skips Far (which doesn't) only when nothing overlaps.
layers.active = low
for _ in range(10):
    if oanim.ops.layer.draw_order_forward() != {'FINISHED'}:
        break
assert order()[-1] == "Low"
oanim.ops.layer.draw_order_set(mode="NORMAL")
# A layer above that doesn't overlap is skipped: the step goes past one you'd see it pass.
s1 = new_layer("S1", 10.0, 11.0)
s2 = new_layer("S2", 20.0, 21.0)
s3 = new_layer("S3", 10.5, 11.5)
o = order()
assert o.index("S2") == o.index("S1") + 1 and o.index("S3") == o.index("S2") + 1
layers.active = s1
oanim.ops.layer.draw_order_forward()
o = order()
assert s1.draw_order == ("FRONT", "S3") and o.index("S1") == o.index("S3") + 1

# --- Across bone layers, a group rule moves the whole group, a faded group moves as one --------------------
doc.frame_current = 0
oanim.ops.layer.new_bone()
rig_a = layers.active
rig_a.name = "Rig A"
oanim.ops.layer.new_group()
arm = layers.active
arm.name = "Arm"
oanim.ops.layer.move_to_group(group=str(rig_a.id))
sleeve = new_layer("Sleeve", group=arm)
paw = new_layer("Paw", group=arm)
layers.active = rig_a
oanim.ops.layer.new_bone()
rig_b = layers.active
rig_b.name = "Rig B"
chest = new_layer("Chest", group=rig_b)
o = order()
assert o.index("Chest") > o.index("Paw"), o

# A drawing inside one bone layer in front of a drawing inside another.
doc.frame_current = 5
layers.active = paw
assert oanim.ops.layer.draw_order_set(mode="FRONT", target="Chest") == {'FINISHED'}
o = order()
assert o.index("Paw") == o.index("Chest") + 1 and o.index("Sleeve") < o.index("Chest"), o

# Only the paw moved; the sleeve stays. Now the whole Arm group in front instead.
layers.active = paw
oanim.ops.layer.draw_order_set(mode="NORMAL")
layers.active = arm
assert oanim.ops.layer.draw_order_set(mode="FRONT", target="Chest") == {'FINISHED'}
o = order()
assert o.index("Sleeve") > o.index("Chest") and o.index("Paw") > o.index("Chest"), o
oanim.ops.layer.draw_order_set(mode="NORMAL")

# Faded below full opacity, Arm is drawn as one image: Paw's rule moves all of it.
arm.opacity = 0.5
layers.active = paw
oanim.ops.layer.draw_order_set(mode="FRONT", target="Chest")
o = order()
assert o.index("Sleeve") > o.index("Chest") and o.index("Paw") > o.index("Chest"), o
arm.opacity = 1.0
o = order()
assert o.index("Sleeve") < o.index("Chest") < o.index("Paw"), o

# The eyedropper in the viewport: a click lands on a drawing, and picks the group beside what moves (Arm
# for Chest, Rig A from Rig B's side), or the drawing itself when its own group holds what moves.
assert oanim.wm.draw_order_pick_target(paw, [chest]) == "Rig A"
assert oanim.wm.draw_order_pick_target(paw, [arm]) == "Paw", "inside the same bone layer and group as Arm"
assert oanim.wm.draw_order_pick_target(chest, [arm]) == "Rig B"
assert oanim.wm.draw_order_pick_target(paw, [sleeve]) == "Paw", "Sleeve's own group holds Paw"

# A layer can't go in front of its own group, or a group in front of what it holds.
layers.active = paw
assert oanim.ops.layer.draw_order_set(mode="FRONT", target="Arm") == {'CANCELLED'}
layers.active = arm
assert oanim.ops.layer.draw_order_set(mode="BEHIND", target="Paw") == {'CANCELLED'}

# --- Saved and opened again ------------------------------------------------------------------------------
doc.frame_current = 5
o5 = order()
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_draw_order_")) / "order.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 5
assert order() == o5
assert find("Paw").draw_order == ("FRONT", "Chest")
doc.frame_current = 0
assert find("Paw").draw_order == ("NORMAL", "")

# --- A rule naming a deleted layer draws as Normal -------------------------------------------------------
doc.frame_current = 5
layers.active = find("Chest")
oanim.ops.layer.delete()
assert find("Paw").draw_order == ("FRONT", "")
o = order()
assert o.index("Sleeve") < o.index("Paw"), "as the Layers editor has it"

# The timeline row and the menu operators exist.
assert oanim.ops.layer.draw_order_menu() == {'FINISHED'}
assert oanim.ops.layer.draw_order_pick(mode="FRONT") == {'FINISHED'}

print("test_draw_order: OK")
