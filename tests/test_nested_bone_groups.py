"""Bone layers inside bone layers.

- A layer's action list reaches the bone layers around it, nearest first, and the operators act on the one asked for, so
  two bone layers can each have an action of one name.
- Mirroring an action takes in the bone layers inside it: one with no side mirrors its own bones, and a pair (Arm_L,
  Arm_R) take each other's.

- A, Alt A and Ctrl I from a bone layer take in the sub bone groups shown inside it, not those around it.
- G, R and S move what's picked in them, each by the same move in the document, unless a picked bone carries them already.

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

# --- Picking all from the bone layer around them --------------------------------------------------------------------------
def picked(rig):
    return [b["name"] for b in rig.bones if b["selected"]]


def press(key, **mods):
    assert oanim.wm.event_simulate(key, area="Viewport", **mods)


at(0)
layers.active = outer
press("A", alt=True)
assert picked(outer) == [] and picked(inner) == []
press("A")
assert "Turn_L" in picked(outer) and sorted(picked(inner)) == ["Cheek_L", "Cheek_R", "Jaw_X"], (picked(outer), picked(inner))
assert sorted(picked(arm_l)) == ["Upper"] and sorted(picked(arm_r)) == ["Upper"], "every sub bone group inside, however deep"
assert layers.active.id == outer.id, "still worked from the outside"
press("A", alt=True)
assert picked(outer) == [] and picked(inner) == [] and picked(arm_l) == []
# Ctrl I turns them over along with its own.
layers.active = inner
inner.select_bone("Jaw_X")
layers.active = outer
press("I", ctrl=True)
assert "Jaw_X" not in picked(inner) and sorted(picked(inner)) == ["Cheek_L", "Cheek_R"], picked(inner)
assert "Turn_L" in picked(outer) and picked(arm_l) == ["Upper"]
press("A", alt=True)

# From inside, the bone layers around it stay out of it.
layers.active = inner
press("A")
assert sorted(picked(inner)) == ["Cheek_L", "Cheek_R", "Jaw_X"] and picked(outer) == [] and picked(arm_l) == []
press("A", alt=True)

# A bone layer that isn't a sub bone group, or that's hidden, isn't worked from outside.
inner.sub_bone_group = False
layers.active = outer
press("A")
assert picked(inner) == [] and "Turn_L" in picked(outer)
press("A", alt=True)
inner.sub_bone_group = True
arm_r.visible = False
press("A")
assert picked(arm_r) == [] and picked(arm_l) == ["Upper"]
press("A", alt=True)
arm_r.visible = True

# --- Moving them from the bone layer around them ---------------------------------------------------------------------------
import math

at(0)
ops.layer.new_bone()
top = layers.active
top.name = "Top"
bone(top, "Root", (0.0, 3.0), (0.0, 3.5))
bone(top, "Arm", (1.0, 3.0), (1.5, 3.0))
sub = rig_in("Sub", top)
bone(sub, "Ear", (0.5, 3.0), (0.5, 3.4))
# Its own space turned a quarter: a move is in the document's, whatever each bone layer's is.
turned = rig_in("Turned", top)
bone(turned, "Tuft", (0.0, 0.0), (0.0, 0.4))
turned.rotation = math.pi / 2
at(5)


def head(rig, name):
    return rig.to_document(rig.bone_pose(name)[:2])


def angle_of(rig, name):
    pose = rig.bone_pose(name)
    a, b = rig.to_document(pose[:2]), rig.to_document(pose[2:4])
    return math.atan2(b[1] - a[1], b[0] - a[0])


def pick_all():
    layers.active = top
    press("A", alt=True)
    press("A")


def state():
    return head(top, "Arm"), head(sub, "Ear"), head(turned, "Tuft"), angle_of(top, "Arm"), angle_of(sub, "Ear"), angle_of(turned, "Tuft")


pick_all()
before = state()
assert ops.armature.translate(move_x="0.2", move_y="0.1", only_bones="0") == {'FINISHED'}
at(5)
after = state()
for was, now in zip(before[:3], after[:3]):
    assert close(now, (was[0] + 0.2, was[1] + 0.1)), ("every picked bone by the same move", was, now)
assert layers.active.id == top.id

before = after
assert ops.armature.rotate(angle="30", only_bones="0") == {'FINISHED'}
at(5)
after = state()
for was, now in zip(before[3:], after[3:]):
    assert abs(math.remainder(now - was - math.radians(30), math.tau)) < 1e-3, ("every picked bone turns", was, now)

before = sub.bone_pose("Ear")
assert ops.armature.resize(scale="2", only_bones="0") == {'FINISHED'}
at(5)
assert sub.bones[0]["scale"][0] > 1.5 and top.bones[1]["scale"][0] > 1.5, (sub.bones[0], top.bones[1])
assert ops.armature.resize(scale="0.5", only_bones="0") == {'FINISHED'}

# One undo step for the lot.
before = state()
assert ops.armature.translate(move_x="0.3", only_bones="0") == {'FINISHED'}
assert ops.ed.undo() == {'FINISHED'}
at(5)
assert all(close(a, b) for a, b in zip(before[:3], state()[:3])), "undone together"

# Only the nested bones picked, the bone layer around them active.
layers.active = top
press("A", alt=True)
layers.active = sub
sub.select_bone("Ear")
layers.active = top
before = state()
assert ops.armature.translate(move_x="0.1", only_bones="0") == {'FINISHED'}
at(5)
after = state()
assert close(after[1], (before[1][0] + 0.1, before[1][1])) and close(after[0], before[0]) and close(after[2], before[2]), (before, after)

# Keys go on each: the frame after holds the move.
at(10)
assert close(head(sub, "Ear"), after[1]), "keyed where it moved"

# A bone layer hung from a picked bone comes along, once.
at(0)
layers.active = top
press("A", alt=True)
top.select_bone("Arm")
layers.active = sub
assert ops.layer.new_group() == {'FINISHED'}
hung = layers.active
hung.name = "Hung"
if hung.parent is None or hung.parent.id != top.id:
    assert ops.layer.move_to_group(group=str(top.id)) == {'FINISHED'}
layers.active = sub
if sub.parent.id != hung.id:
    assert ops.layer.move_to_group(group=str(hung.id)) == {'FINISHED'}
layers.active = hung
assert ops.layer.parent_bone_set(bone="Arm") == {'FINISHED'}
at(15)
pick_all()
before = state()
assert ops.armature.translate(move_x="0.2", only_bones="0") == {'FINISHED'}
at(15)
after = state()
assert close(after[0], (before[0][0] + 0.2, before[0][1]))
assert close(after[1], (before[1][0] + 0.2, before[1][1])), ("carried, not moved twice", before[1], after[1])
assert close(after[2], (before[2][0] + 0.2, before[2][1]))

# From inside, the bone layer around it doesn't move.
at(20)
layers.active = top
press("A", alt=True)
pick_all()
layers.active = sub
before = state()
assert ops.armature.translate(move_x="0.1", only_bones="0") == {'FINISHED'}
at(20)
after = state()
assert close(after[1], (before[1][0] + 0.1, before[1][1])) and close(after[0], before[0]), (before, after)

print("nested_bone_groups: ok")
