"""The Bevel modifier: a drawing's corners cut off and rounded, Blender's vertex bevel for curves.
Width Type says what Amount measures (Offset, Width, Depth, Percent); Limit Method which corners (None,
Angle, a Point Group, its weights scaling each one); Profile the cut's shape; Clamp Overlap keeps cuts
on one segment from crossing.

Run with:  OpenAnimate --background --python tests/test_bevel_modifier.py
"""

import math
import os
import tempfile

import oanim

layers = oanim.data.layers
doc = oanim.data.document
doc.frame_current = 0
art = layers.active


def add_square(x0, y0, x1, y1):
    before = art.point_count
    art.add_curve([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], closed=True, fill=True)
    art.select_points(range(art.point_count), False)
    art.select_points(range(before, art.point_count))
    assert oanim.ops.mesh.peak() == {'FINISHED'}
    art.select_points(range(art.point_count), False)


add_square(-1, -1, 1, 1)  # sides 2, corners turning 90 degrees


def shown():
    points, _, shapes = art.evaluated()
    return len(points), shapes[0][4]


def bevel(**values):
    art.modifier_set(0, **values)


assert oanim.ops.object.modifier_add(type="BEVEL") == {'FINISHED'}
m = art.modifiers[0]
assert (m["type"], m["name"], m["limit"], m["width_type"], round(m["angle"]), m["profile"], m["clamp"]) == \
    ("BEVEL", "Bevel", "ANGLE", "OFFSET", 30, 0.5, True), m

# Offset 0.2, straight (Profile 0.25): each corner loses a right triangle of 0.2 x 0.2.
bevel(amount=0.2, profile=0.25)
count, area = shown()
assert count == 8 and abs(area - (4 - 4 * 0.02)) < 1e-3, (count, area)
# Round (0.5): a quarter circle's worth stays.
bevel(profile=0.5)
_, area = shown()
assert abs(area - (4 - 4 * 0.04 * (1 - math.pi / 4))) < 2e-3, area
# Square (1): almost the corner again; inward (0): scooped, a quarter circle taken out.
bevel(profile=1.0)
assert shown()[1] > 3.985
bevel(profile=0.0)
_, area = shown()
assert abs(area - (4 - math.pi * 0.04)) < 3e-3, area
bevel(profile=0.25)

# Width Type: the same cut measured other ways (90 degree corners).
bevel(width_type="WIDTH", amount=0.2 * math.sqrt(2))
assert abs(shown()[1] - 3.92) < 1e-3, "the cut 0.28 long"
bevel(width_type="DEPTH", amount=0.2 / math.sqrt(2))
assert abs(shown()[1] - 3.92) < 1e-3, "the cut 0.14 from the corner"
bevel(width_type="PERCENT", amount=10.0)
assert abs(shown()[1] - 3.92) < 1e-3, "a tenth of each side"
bevel(width_type="OFFSET", amount=0.2)

# Clamp Overlap: a cut bigger than half a side stops at its middle (a diamond is left).
bevel(amount=1.5)
assert abs(shown()[1] - 2.0) < 1e-3, shown()
bevel(amount=0.2)

# Limit Method Angle: corners turning less than the Angle aren't cut.
bevel(angle=100.0)
assert shown()[0] == 4
bevel(angle=30.0)

# A smooth loop has no corners to cut.
oanim.ops.layer.new()
loop = layers.active
loop.add_curve([(5, 0), (6, 1), (5, 2), (4, 1)], closed=True, fill=True)
assert oanim.ops.object.modifier_add(type="BEVEL") == {'FINISHED'}
loop.modifier_set(0, amount=0.2)
assert len(loop.evaluated()[0]) == 4, "smooth points aren't corners"
loop.modifier_set(0, limit="NONE")
assert len(loop.evaluated()[0]) == 8, "None: every point"
layers.active = art

# Limit Method Point Group: only its points, each by its weight.
art.select_points([0, 1])
oanim.ops.mesh.point_group_add(name="Corners")
assert oanim.ops.mesh.point_group_assign() == {'FINISHED'}
art.select_points(range(art.point_count), False)
art.select_points([1])
assert oanim.ops.mesh.point_group_assign(weight="0.5") == {'FINISHED'}
bevel(limit="GROUP", group="Corners")
count, area = shown()
assert count == 6 and abs(area - (4 - 0.02 - 0.005)) < 1e-3, ("one cut 0.2, one 0.1", count, area)
bevel(group="Nope")
assert shown()[0] == 4, "no such group: nothing cut"
bevel(group="Corners")

# Keyed Amount.
path = "modifiers[0].amount"
assert oanim.ops.anim.keyframe_toggle(layer=str(art.id), paths=path) == {'FINISHED'}
doc.frame_current = 10
bevel(amount=0.0)
assert oanim.ops.anim.keyframe_toggle(layer=str(art.id), paths=path) == {'FINISHED'}
doc.frame_current = 5
assert shown()[0] == 6 and 4 - 0.025 < shown()[1] < 4.0, shown()
doc.frame_current = 0

# Saved and opened.
path = os.path.join(tempfile.mkdtemp(), "bevel.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
art = next(l for l in layers.all() if l.id == art.id)
layers.active = art
m = art.modifiers[0]
assert (m["limit"], m["group"], m["width_type"], round(m["amount"], 3)) == ("GROUP", "Corners", "OFFSET", 0.2), m

# Apply: the cuts become the drawing's own points.
before = shown()
assert oanim.ops.object.modifier_apply(index="0") == {'FINISHED'}
assert art.modifiers == [] and art.point_count == 6
assert abs(shown()[1] - before[1]) < 1e-4

print("bevel_modifier: ok")
