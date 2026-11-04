"""A point bent by a deform modifier lands where it is placed: place_point, as a G drag, takes the move back through what
the whole stack does right around the point, its turn and stretch, bones and each point's share included.

Run with:  OpenAnimate --background --python tests/test_deform_drag.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True
anchor = layers.active


def close(a, b, eps=0.003):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def new_layer(name, points=None):
    layers.active = anchor
    ops.layer.new()
    layer = layers.active
    layer.name = name
    if points:
        layer.add_curve(points)
    return layer


def new_box(name):
    layer = new_layer(name)
    assert layer.add_primitive("BOX", [(-1, -1), (1, 1)])
    return layer


def deform(layer, target, kind="CAGE_DEFORM"):
    layers.active = layer
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    layer.modifier_set(len(layer.modifiers) - 1, **{"CURVE_DEFORM": dict(curve_method="SMOOTH", cage_method="MEAN_VALUE"), "MESH_DEFORM": dict(curve_method="SMOOTH", cage_method="MEAN_VALUE"), "CAGE_DEFORM": dict(cage_method="OUTLINE", resample_self=True)}.get(kind, {}))
    index = len(layer.modifiers) - 1
    layer.modifier_set(index, layer=target.name, shape=target.shapes[0][0])
    return index


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def reshape(layer, change, frame=10):
    doc.frame_current = frame
    for i, p in enumerate(layer.points):
        layer.set_point(i, change(p[0], p[1]))
    at(frame)


def lands(layer, point=0, frame=10, move=(0.05, 0.03)):
    at(frame)
    x, y = layer.shown()[0][point]
    layer.place_point(point, (x + move[0], y + move[1]))
    at(frame)
    now = layer.shown()[0][point]
    assert close(now, (x + move[0], y + move[1])), (layer.name, now, (x + move[0], y + move[1]))


# --- Stretched ----------------------------------------------------------------------------------------------
cage = new_box("Cage")
art = new_layer("Art", [(0.3, 0.2), (-0.2, -0.1)])
deform(art, cage)
reshape(cage, lambda x, y: (x * 1.5, y))
lands(art)
lands(art, 1, move=(-0.04, 0.05))
assert close(art.points[1][:2], (-0.2 - 0.04 / 1.5, -0.1 + 0.05)), "stored a third less far, the cage stretching it"

# --- Turned ---------------------------------------------------------------------------------------------------
turned = new_box("Turned")
spun = new_layer("Spun", [(0.3, 0.2), (-0.2, -0.1)])
deform(spun, turned)
reshape(turned, lambda x, y: (-y, x))
lands(spun)

# --- Along a line ---------------------------------------------------------------------------------------------
line = new_layer("Line", [(-1.0, 0.0), (1.0, 0.0)])
hair = new_layer("Hair", [(0.2, 0.1), (-0.3, 0.1)])
deform(hair, line, "CURVE_DEFORM")
reshape(line, lambda x, y: (y, x))
lands(hair)

# --- A grid ---------------------------------------------------------------------------------------------------
grid = new_layer("Grid")
assert grid.add_primitive("GRID", [(-1, -1), (1, 1)], columns=2, rows=2)
cloth = new_layer("Cloth", [(0.3, 0.3), (-0.3, -0.3)])
deform(cloth, grid, "MESH_DEFORM")
reshape(grid, lambda x, y: (x * 1.3 + y * 0.4, y))
lands(cloth)

# --- Resample above the deform: the drawing's own points keep their places -----------------------------------------
fine_cage = new_box("FineCage")
fine = new_layer("Fine", [(0.5, 0.2), (-0.5, 0.2)])
layers.active = fine
assert ops.object.modifier_add(type="RESAMPLE") == {'FINISHED'}
deform(fine, fine_cage)
reshape(fine_cage, lambda x, y: (x * 1.5, y * 0.8))
lands(fine)

# --- Two deforms, one after the other -------------------------------------------------------------------------------
first, second = new_box("First"), new_box("Second")
both = new_layer("Both", [(0.3, 0.2), (-0.2, -0.1)])
deform(both, first)
deform(both, second)
reshape(first, lambda x, y: (x * 1.5, y))
reshape(second, lambda x, y: (x, y * 1.4))
lands(both)

# --- Shared with the bones -----------------------------------------------------------------------------------------
doc.frame_current = 0
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="b", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
bone_cage = new_box("BoneCage")
boned = new_layer("Boned", [(0.3, 0.2), (-0.2, -0.1)])
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
boned = next(l for l in layers.all() if l.name == "Boned")
layers.active = boned
ops.mesh.select_none()
boned.select_points([0, 1])
rig.select_bone("b")
assert ops.weight.assign(weight="1.0") == {'FINISHED'}
ops.mesh.select_none()
b = deform(boned, bone_cage)
reshape(bone_cage, lambda x, y: (x * 1.5, y))
doc.frame_current = 10
rig.bone_set("b", angle=0.7)
at(10)
lands(boned)
boned.modifier_set(b, strength=0.5)
at(10)
# Half the bone's turn, half the cage's stretch: neither alone takes it back.
lands(boned)

# --- A shape collapsed, and the setup frame ---------------------------------------------------------------------------
flat = new_box("Flat")
pressed = new_layer("Pressed", [(0.3, 0.2), (-0.2, -0.1)])
deform(pressed, flat)
reshape(flat, lambda x, y: (x * 0.0, y))
at(10)
assert all(not math.isnan(v) for p in pressed.shown()[0] for v in p), pressed.shown()
at(0)
x, y = art.shown()[0][0]
art.place_point(0, (x + 0.1, y))
assert close(art.shown()[0][0], (x + 0.1, y)), "on the setup frame nothing bends"

print("deform drags: all good")
