"""Deform modifiers on groups: Curve, Cage and Mesh Deform on a group bend every drawing inside it, however deep. Each
drawing runs its own stack first, then the groups', nearest first. The shape's movement counts once: a drawing it isn't
moved with follows it whole, one the same bones move takes only its change of shape. Nothing bends twice, and moving
the group itself still moves what is in it. Ignore Group Deforms opts a drawing out.

Run with:  OpenAnimate --background --python tests/test_group_deform.py
"""

import os
import shutil
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0


def close(a, b, eps=1e-3):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


# New layers and groups start at the top of the document, beside this one.
anchor = layers.active


def new_layer(name, points=None):
    layers.active = anchor
    ops.layer.new()
    layer = layers.active
    layer.name = name
    if points:
        layer.add_curve(points)
    return layer


def into_group(group, *children):
    for child in children:
        layers.active = child
        assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}


def new_group(name, *children):
    layers.active = anchor
    assert ops.layer.new_group() == {'FINISHED'}
    group = layers.active
    group.name = name
    into_group(group, *children)
    return group


def add_cage(group, cage, kind="CAGE_DEFORM"):
    layers.active = group
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    index = len(group.modifiers) - 1
    group.modifier_set(index, layer=cage.name, shape=cage.shapes[0][0])
    return index


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def key_point(layer, index, pos, frame=10):
    doc.frame_current = frame
    layer.set_point(index, pos)
    at(frame)


# --- A group's Modifiers ------------------------------------------------------------------------------------
cage = new_layer("Cage")
assert cage.add_primitive("BOX", [(-1, -1), (1, 1)])
art = new_layer("Art", [(1.0, 0.0), (0.0, 0.0)])
deep = new_layer("Deep", [(1.0, 0.0), (0.0, 0.0)])
group = new_group("Group", art)
inner = new_group("Inner", deep)
into_group(group, inner)

layers.active = group
assert ops.object.modifier_add(type="MIRROR") == {'CANCELLED'}, "a group takes the deforms only"
assert ops.object.modifier_add(type="BOOLEAN") == {'CANCELLED'}
assert ops.object.modifier_add(type="BEVEL") == {'CANCELLED'}
assert not group.modifiers
for kind, wanted in (("CURVE_DEFORM", "Curve Deform"), ("MESH_DEFORM", "Mesh Deform"), ("CAGE_DEFORM", "Cage Deform")):
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    assert group.modifiers[-1]["name"] == wanted, group.modifiers
while group.modifiers:
    assert ops.object.modifier_remove(index="0") == {'FINISHED'}
i = add_cage(group, cage)
m = group.modifiers[i]
assert m["kind"] == "Cage" and m["layer"] == "Cage" and m["weights"] == "AUTOMATIC", m
assert ops.object.modifier_apply(index=str(i)) == {'CANCELLED'}, "a group's deform can't be applied"

# --- It bends what is inside, however deep, and nothing on the setup frame -----------------------------------
assert all(close(p, q[:2]) for p, q in zip(art.evaluated()[0], art.points)), "on the setup frame nothing bends"
key_point(cage, 2, (1.5, 1.5))
pts = art.evaluated()[0]
assert close(pts[0], (1.25, 0.25)), pts
assert 0.0 < pts[1][0] < 0.5, "inside, it follows partly"
assert close(deep.evaluated()[0][0], (1.25, 0.25)), "a nested group's drawing bends too"
assert close(art.shown()[0][0], (1.25, 0.25)), art.shown()
assert art.points[0][:2] == (1.0, 0.0), "the drawing itself stays as it is"
assert close(cage.evaluated()[0][2], (1.5, 1.5)), "the shape, outside, is as it is drawn"

# --- Strength ------------------------------------------------------------------------------------------------
group.modifier_set(i, strength=0.5)
assert close(art.evaluated()[0][0], (1.125, 0.125)), art.evaluated()[0]
group.modifier_set(i, strength=1.0)
doc.frame_current = 10
assert ops.anim.keyframe_toggle(layer=str(group.id), paths="modifiers[0].strength") == {'FINISHED'}
doc.frame_current = 20
assert ops.anim.keyframe_toggle(layer=str(group.id), paths="modifiers[0].strength") == {'FINISHED'}
group.modifier_set(i, strength=0.0)
at(20)
assert group.modifiers[i]["strength"] == 0.0, group.modifiers[i]
assert close(art.evaluated()[0][0], (1.0, 0.0)), "keyed to nothing"
at(15)
assert 1.0 < art.evaluated()[0][0][0] < 1.25, ("between its keys", art.evaluated()[0])
at(20)
assert ops.anim.keyframe_toggle(layer=str(group.id), paths="modifiers[0].strength") == {'FINISHED'}
at(10)
assert ops.anim.keyframe_toggle(layer=str(group.id), paths="modifiers[0].strength") == {'FINISHED'}
group.modifier_set(i, strength=1.0)
at(10)
assert close(art.evaluated()[0][0], (1.25, 0.25)), art.evaluated()[0]

# --- Off: in the viewport, and by the drawing ------------------------------------------------------------------
assert art.ignore_group_deforms is False
art.ignore_group_deforms = True
at(10)
assert close(art.evaluated()[0][0], (1.0, 0.0)), "Ignore Group Deforms"
assert close(deep.evaluated()[0][0], (1.25, 0.25)), "only that drawing"

# --- Saved -----------------------------------------------------------------------------------------------------
folder = tempfile.mkdtemp()
path = os.path.join(folder, "group_deform.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
shutil.rmtree(folder, ignore_errors=True)
find = lambda name: [l for l in layers.all() if l.name == name][0]
cage, art, deep, group, inner = (find(n) for n in ("Cage", "Art", "Deep", "Group", "Inner"))
assert len(group.modifiers) == 1 and group.modifiers[0]["layer"] == "Cage", group.modifiers
assert art.ignore_group_deforms is True and deep.ignore_group_deforms is False
art.ignore_group_deforms = False
at(10)
assert close(art.evaluated()[0][0], (1.25, 0.25)), art.evaluated()[0]

# --- Undo: adding it is one step -------------------------------------------------------------------------------
layers.active = inner
assert ops.object.modifier_add(type="CURVE_DEFORM") == {'FINISHED'}
assert len(inner.modifiers) == 1
assert ops.ed.undo() == {'FINISHED'}
inner = find("Inner")
assert not inner.modifiers, "one undo step"

# --- Nearest group first -------------------------------------------------------------------------------------
doc.frame_current = 0
line = new_layer("Line", [(-2.0, 2.0), (2.0, 2.0)])
inner_cage = new_layer("InnerCage")
assert inner_cage.add_primitive("BOX", [(-1, -1), (1, 1)])
i2 = add_cage(inner, inner_cage)
key_point(inner_cage, 2, (1.5, 1.0))
# Inner moves its top right corner along x and Group's then bends that: not the other way round.
doc.frame_current = 10
find_deep = deep.evaluated()[0][0]
at(10)
inner.modifier_set(i2, strength=0.0)
at(10)
alone = deep.evaluated()[0][0]
inner.modifier_set(i2, strength=1.0)
at(10)
assert not close(find_deep, alone), (find_deep, alone)
layers.active = inner
assert ops.object.modifier_remove(index="0") == {'FINISHED'}
at(10)

# --- The shape inside the group, no double bending ---------------------------------------------------------------
doc.frame_current = 0
shape = new_layer("Shape")
assert shape.add_primitive("BOX", [(-1, -1), (1, 1)])
warp = new_layer("Warp", [(-1.0, 0.0), (1.0, 0.0)])
hair = new_layer("Hair", [(-1.0, 0.5), (0.0, 0.5), (1.0, 0.5)])
cover = new_group("Cover", shape, warp, hair)
add_cage(cover, shape)
layers.active = hair
assert ops.object.modifier_add(type="CURVE_DEFORM") == {'FINISHED'}
hair.modifier_set(0, layer="Warp", shape=warp.shapes[0][0])
key_point(shape, 2, (1.5, 1.5))
assert close(shape.evaluated()[0][2], (1.5, 1.5)), "the shape inside its group isn't bent by itself"
assert not close(warp.evaluated()[0][1], (1.0, 0.0)), "the group bends the sibling"
# The line bends once, by the group: read by its neighbour it counts as it was, so the neighbour isn't bent by the line
# having been bent, on top of the group bending it.
alone = hair.evaluated()[0]
layers.active = hair
hair.modifier_set(0, strength=0.0)
at(10)
without = hair.evaluated()[0]
hair.modifier_set(0, strength=1.0)
at(10)
assert all(close(a, b) for a, b in zip(alone[:3], without[:3])), ("bent twice", alone[:3], without[:3])

# --- A reference copy follows ------------------------------------------------------------------------------------
doc.frame_current = 0
source = new_layer("Source", [(1.0, 0.0), (0.0, 0.0)])
ref_cage = new_layer("RefCage")
assert ref_cage.add_primitive("BOX", [(-1, -1), (1, 1)])
layers.active = source
assert ops.layer.make_reference() == {'FINISHED'}
copy = layers.active
copy.name = "Copy"
assert copy.reference["match_original"]
held = new_group("Held", source, copy)
add_cage(held, ref_cage)
key_point(ref_cage, 2, (1.5, 1.5))
assert close(source.evaluated()[0][0], (1.25, 0.25)), source.evaluated()[0]
assert close(copy.evaluated()[0][0], (1.25, 0.25)), ("a Match Original reference bends with it", copy.evaluated()[0])
assert close(copy.shown()[0][0], (1.25, 0.25)), copy.shown()

# --- The shape's movement: followed whole, or only its change of shape --------------------------------------------
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
rig.name = "Rig"
assert ops.armature.bone_add(name="head", head_x="0.0", head_y="0.0", tail_x="0.0", tail_y="1.0") == {'FINISHED'}
carried_cage = new_layer("CarriedCage")
assert carried_cage.add_primitive("BOX", [(-1, -1), (1, 1)])
into_group(rig, carried_cage)
bound = new_layer("Bound", [(1.0, 0.0), (0.0, 0.0)])
into_group(rig, bound)
# One group around a drawing the bones move and one around a drawing they don't, both on the shape.
loose = new_layer("Loose", [(1.0, 0.0), (0.0, 0.0)])
outside = new_group("Outside", loose)
add_cage(outside, carried_cage)
inside = new_group("Inside")
into_group(rig, inside)
into_group(inside, bound)
add_cage(inside, carried_cage)
doc.frame_current = 10
rig.bone_set("head", position=(0.0, 0.5))
at(10)
assert close(carried_cage.evaluated()[0][0], (-1.0, -0.5)), carried_cage.evaluated()[0]
assert close(loose.evaluated()[0][1], (0.0, 0.5)), ("no bones of its own: it follows the shape whole", loose.evaluated()[0])
assert close(bound.evaluated()[0][1], (0.0, 0.5)), ("the bones move it, and once", bound.evaluated()[0])
# The cage's own change of shape then bends both.
carried_cage.set_point(2, (1.5, 1.5))
at(10)
bent = bound.evaluated()[0][0]
assert close(bent, (1.25, 0.75)), ("bones first, then its change of shape", bent)
assert close(loose.evaluated()[0][0], (1.25, 0.75)), loose.evaluated()[0]

# --- Moving the group itself moves what is in it ------------------------------------------------------------------
doc.frame_current = 0
mover = new_layer("Mover", [(1.0, 0.0), (0.0, 0.0)])
still = new_layer("StillCage")
assert still.add_primitive("BOX", [(-1, -1), (1, 1)])
walking = new_group("Walking", mover)
add_cage(walking, still)
doc.frame_current = 10
walking.location_x = 0.5
at(10)
assert close(mover.evaluated()[0][0], (1.0, 0.0)), ("an unchanged shape bends nothing", mover.evaluated()[0])

print("group deforms: all good")
