"""Switch layers stage 3: sub bone groups and bone swaps. A switch of legs, each a bone layer with its
own bones inside the body's bone layer: the bone layers worked in together (a family) are the body and
the leg the switch shows. Bones stand in for each other across the switch's children (Auto-link by
Name, or one at a time); a pick made while working in one leg's bones selects their counterparts in
the next; Match Pose from Previous Drawing poses the incoming leg as the outgoing one lies, keyed.
Swaps are saved with the file; poses are never shared.

Run with:  OpenAnimate --background --python tests/test_switch_bones.py
"""

import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_switch_bones_"))
doc.frame_current = 0
doc.auto_key = True


def sync():
    doc.frame_current = doc.frame_current


def goto(frame):
    doc.frame_current = frame


def bone(layer, name):
    return next(b for b in layer.bones if b["name"] == name)


def find(name):
    return next(l for l in layers.all() if l.name == name)


oanim.ops.layer.new_bone()
body = layers.active
body.name = "Body"
oanim.ops.armature.bone_add(name="hip", head_x="0", head_y="0", tail_x="0", tail_y="-0.5", parent="")

legs = {}
for name, knee in (("Straight", 0.0), ("Bent", 0.6)):
    layers.active = body
    oanim.ops.layer.new_bone()
    leg = layers.active
    leg.name = name
    oanim.ops.armature.bone_add(name="thigh", head_x="0", head_y="-0.5", tail_x="0", tail_y="-1.5", parent="")
    end_x = math.sin(knee)
    oanim.ops.armature.bone_add(name="shin", head_x="0", head_y="-1.5", tail_x=str(end_x), tail_y=str(-1.5 - math.cos(knee)),
                                parent="thigh")
    legs[name] = leg
layers.active = legs["Straight"]
legs["Straight"].selected = True
legs["Bent"].selected = True
assert oanim.ops.layer.wrap_in_switch() == {'FINISHED'}
switch = layers.active
switch.name = "Leg"
assert oanim.ops.layer.move_to_group(group=str(body.id)) == {'FINISHED'}
sync()
assert [c.name for c in switch.children] == ["Bent", "Straight"] or [c.name for c in switch.children] == ["Straight", "Bent"]
default = switch.switch_default
other = "Bent" if default == "Straight" else "Straight"

# --- The family: the body and the leg shown, from either side ---------------------------------------
layers.active = body
sync()
assert body.bone_family == ["Body", default], body.bone_family
assert legs[default].bone_family == ["Body", default], legs[default].bone_family
# A leg the switch isn't showing isn't worked in with the body.
assert other not in body.bone_family
# Not a sub bone group: on its own again.
legs[default].sub_bone_group = False
assert legs[default].bone_family == [default] and body.bone_family == ["Body"], (legs[default].bone_family, body.bone_family)
legs[default].sub_bone_group = True

# --- Swaps: Auto-link by Name; one at a time; never a loop ------------------------------------------
layers.active = switch
assert oanim.ops.armature.swaps_auto_link() == {'FINISHED'}
hub, linked = default, other
assert bone(legs[linked], "thigh")["stands_in_for"] == (hub, "thigh"), bone(legs[linked], "thigh")
assert bone(legs[linked], "shin")["stands_in_for"] == (hub, "shin")
assert bone(legs[hub], "thigh")["stands_in_for"] is None
assert oanim.ops.armature.swaps_auto_link() == {'CANCELLED'}, "nothing left to link"
layers.active = legs[hub]
assert oanim.ops.armature.bone_swap_set(bone="thigh", layer=linked, target="thigh") == {'CANCELLED'}, "a loop"
layers.active = legs[linked]
assert oanim.ops.armature.bone_swap_set(bone="shin") == {'FINISHED'}
assert bone(legs[linked], "shin")["stands_in_for"] is None
assert oanim.ops.armature.bone_swap_set(bone="shin", layer=hub, target="shin") == {'FINISHED'}

# --- A pick made while working in a leg's bones selects their counterparts in the next --------------
goto(5)
layers.active = legs[hub]
legs[hub].select_bone("shin")
sync()
assert switch.switch_active == hub
assert oanim.ops.layer.switch_set(layer=switch.name, child=linked) == {'FINISHED'}
assert layers.active.name == linked, layers.active.name
assert [b["name"] for b in legs[linked].bones if b["selected"]] == ["shin"], legs[linked].bones
assert legs[linked].bone_family == ["Body", linked]

# --- Match Pose from Previous Drawing ---------------------------------------------------------------
# On frame 20 the hub leg is shown, posed; from frame 30 the other leg, matched to it.
goto(20)
assert oanim.ops.layer.switch_set(layer=switch.name, child=hub) == {'FINISHED'}
legs[hub].bone_set("thigh", angle=bone(legs[hub], "thigh")["angle"] + 0.4)
legs[hub].bone_set("shin", angle=bone(legs[hub], "shin")["angle"] - 0.3)
legs[hub].bone_set("thigh", position=(0.2, -0.4))
sync()
goto(30)
assert oanim.ops.layer.switch_set(layer=switch.name, child=linked) == {'FINISHED'}
layers.active = legs[linked]
assert oanim.ops.armature.match_pose_previous() == {'FINISHED'}


def world_angles(leg):
    thigh, shin = bone(leg, "thigh"), bone(leg, "shin")
    return thigh["angle"], thigh["angle"] + shin["angle"]


a, b = world_angles(legs[hub]), world_angles(legs[linked])
assert abs(a[0] - b[0]) < 1e-3 and abs(a[1] - b[1]) < 1e-3, (a, b)
pa, pb = bone(legs[hub], "thigh")["position"], bone(legs[linked], "thigh")["position"]
assert abs(pa[0] - pb[0]) < 1e-3 and abs(pa[1] - pb[1]) < 1e-3, (pa, pb)
assert 30 in [k["frame"] for k in legs[linked].keyframes('bones["thigh"].angle')], "keyed"
# Poses aren't shared: the hub leg is as it was.
assert abs(world_angles(legs[hub])[0] - a[0]) < 1e-6
# On frame 0 there's nothing to match; outside a switch neither.
goto(0)
layers.active = legs[linked]
assert oanim.ops.armature.match_pose_previous() == {'CANCELLED'}
goto(30)
layers.active = body
assert oanim.ops.armature.match_pose_previous() == {'CANCELLED'}

# --- Select Bones This Drawing Uses ------------------------------------------------------------------
goto(0)
layers.active = legs[hub]
oanim.ops.layer.new()
foot = layers.active
foot.name = "Foot"
foot.add_curve([(-0.1, -2.4), (0.1, -2.4), (0.1, -2.6), (-0.1, -2.6)], closed=True, fill=True)
assert oanim.ops.layer.move_to_group(group=str(legs[hub].id)) == {'FINISHED'}
layers.active = foot
sync()
assert oanim.ops.armature.select_used_bones() == {'FINISHED'}
assert layers.active.name == hub
assert [b["name"] for b in legs[hub].bones if b["selected"]], "the bones its points are weighted to"

# --- Saved with the file ----------------------------------------------------------------------------
path = folder / "legs.oanim"
assert oanim.ops.wm.save_as_mainfile(filepath=str(path)) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=str(path)) == {'FINISHED'}
assert bone(find(linked), "thigh")["stands_in_for"] == (hub, "thigh")
assert find(hub).sub_bone_group

print("switch bones OK")
