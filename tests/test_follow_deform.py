"""Follow Deform, a modifier: one button, a Layer and a Shape as the other deforms have, and the whole drawing carried by a
point on that shape without bending, as Toon Boom's Point Kinematic Output.

- It takes a line, a closed shape, a grid or a mesh, and lands where the deform that fits them puts a point there.
- The drawing stays exactly its shape. Its point starts at its middle, nothing jumps, frame 0 is the rest.
- Strength and a second point (Rotation, Scale), each as asked for; the point dragged or keyed along the shape.
- Or it sits on a point the shape already has, and goes exactly as that point does.
- Its bones stay on top; those that move the shape as well count once.
- Collapsed shapes and loops never break it. Saved with the file, undone, and duplicated with the layer.

Run with:  OpenAnimate --background --python tests/test_follow_deform.py
"""

import math
import os
import shutil
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
anchor = layers.active
folder = tempfile.mkdtemp(prefix="oanim_follow_deform_")


def close(a, b, eps=1e-3):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def finite(points):
    return all(math.isfinite(v) for p in points for v in p[:2])


def new_layer(name):
    layers.active = anchor
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


def box(name, lo=(-1, -1), hi=(1, 1)):
    layer = new_layer(name)
    assert layer.add_primitive("BOX", [lo, hi])
    return layer


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def add_follow(layer, shape, **settings):
    layers.active = layer
    assert ops.object.modifier_add(type="FOLLOW_DEFORM") == {'FINISHED'}
    index = len(layer.modifiers) - 1
    layer.modifier_set(index, layer=shape.name, shape=shape.shapes[0][0], **settings)
    return index


def drawn(layer):
    return layer.evaluated()[0]


def place(shape, fn, frame=10):
    doc.frame_current = 0
    rest = [p[:2] for p in shape.points]
    doc.frame_current = frame
    for i, p in enumerate(rest):
        shape.set_point(i, fn(p))
    at(frame)


def sides(points):
    return [(q[0] - points[0][0], q[1] - points[0][1]) for q in points]


# --- One button, a shape as the others have ---------------------------------------------------------------------------
cage = box("Cage")
stud = box("Stud", (0.1, 0.1), (0.3, 0.3))
i = add_follow(stud, cage)
m = stud.modifiers[i]
assert m["type"] == "FOLLOW_DEFORM" and m["name"] == "Follow Deform", m
assert m["layer"] == "Cage" and m["kind"] == "Cage" and m["fits"] == "FOLLOW_DEFORM", m
assert close(m["point"], (0.2, 0.2)), ("starts at its middle", m["point"])
assert not m["show_point"] and not m["second"] and m["rotation"] and not m["scale"]
with_type = False
try:
    stud.modifier_set(i, type="CAGE_DEFORM")
except ValueError:
    with_type = True
assert with_type, "it stays what it is"
# Not on a group: a group's deforms are for the drawings in it.
layers.active = new_layer("Spare")
assert ops.layer.new_group() == {'FINISHED'}
assert ops.object.modifier_add(type="FOLLOW_DEFORM") == {'CANCELLED'}

# Every kind of shape, as the deform that fits it puts a point there.
at(0)
line = new_layer("Line")
line.add_curve([(-1, 0.2), (0, 0.2), (1, 0.2)])
grid = new_layer("Grid")
assert grid.add_primitive("GRID", [(-1, -1), (1, 1)], columns=2, rows=2)
for shape, kind, deform in ((cage, "Cage", "CAGE_DEFORM"), (line, "Line", "CURVE_DEFORM"), (grid, "Grid", "MESH_DEFORM")):
    tag = kind
    rider = box("Rider" + tag, (0.1, 0.1), (0.3, 0.3))
    n = add_follow(rider, shape)
    assert rider.modifiers[n]["kind"] == kind, rider.modifiers[n]
    probe = new_layer("Probe" + tag)
    probe.add_curve([(0.2, 0.2), (0.21, 0.2)])
    layers.active = probe
    assert ops.object.modifier_add(type=deform) == {'FINISHED'}
    probe.modifier_set(len(probe.modifiers) - 1, layer=shape.name, shape=shape.shapes[0][0], precision=0.0)
    rest = drawn(rider)
    place(shape, lambda p: (p[0] * 1.2 + 0.2 * p[1] + 0.1, p[1] + 0.3 * p[0] * p[0] - 0.1))
    points = drawn(rider)
    move = (drawn(probe)[0][0] - 0.2, drawn(probe)[0][1] - 0.2)
    assert move != (0.0, 0.0)
    for p, r in zip(points, rest):
        assert close(p, (r[0] + move[0], r[1] + move[1]), 1e-4), (kind, p, r, move)
    assert close(sides(points)[2], sides(rest)[2], 1e-5), ("rigid", kind)
    place(shape, lambda p: p)
    at(0)
    assert drawn(rider) == rest

# --- Strength, the point, frame 0 -----------------------------------------------------------------------------------
at(0)
place(cage, lambda p: (p[0] + 0.5, p[1]))
assert close(drawn(stud)[0], (0.6, 0.1)), drawn(stud)
at(5)
assert close(drawn(stud)[0], (0.35, 0.1)), ("between the keys", drawn(stud))
at(0)
assert close(drawn(stud)[0], (0.1, 0.1)), "frame 0 is the rest"
stud.modifier_set(i, strength=0.5)
at(10)
assert close(drawn(stud)[0], (0.35, 0.1)), drawn(stud)
stud.modifier_set(i, strength=1.0)
at(0)

# A corner pulled: which place carries it is the point's, and at rest moving it changes nothing.
place(cage, lambda p: (p[0], p[1] + (0.6 if p[0] > 0 and p[1] > 0 else 0.0)))
near = drawn(stud)[0]
at(0)
rest = drawn(stud)
stud.modifier_set(i, point=(-0.8, -0.8))
assert drawn(stud) == rest, "moving the point at rest moves nothing"
at(10)
far = drawn(stud)[0]
assert near[1] - 0.1 > far[1] - 0.1 + 0.05, (near, far)
at(0)
stud.modifier_set(i, point=(0.2, 0.2))

# --- A second point -------------------------------------------------------------------------------------------------
at(0)
gem_cage = box("GemCage")
gem = box("Gem", (-0.1, -0.1), (0.1, 0.1))
g = add_follow(gem, gem_cage)
gem.modifier_set(g, second=True)
assert close(gem.modifiers[g]["point2"], (0.1, 0.0)), "beside the first"
gem.modifier_set(g, point2=(0.5, 0.0))
rest = drawn(gem)
place(gem_cage, lambda p: (-p[1], p[0]))
points = drawn(gem)
assert close(points[0], (rest[0][1] * -1, rest[0][0]), 0.02), ("a quarter turn", points, rest)
assert close(sides(points)[2], (-sides(rest)[2][1], sides(rest)[2][0]), 0.02)
gem.modifier_set(g, rotation=False)
at(10)
assert close(sides(drawn(gem))[2], sides(rest)[2], 1e-3), "Rotation off"
gem.modifier_set(g, rotation=True)
place(gem_cage, lambda p: (p[0] * 2, p[1] * 2))
assert close(sides(drawn(gem))[2], sides(rest)[2], 1e-3), "no Scale until asked"
gem.modifier_set(g, scale=True)
at(10)
stretched = sides(drawn(gem))[2]
assert abs(stretched[0] / sides(rest)[2][0] - 2.0) < 0.06, (stretched, sides(rest)[2])
at(0)
assert drawn(gem) == rest

# --- Keyed along the shape -----------------------------------------------------------------------------------------
at(0)
slide_cage = box("SlideCage")
slide = box("Slide", (-0.1, -0.1), (0.1, 0.1))
s = add_follow(slide, slide_cage)
doc.frame_current = 10
slide.modifier_set(s, shift=(0.4, 0.0))
at(10)
assert close(slide.modifiers[s]["shift"], (0.4, 0.0)) and close(drawn(slide)[0], (0.3, -0.1)), drawn(slide)
at(0)
assert close(slide.modifiers[s]["shift"], (0.0, 0.0)) and close(drawn(slide)[0], (-0.1, -0.1))
at(5)
assert -0.1 < drawn(slide)[0][0] < 0.3, drawn(slide)
# As a drag does: it lands under the mouse through the bend.
at(0)
pull_cage = box("PullCage")
pull = box("Pull", (-0.1, -0.1), (0.1, 0.1))
pu = add_follow(pull, pull_cage)
doc.frame_current = 0
corner = max(range(len(pull_cage.points)), key=lambda k: pull_cage.points[k][0] + pull_cage.points[k][1])
start = pull_cage.points[corner][:2]
doc.frame_current = 10
pull_cage.set_point(corner, (start[0] + 0.7, start[1] + 0.5))
at(10)
for target in ((0.5, 0.3), (-0.3, 0.2)):
    doc.frame_current = 10
    pull.modifier_set(pu, point_to=target)
    at(10)
    points = drawn(pull)
    middle = ((points[0][0] + points[2][0]) / 2, (points[0][1] + points[2][1]) / 2)
    assert close(middle, target, 2e-3), ("under the mouse", target, middle, pull.modifiers[pu]["shift"])

# --- Bones ----------------------------------------------------------------------------------------------------------
at(0)
layers.active = anchor
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
rig.name = "Rig"
assert ops.armature.bone_add(name="head", head_x="0", head_y="0", tail_x="0", tail_y="1") == {'FINISHED'}
ops.armature.bone_parent_set(bone="head", parent="")
assert ops.armature.bone_add(name="other", head_x="2", head_y="0", tail_x="2", tail_y="1") == {'FINISHED'}
ops.armature.bone_parent_set(bone="other", parent="")
head_cage = box("HeadCage")
worn = box("Worn", (0.3, 0.3), (0.5, 0.5))
loose_cage = box("LooseCage")
loose = box("LooseStud", (0.3, 0.3), (0.5, 0.5))
for layer, bone in ((head_cage, "head"), (worn, "head"), (loose, "other")):
    layers.active = layer
    assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
    assert ops.layer.parent_bone_set(bone=bone) == {'FINISHED'}
add_follow(worn, head_cage)
add_follow(loose, loose_cage)
# A bone moving the shape and the drawing counts once.
doc.frame_current = 10
rig.bone_set("head", angle=rig.bones[0]["setup_angle"] + math.pi / 2)
at(10)
points = [worn.to_document(p) for p in drawn(worn)]
assert close(points[0], (-0.3, 0.3), 2e-2), ("a quarter turn once", points[0])
# Its own bones stay on top of a shape that has nothing to do with them.
doc.frame_current = 10
rig.bone_set("other", position=(rig.bones[1]["position"][0] + 0.2, rig.bones[1]["position"][1]))
place(loose_cage, lambda p: (p[0], p[1] + 0.25))
shown = loose.to_document(drawn(loose)[0])
assert close(shown, (0.5, 0.55), 2e-2), ("its bone's move and the shape's", shown)

# Inside a group bending by the same shape: carried, not bent as well.
at(0)
face_cage = box("FaceCage")
eye = box("EyeOn", (0.3, 0.3), (0.5, 0.5))
skin = box("SkinOn", (-0.5, 0.3), (-0.3, 0.5))
layers.active = anchor
assert ops.layer.new_group() == {'FINISHED'}
face = layers.active
face.name = "Face"
for layer in (eye, skin):
    layers.active = layer
    assert ops.layer.move_to_group(group=str(face.id)) == {'FINISHED'}
layers.active = face
assert ops.object.modifier_add(type="CAGE_DEFORM") == {'FINISHED'}
face.modifier_set(0, layer=face_cage.name, shape=face_cage.shapes[0][0])
add_follow(eye, face_cage)
rest = drawn(eye)
place(face_cage, lambda p: (p[0] * 1.5, p[1] + 0.2 * p[0]))
points = drawn(eye)
assert close(sides(points)[2], sides(rest)[2], 1e-4), ("the group doesn't bend it", sides(points), sides(rest))
assert points[0] != rest[0], "but it goes with the shape"
assert not close(sides(drawn(skin))[2], (0.2, 0.2), 1e-3), "what's beside it still bends"

# --- A point the shape already has ------------------------------------------------------------------------------------
at(0)
line2 = new_layer("PickLine")
line2.add_curve([(-1, 0.2), (0, 0.5), (1, 0.2)])
grid2 = new_layer("PickGrid")
assert grid2.add_primitive("GRID", [(-1, -1), (1, 1)], columns=2, rows=2)
rigid = box("PickRigid")
methods = ((box("PickCage"), {}), (rigid, {"cage_method": "RIGID"}), (line2, {}), (grid2, {}))
for shape, settings in methods:
    at(0)
    # A grid's middle node is on no outline, but it's a point the grid has.
    number = max(range(len(shape.points)), key=lambda k: -abs(shape.points[k][0]) - abs(shape.points[k][1]))
    start = shape.points[number][:2]
    rider = box("On" + shape.name, start, (start[0] + 0.1, start[1] + 0.1))
    n = add_follow(rider, shape, **settings)
    rider.modifier_set(n, shape_point=number)
    m = rider.modifiers[n]
    assert m["shape_point"] == number and m["shape_point2"] is None, m
    assert close(m["point"], start, 1e-6), ("it rests where the shape's point does", m["point"], start)
    rest = drawn(rider)
    assert close(rest[0], start, 1e-6), "nothing jumps"
    place(shape, lambda p: (p[0] * 1.3 + 0.2 * p[1] + 0.2, p[1] * 0.7 + 0.4 * p[0] * p[0] - 0.3))
    now = shape.evaluated()[0][number]
    points = drawn(rider)
    assert close(points[0], now, 1e-5), ("exactly on it", shape.name, points[0], now)
    assert close(sides(points)[2], sides(rest)[2], 1e-5), ("and not bent", shape.name)
    # Nothing to shift: it is that point.
    rider.modifier_set(n, shift=(0.3, 0.3))
    at(10)
    assert close(drawn(rider)[0], now, 1e-5), ("a picked point has no shift", shape.name)
    rider.modifier_set(n, shift=(0.0, 0.0))
    # Strength still says how much.
    rider.modifier_set(n, strength=0.5)
    at(10)
    half = ((now[0] + start[0]) / 2, (now[1] + start[1]) / 2)
    assert close(drawn(rider)[0], half, 1e-5), ("half way", drawn(rider)[0], half)
    rider.modifier_set(n, strength=1.0)
    # Moved on frame 0, the point rests somewhere else: the drawing stays and takes its change from there.
    at(0)
    moved = (start[0] + 0.05, start[1] - 0.05)
    shape.set_point(number, moved)
    assert close(drawn(rider)[0], start, 1e-6), "frame 0 is the rest"
    at(10)
    now = shape.evaluated()[0][number]
    change = (now[0] - moved[0], now[1] - moved[1])
    assert close(drawn(rider)[0], (start[0] + change[0], start[1] + change[1]), 1e-5), ("its change from rest", shape.name)
    # Let go of on frame 0: its own place, where the point rests.
    at(0)
    here = shape.points[number][:2]
    rider.modifier_set(n, shape_point=None)
    assert rider.modifiers[n]["shape_point"] is None and close(rider.modifiers[n]["point"], here, 1e-6)

# A grid's inner nodes are on offer, not only its outline.
sitter = box("Sitter", (0.0, 0.0), (0.1, 0.1))
n = add_follow(sitter, grid2)
for node in range(len(grid2.points)):
    sitter.modifier_set(n, shape_point=node)
    assert sitter.modifiers[n]["shape_point"] == node

# Two of the shape's points turn it.
at(0)
hoop = box("Hoop", (-0.8, -0.8), (0.8, 0.8))
ring = box("Ring", (-0.8, -0.8), (-0.6, -0.6))
n = add_follow(ring, hoop)
ring.modifier_set(n, shape_point=0, second=True, shape_point2=1)
assert ring.modifiers[n]["shape_point2"] == 1 and close(ring.modifiers[n]["point2"], hoop.points[1][:2], 1e-6)
rest = drawn(ring)
place(hoop, lambda p: (-p[1], p[0]))
points = drawn(ring)
assert close(points[0], hoop.evaluated()[0][0], 1e-5), points[0]
assert close(sides(points)[2], (-sides(rest)[2][1], sides(rest)[2][0]), 1e-5), ("a quarter turn", sides(points)[2], sides(rest)[2])
# With its first on the shape and its second its own.
ring.modifier_set(n, shape_point2=None)
ring.modifier_set(n, point2=(0.8, -0.8))
at(10)
assert close(drawn(ring)[0], hoop.evaluated()[0][0], 1e-5)
assert close(sides(drawn(ring))[2], (-sides(rest)[2][1], sides(rest)[2][0]), 0.02), sides(drawn(ring))[2]

# Bones moving both, and the shape turning it on top: round the point, where they have put it.
at(0)
bone_cage = box("BoneCage")
badge = box("Badge", (1.0, 1.0), (1.2, 1.2))
for layer in (bone_cage, badge):
    layers.active = layer
    assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
    assert ops.layer.parent_bone_set(bone="head") == {'FINISHED'}
corner = next(k for k in range(len(bone_cage.points)) if close(bone_cage.points[k], (1, 1)))
across = next(k for k in range(len(bone_cage.points)) if close(bone_cage.points[k], (-1, 1)))
n = add_follow(badge, bone_cage)
badge.modifier_set(n, shape_point=corner, second=True, shape_point2=across)
place(bone_cage, lambda p: (2 - p[1], p[0]))
shown = badge.to_document(drawn(badge)[0])
wanted = bone_cage.to_document(bone_cage.evaluated()[0][corner])
assert close(shown, wanted, 1e-4), ("on it, bones and all", shown, wanted)

# Only the shape's own points, and only with a shape.
at(0)
two = box("TwoShapes", (-0.8, -0.8), (0.8, 0.8))
others = len(two.points)
assert two.add_primitive("BOX", [(1.0, 1.0), (1.2, 1.2)])
off = box("Off", (0, 0), (0.1, 0.1))
n = add_follow(off, two)
for bad in (others, 999, -1):
    try:
        off.modifier_set(n, shape_point=bad)
    except ValueError:
        continue
    raise AssertionError(("a point off the shape", bad))
assert off.modifiers[n]["shape_point"] is None
off.modifier_set(n, layer="")
try:
    off.modifier_set(n, shape_point=0)
except ValueError:
    pass
else:
    raise AssertionError("no shape")

# A new shape or layer lets go of it: its id means nothing there.
off.modifier_set(n, layer=two.name, shape=two.shapes[0][0])
off.modifier_set(n, shape_point=0)
off.modifier_set(n, shape=two.shapes[1][0])
assert off.modifiers[n]["shape_point"] is None
off.modifier_set(n, shape=two.shapes[0][0], shape_point=0)
off.modifier_set(n, layer=hoop.name)
assert off.modifiers[n]["shape_point"] is None

# The point deleted from the shape: it stays put, and says so by having none.
at(0)
gone = box("Gone", (-0.8, -0.8), (0.8, 0.8))
rider = box("GoneRider", (0, 0), (0.1, 0.1))
n = add_follow(rider, gone)
rider.modifier_set(n, shape_point=1)
at(0)
layers.active = gone
gone.select_points(range(len(gone.points)), False)
gone.select_points([1])
assert ops.mesh.delete() == {'FINISHED'}
at(10)
assert close(drawn(rider)[0], (0.0, 0.0)) and rider.modifiers[n]["shape_point"] is None
at(0)

# Symmetrized, the copy's point is across from the first: picked or its own, the copy follows its own side.
at(0)
middle = box("Middle")
for name, pick in (("Stud_L", False), ("Pin_L", True)):
    at(0)
    lefty = box(name, (0.8, 0.8), (1.0, 1.0))
    n = add_follow(lefty, middle)
    corner = next(k for k in range(len(middle.points)) if close(middle.points[k], (1, 1)))
    if pick:
        lefty.modifier_set(n, shape_point=corner)
    else:
        lefty.modifier_set(n, point=(1.0, 1.0))
    layers.active = lefty
    doc.frame_current = 10
    lefty.modifier_set(n, shift=(0.01, 0.0))
    at(0)
    assert ops.layer.symmetrize() == {'FINISHED'}
    righty = next(l for l in layers.all() if l.name == name[:-1] + "R")
    m = righty.modifiers[n]
    assert close(m["point"], (-1.0, 1.0), 1e-5) and m["shape_point"] is None and close(m["shift"], (0.0, 0.0)), m
    left_corner = next(k for k in range(len(middle.points)) if close(middle.points[k], (-1, 1)))
    start = righty.to_document(drawn(righty)[0])
    place(middle, lambda p: (p[0] + (0.4 if p[0] < 0 and p[1] > 0 else 0.0), p[1]))
    moved = righty.to_document(drawn(righty)[0])
    assert close((moved[0] - start[0], moved[1] - start[1]), (0.4, 0.0), 1e-3), ("its own side", name, start, moved)
    place(middle, lambda p: p)
    at(0)

# --- What can go wrong ----------------------------------------------------------------------------------------------
at(0)
wild = box("Wild")
rider = box("WildRider", (0.1, 0.1), (0.3, 0.3))
w = add_follow(rider, wild)
rider.modifier_set(w, second=True, scale=True)
for name, fn in (("a point", lambda p: (0.2, 0.3)), ("a line", lambda p: (p[0], 0.0)), ("flipped", lambda p: (-p[0], p[1])),
                 ("huge", lambda p: (p[0] * 1e5, p[1] * 1e5)), ("tiny", lambda p: (p[0] * 1e-6, p[1] * 1e-6))):
    place(wild, fn)
    assert finite(drawn(rider)), (name, drawn(rider))
# Its shape gone, it stays put.
at(0)
rider.modifier_set(w, layer="")
at(10)
assert close(drawn(rider)[0], (0.1, 0.1)), drawn(rider)
# A shape carried by what it carries: a loop, and neither moves.
at(0)
a, b = box("LoopA"), box("LoopB")
add_follow(a, b)
add_follow(b, a)
place(b, lambda p: (p[0] + 0.1, p[1]))
assert finite(drawn(a)) and finite(drawn(b))

# --- Saved, undone, duplicated ----------------------------------------------------------------------------------------
at(10)
keep = drawn(slide), drawn(gem), drawn(stud), None
stud.modifier_set(i, show_point=True)
ring.modifier_set(0, shape_point2=1)
at(10)
keep = keep[:3] + (drawn(ring),)
path = os.path.join(folder, "follow.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
find = lambda name: next(l for l in layers.all() if l.name == name)
stud, gem, slide, ring = find("Stud"), find("Gem"), find("Slide"), find("Ring")
m = stud.modifiers[0]
assert m["type"] == "FOLLOW_DEFORM" and m["show_point"] and close(m["point"], (0.2, 0.2)), m
assert gem.modifiers[0]["second"] and gem.modifiers[0]["scale"] and close(gem.modifiers[0]["point2"], (0.5, 0.0))
at(10)
assert close(drawn(slide)[0], keep[0][0]) and close(drawn(gem)[0], keep[1][0]) and close(drawn(stud)[0], keep[2][0])
assert ring.modifiers[0]["shape_point"] == 0 and ring.modifiers[0]["shape_point2"] == 1 and close(drawn(ring)[0], keep[3][0], 1e-5)
at(0)
assert close(slide.modifiers[0]["shift"], (0.0, 0.0))
layers.active = stud
assert ops.object.modifier_remove(index=str(len(stud.modifiers) - 1)) == {'FINISHED'}
assert ops.ed.undo() == {'FINISHED'}
assert stud.modifiers[-1]["type"] == "FOLLOW_DEFORM"
layers.active = stud
assert ops.layer.duplicate() == {'FINISHED'}
copy = layers.active
assert copy.id != stud.id and copy.modifiers[-1]["type"] == "FOLLOW_DEFORM" and close(copy.modifiers[-1]["point"], (0.2, 0.2))

shutil.rmtree(folder, ignore_errors=True)
print("follow_deform: ok")
