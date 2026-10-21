"""Detail levels (multiresolution shapes): the knife adds detail points at a shape's detail level;
they ride the coarser curve (stretching with it), hide below their level, key as offsets, save,
and Delete Higher, Apply Detail and Rebuild Levels work on the selected shapes.

Run with:  OpenAnimate --background --python tests/test_detail_levels.py
"""

import os
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0
doc.auto_key = True


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def where(i):
    return tuple(layer.points[i][:2])


def outline(l=None, shape=0):
    return (l or layer).shape_outline(shape, 32)[0]


def to_polyline(p, line):
    """How far point p is from a polyline."""
    best = 1e9
    for (ax, ay), (bx, by) in zip(line, line[1:] + line[:1]):
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / max(dx * dx + dy * dy, 1e-12)))
        best = min(best, ((p[0] - ax - dx * t) ** 2 + (p[1] - ay - dy * t) ** 2) ** 0.5)
    return best


def same_outline(a, b, eps=2e-3):
    """The two outlines run along the same line (whatever their samples)."""
    return all(to_polyline(p, b) < eps for p in a) and all(to_polyline(p, a) < eps for p in b)


layer = layers.active
layer.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True, fill=True)
oanim.ops.mesh.select_all()

# No shape selected: the level can't be set.
oanim.ops.mesh.select_none()
assert oanim.ops.mesh.detail_level_set(level="1") == {'CANCELLED'}
oanim.ops.mesh.select_all()

# Level 0 has nothing above it yet, so one past it (1) is open, and no further.
assert oanim.ops.mesh.detail_level_set(level="3") == {'FINISHED'}
assert layer.detail["shape_levels"] == [1], layer.detail
# Ctrl 0 to Ctrl 4 set it, Alt Page Up and Page Down step it.
assert oanim.wm.event_simulate("0", ctrl=True, area="Viewport")
assert layer.detail["shape_levels"] == [0]
assert oanim.wm.event_simulate("PAGE_UP", alt=True, area="Viewport")
assert layer.detail["shape_levels"] == [1]

# The knife at level 1 adds a detail point, keeping the shape's look.
before = outline()
d = layer.split_segment(0, 0, 0.5)
assert layer.detail["levels"][d] == 1, layer.detail
doc.frame_current = 1
doc.frame_current = 0
assert same_outline(before, outline()), "the knife keeps the look"
on_curve = where(d)
assert abs(on_curve[0] - 0.5) < 1e-3, on_curve

# A bump: the detail point pulled out below the bottom edge.
bump = (0.5, on_curve[1] - 0.2)
layer.set_point(d, bump)
doc.frame_current = 1
doc.frame_current = 0
assert close(where(d), bump)

# Scaling the coarse points carries the detail with them, stretched (twice as big here). At level 0
# the detail point is hidden: it can't be selected, and rides the curve.
assert oanim.ops.mesh.detail_level_set(level="0") == {'FINISHED'}
assert layer.detail["hidden"][d], layer.detail
assert layer.detail["wires"][0] == [0, 1, 2, 3], ("the wire runs through the points shown", layer.detail["wires"])
oanim.ops.mesh.select_all()
assert not layer.points[d][2], "hidden detail isn't selected"
assert oanim.ops.mesh.transform_selected(pivot_x="0", pivot_y="0", scale_x="2", scale_y="2") == {'FINISHED'}
assert close(where(d), (bump[0] * 2, bump[1] * 2), 1e-3), where(d)

# Moving one coarse point bends the segment; the bump stays a bump on it.
assert oanim.ops.mesh.transform_selected(pivot_x="0", pivot_y="0", scale_x="0.5", scale_y="0.5") == {'FINISHED'}
assert close(where(d), bump, 1e-3)

# Animation: posing the coarse points away from frame 0 moves the detail without keying it.
doc.frame_current = 10
oanim.ops.mesh.select_none()
layer.select_points([1])
assert oanim.ops.mesh.transform_selected(move_x="1") == {'FINISHED'}
doc.frame_current = 11
doc.frame_current = 10
assert [k["frame"] for k in layer.keyframes("points[1].pos")] == [0, 10]
assert layer.keyframes("points[%d].pos" % d) == [], "detail rides: no keys of its own"
assert where(d)[0] > 0.6, ("rides the stretched edge", where(d))
doc.frame_current = 0
assert close(where(d), bump, 1e-3), "and frame 0 is as it was"

# The detail point keyed at its level: its keys hold its offset from the curve.
assert oanim.ops.mesh.select_all() == {'FINISHED'}
assert oanim.ops.mesh.detail_level_set(level="1") == {'FINISHED'}
assert layer.detail["wires"][0] == [0, d, 1, 2, 3], layer.detail["wires"]
doc.frame_current = 20
layer.set_point(d, (where(d)[0], where(d)[1] - 0.3))
doc.frame_current = 21
assert [k["frame"] for k in layer.keyframes("points[%d].pos" % d)] == [0, 20], layer.keyframes("points[%d].pos" % d)
doc.frame_current = 0
assert close(where(d), bump, 1e-3)

# Smooth to Lower Level: the detail point back onto the curve of the level below (half way, then all
# the way), on the setup frame here.
oanim.ops.mesh.select_none()
layer.select_points([d])
assert oanim.ops.mesh.detail_smooth(influence="0.5") == {'FINISHED'}
assert close(where(d), ((bump[0] + on_curve[0]) / 2, (bump[1] + on_curve[1]) / 2), 1e-3), where(d)
assert oanim.ops.mesh.detail_smooth() == {'FINISHED'}
assert close(where(d), on_curve, 1e-3), ("on the level below's curve", where(d), on_curve)
oanim.ops.ed.undo()
oanim.ops.ed.undo()
assert close(where(d), bump, 1e-3)
oanim.ops.mesh.select_none()
layer.select_points([0])
assert oanim.ops.mesh.detail_smooth() == {'CANCELLED'}, "a base point has no level below"
oanim.ops.mesh.select_all()

# Saved with the project: levels, places and offsets.
folder = tempfile.mkdtemp(prefix="oanim_detail_")
path = os.path.join(folder, "detail.oanim")
look = outline()
assert oanim.ops.wm.save_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
layer = layers.active
assert layer.detail["levels"][d] == 1 and layer.detail["shape_levels"] == [1], layer.detail
assert same_outline(look, outline()), "opens as saved"

# Delete Higher at level 0 takes the detail away: the coarse square is left.
oanim.ops.mesh.select_all()
assert oanim.ops.mesh.detail_level_set(level="0") == {'FINISHED'}
oanim.ops.mesh.select_all()
assert oanim.ops.mesh.detail_delete_higher() == {'FINISHED'}
assert max(layer.detail["levels"]) == 0 and layer.point_count == 4, layer.detail
assert same_outline(before, outline()), "the bump went with it: the square is as it was"
oanim.ops.ed.undo()
assert layer.point_count == 5 and same_outline(look, outline())

# Rebuild Levels on a shape whose every point makes its shape changes nothing (not even its levels).
levels_before = layer.detail["levels"]
assert oanim.ops.mesh.detail_rebuild(tolerance="0.001") == {'CANCELLED'}
assert layer.detail["levels"] == levels_before and same_outline(look, outline()), "left as it was"

# Apply Detail: the detail points become the shape's own, looking the same.
oanim.ops.mesh.select_all()
assert oanim.ops.mesh.detail_apply() == {'FINISHED'}
assert max(layer.detail["levels"]) == 0
assert same_outline(look, outline())

# Rebuild Levels: a dense line keeps the points that make its shape, the rest become detail.
layer2 = layers.active
oanim.ops.mesh.select_none()
layer2.add_curve([(x / 10.0, 2.0 + (0.05 if x == 5 else 0.0)) for x in range(11)])
oanim.ops.mesh.select_none()
layer2.select_points([layer2.point_count - 1])
line_before = outline(layer2, 1)
assert oanim.ops.mesh.detail_rebuild(tolerance="0.01") == {'FINISHED'}
levels = layer2.detail["levels"][-11:]
assert levels[0] == 0 and levels[-1] == 0 and levels[5] == 0, levels
assert levels.count(1) >= 4, levels
line_first = layer2.point_count - 11
assert same_outline(line_before, outline(layer2, 1)), "rebuilt, it looks the same"

# A round shape drawn with many points keeps a few and makes the rest detail.
import math
oanim.ops.mesh.select_none()
layer2.add_curve([(3 + math.cos(a * math.pi / 8), math.sin(a * math.pi / 8)) for a in range(16)], closed=True, fill=True)
oanim.ops.mesh.select_none()
layer2.select_points([layer2.point_count - 1])
round_before = outline(layer2, layer2.shape_count - 1)
assert oanim.ops.mesh.detail_rebuild(tolerance="0.02") == {'FINISHED'}
round_levels = layer2.detail["levels"][-16:]
assert round_levels.count(1) >= 6 and round_levels.count(0) >= 3, round_levels
assert same_outline(round_before, outline(layer2, layer2.shape_count - 1)), "rebuilt, the circle looks the same"

# Deleting a coarse point next to detail keeps the detail on the joined segment.
oanim.ops.mesh.select_none()
first = line_first
layer2.select_points([first + 5])
assert oanim.ops.mesh.dissolve_points() == {'FINISHED'}
assert layer2.detail["levels"].count(1) >= 6

print("Detail level tests passed")
