"""Curve Deform's Part: a drawing following one stretch of a shape, here two lips on one mouth line.

- a closed mouth, shut at rest, carries each lip by its own side as it opens, where the whole loop as a cage throws
  them far off;
- nothing moves at rest, and the two sides can't drift apart: they are the one line;
- Other Side, swapped ends, follows the other way round;
- a Part is set from selected points, which must run along the shape; an open line's Part follows only that stretch;
- it saves, and undo takes it back;
- a Part whose points are deleted turns the deform off, it doesn't guess; a deform bent by a Precision of the line's
  own keeps its Part.

Run with:  OpenAnimate --background --python tests/test_curve_deform_part.py
"""
import math
import os
import tempfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0


def new_layer(name, points, closed=False):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    layer.add_curve(points, closed=closed, fill=closed)
    return layer


def deform(layer, target, **settings):
    layers.active = layer
    assert ops.object.modifier_add(type="CURVE_DEFORM") == {'FINISHED'}
    layer.modifier_set(len(layer.modifiers) - 1, layer=target.name, shape=target.shapes[0][0], precision=0.0, **settings)
    return len(layer.modifiers) - 1


def shown(layer):
    return [tuple(p[:2]) for p in layer.evaluated()[0]]


def near(a, b, tol=1e-3):
    return math.hypot(a[0] - b[0], a[1] - b[1]) < tol


# A mouth as a loop of four points, shut at rest: 0 right corner, 1 bottom middle, 2 left corner, 3 top middle.
RIGHT, BOTTOM, LEFT, TOP = 0, 1, 2, 3
mouth = new_layer("mouth", [(0.2, 0.0), (0.0, -0.0001), (-0.2, 0.0), (0.0, 0.0001)], closed=True)
top_lip = new_layer("top_lip", [(-0.1, 0.05), (0.0, 0.08), (0.1, 0.05)])
bottom_lip = new_layer("bottom_lip", [(-0.1, -0.05), (0.0, -0.08), (0.1, -0.05)])

i_top = deform(top_lip, mouth)
i_bottom = deform(bottom_lip, mouth)
assert top_lip.modifiers[i_top]["kind"] == "", top_lip.modifiers[i_top]
assert top_lip.modifiers[i_top]["part"] is None
assert shown(top_lip)[1] == tuple(top_lip.points[1][:2]), "a closed shape with no Part does nothing"

top_lip.modifier_set(i_top, part=(RIGHT, LEFT))
bottom_lip.modifier_set(i_bottom, part=(RIGHT, LEFT))
assert top_lip.modifiers[i_top]["kind"] == "Line"
assert top_lip.modifiers[i_top]["part"] == (RIGHT, LEFT)
assert top_lip.modifiers[i_top]["stretch"]

# Which way round from the right corner to the left is the top depends on how the points run: pick by the ends swapped.
doc.frame_current = 10
mouth.set_point(TOP, (0.0, 0.03))
mouth.set_point(BOTTOM, (0.0, -0.06))
first = shown(top_lip)[1][1]
top_lip.modifier_set(i_top, part=(LEFT, RIGHT))
swapped = shown(top_lip)[1][1]
assert abs(first - swapped) > 0.05, ("the ends swapped are the other side", first, swapped)
if first > swapped:
    top_lip.modifier_set(i_top, part=(RIGHT, LEFT))
    bottom_lip.modifier_set(i_bottom, part=(LEFT, RIGHT))
else:
    bottom_lip.modifier_set(i_bottom, part=(RIGHT, LEFT))

a, b = shown(top_lip)[1], shown(bottom_lip)[1]
assert near(a, (0.0, 0.11)), a
assert near(b, (0.0, -0.14)), b
print("METRIC top lip", a, "bottom lip", b)

doc.frame_current = 0
assert near(shown(top_lip)[1], (0.0, 0.08), 1e-5)
assert near(shown(bottom_lip)[1], (0.0, -0.08), 1e-5)
doc.frame_current = 10

# As the corners spread, the lips spread with them.
mouth.set_point(RIGHT, (0.3, 0.0))
mouth.set_point(LEFT, (-0.3, 0.0))
xs = [p[0] for p in shown(top_lip)]
assert max(xs) > 0.13 and min(xs) < -0.13, xs
mouth.set_point(RIGHT, (0.2, 0.0))
mouth.set_point(LEFT, (-0.2, 0.0))

# The whole loop as a cage, what this is for: shut at rest, it has nothing to measure from and throws a lip far off.
caged = new_layer("caged", [(-0.1, 0.05), (0.0, 0.08), (0.1, 0.05)])
doc.frame_current = 0
layers.active = caged
assert ops.object.modifier_add(type="CAGE_DEFORM") == {'FINISHED'}
caged.modifier_set(0 if caged.modifiers[0]["type"] == "CAGE_DEFORM" else 1, layer="mouth", shape=mouth.shapes[0][0], precision=0.0,
                   cage_method="ENVELOPE")
doc.frame_current = 10
print("METRIC whole loop as a cage", shown(caged)[1], "the Part", shown(top_lip)[1])
assert not near(shown(caged)[1], (0.0, 0.11), 0.05), "no longer needed? the cage carried the lip"
doc.frame_current = 0

# Set from the selection: the points along the stretch, end to end.
picked = new_layer("picked", [(-0.1, 0.05), (0.0, 0.08), (0.1, 0.05)])
i_picked = deform(picked, mouth)
for selection, ok in (([RIGHT, TOP, LEFT], True), ([RIGHT, BOTTOM, LEFT], True), ([TOP, LEFT], True), ([BOTTOM, TOP], False),
                      ([TOP], False), ([RIGHT, BOTTOM, LEFT, TOP], False)):
    layers.active = mouth
    ops.mesh.select_none()
    mouth.select_points(selection)
    layers.active = picked
    try:
        picked.modifier_set(i_picked, part="SELECTED")
        got = picked.modifiers[i_picked]["part"]
        assert ok and {got[0], got[1]} == {selection[0], selection[-1]}, (selection, got)
    except ValueError:
        assert not ok, selection
layers.active = mouth
ops.mesh.select_none()

# The other way, Clear: a closed shape does nothing again; another shape takes the Part away.
picked.modifier_set(i_picked, part=None)
assert picked.modifiers[i_picked]["kind"] == "" and picked.modifiers[i_picked]["part"] is None

# An open line's Part follows that stretch alone.
line = new_layer("line", [(-0.3, 0.3), (-0.1, 0.3), (0.1, 0.3), (0.3, 0.3)])
rider = new_layer("rider", [(0.0, 0.35), (0.05, 0.35)])
i_rider = deform(rider, line, part=(1, 2))
assert rider.modifiers[i_rider]["kind"] == "Line"
doc.frame_current = 10
line.set_point(0, (-0.3, 0.8))
assert near(shown(rider)[0], (0.0, 0.35)), ("what the Part leaves out moves nothing", shown(rider))
line.set_point(0, (-0.3, 0.3))
line.set_point(1, (-0.1, 0.5))
line.set_point(2, (0.1, 0.5))
assert near(shown(rider)[0], (0.0, 0.55), 2e-3), shown(rider)
print("METRIC open line's Part", shown(rider)[0])

# It saves.
path = os.path.join(tempfile.mkdtemp(), "part.oanim")
before = shown(top_lip), shown(bottom_lip)
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
L = {l.name: l for l in layers.all()}
top_lip, bottom_lip, mouth = L["top_lip"], L["bottom_lip"], L["mouth"]
doc.frame_current = 10
assert top_lip.modifiers[-1]["part"] is not None
for x, y in zip(before, (shown(top_lip), shown(bottom_lip))):
    assert all(near(p, q, 1e-5) for p, q in zip(x, y)), (x, y)

# Its points deleted: the deform is off, the lip where it was drawn, and nothing crashes.
ends = top_lip.modifiers[-1]["part"]
doc.frame_current = 0
layers.active = mouth
ops.mesh.select_none()
mouth.select_points([ends[1]])
assert ops.mesh.delete() == {'FINISHED'}
assert top_lip.modifiers[-1]["part"] is None and top_lip.modifiers[-1]["kind"] == ""
assert near(shown(top_lip)[1], (0.0, 0.08), 1e-5), shown(top_lip)
print("all good")
