"""Bone layers inside bone layers.

- A layer's action list reaches the bone layers around it, nearest first, and the operators act on the one asked for, so
  two bone layers can each have an action of one name.
- Mirroring an action takes in the bone layers inside it: one with no side mirrors its own bones, and a pair (Arm_L,
  Arm_R) take each other's.

Clicking and boxing bones through a family needs a viewport: that was checked on a private copy of a real rig.

Run with:  OpenAnimate --background --python tests/test_nested_bone_groups.py
"""

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers
doc.auto_key = True
TURN = 1.2


def close(a, b, eps=1e-3):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def rig_in(name, parent=None):
    if parent:
        layers.active = parent
    assert ops.layer.new_bone() == {'FINISHED'}
    rig = layers.active
    rig.name = name
    if parent:
        assert ops.layer.move_to_group(group=str(parent.id)) == {'FINISHED'}
        layers.active = rig
    return rig


def bone(rig, name, head, tail):
    layers.active = rig
    assert ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]), tail_y=str(tail[1])) == {'FINISHED'}
    ops.armature.bone_parent_set(bone=name, parent="")


def names(rig):
    return [a['name'] for a in rig.actions]


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def tip(rig, name):
    pose = rig.bone_pose(name)
    return rig.to_document(pose[2:4])


# --- The action list ---------------------------------------------------------------------------------------------------
outer = rig_in("Outer")
bone(outer, "Turn_L", (2.0, 1.0), (2.4, 1.0))
middle = rig_in("Middle", outer)
inner = rig_in("Inner", middle)
bone(inner, "Jaw_X", (0.0, 0.2), (0.0, 0.5))
bone(inner, "Cheek_L", (0.5, 0.0), (0.8, 0.3))
bone(inner, "Cheek_R", (-0.5, 0.0), (-0.8, 0.3))
layers.active = inner
assert ops.layer.new() == {'FINISHED'}
drawing = layers.active
drawing.name = "Inside"
assert ops.layer.move_to_group(group=str(inner.id)) == {'FINISHED'}
at(0)
assert [l.name for l in drawing.action_owners] == ["Inner", "Middle", "Outer"]
assert [l.name for l in inner.action_owners] == ["Inner", "Middle", "Outer"]
assert [l.name for l in outer.action_owners] == ["Outer"]

layers.active = outer
assert ops.anim.action_new(name="Walk") == {'FINISHED'}
assert ops.anim.action_set(name="") == {'FINISHED'}
assert names(outer) == ["Walk"] and names(inner) == []

# From inside, by name, and with the layer the menu passes.
layers.active = drawing
assert ops.anim.action_set(name="Walk", layer=str(outer.id)) == {'FINISHED'}
assert doc.action == "Walk"
assert ops.anim.action_set(name="") == {'FINISHED'}
doc.action = "Walk"
assert doc.action == "Walk"
assert ops.anim.action_set(name="") == {'FINISHED'}

# Two bone layers with an action of one name: the menu means the one it lists it under.
layers.active = inner
assert ops.anim.action_new(name="Walk", layer=str(inner.id)) == {'FINISHED'}
assert "Walk" in names(inner) and names(outer) == ["Walk"]
assert ops.anim.action_set(name="") == {'FINISHED'}
assert ops.anim.action_set(name="Walk") == {'FINISHED'}, "the nearest by name alone"
inner.bone_set("Jaw_X", angle=inner.bones[0]["setup_angle"])
assert ops.anim.action_set(name="Walk", layer=str(outer.id)) == {'FINISHED'}, "the same name on another bone layer is a move"
assert ops.anim.action_set(name="") == {'FINISHED'}

# Rename and delete on the one listed, whichever is active; New on the one asked for.
assert ops.anim.action_rename(name="Walk", to="Stride", layer=str(outer.id)) == {'FINISHED'}
assert names(outer) == ["Stride"] and names(inner) == ["Walk"]
assert ops.anim.action_delete(name="Stride", layer=str(outer.id)) == {'FINISHED'}
assert names(outer) == [] and names(inner) == ["Walk"]
assert ops.anim.action_new(name="Hop", layer=str(outer.id)) == {'FINISHED'}
assert names(outer) == ["Hop"] and "Hop" not in names(inner)
assert ops.anim.action_set(name="") == {'FINISHED'}
assert ops.anim.action_delete(name="Hop", layer=str(outer.id)) == {'FINISHED'}
assert ops.anim.action_delete(name="Walk") == {'FINISHED'}
# A layer that isn't a bone layer has no actions to add to.
assert ops.anim.action_new(name="No", layer=str(drawing.id)) == {'CANCELLED'}

# --- Mirroring an action of the bone layer around them ----------------------------------------------------------------
at(0)
arm_l = rig_in("Arm_L", outer)
bone(arm_l, "Upper", (1.0, 0.0), (1.5, 0.0))
arm_r = rig_in("Arm_R", outer)
bone(arm_r, "Upper", (-1.0, 0.0), (-1.5, 0.0))
layers.active = outer
outer.select_bone("Turn_L")
assert ops.anim.action_new(name="Turn_L", smart="1") == {'FINISHED'}
doc.frame_current = 10
outer.bone_set("Turn_L", angle=TURN)
inner.bone_set("Cheek_L", angle=0.5)
inner.bone_set("Jaw_X", angle=0.3)
inner.bone_set("Cheek_R", position=(inner.bones[2]["position"][0] - 0.1, inner.bones[2]["position"][1]))
arm_l.bone_set("Upper", angle=0.4)
at(10)
wanted = {name: tip(inner, name) for name in ("Cheek_L", "Cheek_R", "Jaw_X")}
wanted["Arm"] = tip(arm_l, "Upper")
assert ops.anim.action_set(name="") == {'FINISHED'}
at(0)
rest = {name: tip(inner, name) for name in ("Cheek_L", "Cheek_R", "Jaw_X")}

layers.active = outer
ops.armature.select_none()
outer.select_bone("Turn_L")
assert ops.armature.symmetrize(smart_actions="1") == {'FINISHED'}
assert "Turn_R" in names(outer), names(outer)
assert ops.anim.action_set(name="Turn_R", layer=str(outer.id)) == {'FINISHED'}
at(10)
got = {name: tip(inner, name) for name in ("Cheek_L", "Cheek_R", "Jaw_X")}
flip = lambda p: (-p[0], p[1])
assert close(got["Cheek_R"], flip(wanted["Cheek_L"])), ("a bone takes its other side's keys", got["Cheek_R"], wanted["Cheek_L"])
assert close(got["Cheek_L"], flip(wanted["Cheek_R"])), ("both ways", got["Cheek_L"], wanted["Cheek_R"])
assert close(got["Jaw_X"], flip(wanted["Jaw_X"])), ("a bone with no side turns the other way", got["Jaw_X"], wanted["Jaw_X"])
assert close(tip(arm_r, "Upper"), flip(wanted["Arm"])), ("a pair takes each other's", tip(arm_r, "Upper"), wanted["Arm"])
assert close(tip(arm_l, "Upper"), (1.5, 0.0)), "and the side it came from has nothing here"
assert ops.anim.action_set(name="") == {'FINISHED'}
at(0)
for name, place in rest.items():
    assert close(tip(inner, name), place), ("the original is as it was", name)

print("nested_bone_groups: ok")
