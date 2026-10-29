"""Make Segment (F): joining two curve ends runs the shapes on along the new segment, even a shape of several runs;
two points that aren't both ends (Blender's F between any two vertices) get a segment of their own, welded to
both, drawn as the line through them is.

Run with:  OpenAnimate --background --python tests/test_make_segment_points.py
"""

import oanim

layers = oanim.data.layers
ops = oanim.ops
ops.layer.new()
l = layers.active


def sel(*points):
    ops.mesh.select_none()
    l.select_points(list(points))


def unstroked():
    """Segments (curve, segment) no shape draws."""
    drawn = set()
    for i in range(l.shape_count):
        for run in l.shape_runs(i):
            drawn.update(run)
    every = set()
    for c in range(l.curve_count):
        n = len(l.curve_handles(c))
        every.update((c, s) for s in range(n - 1))
    return sorted(every - drawn)


# A line, then a new line from its middle point (Extrude there starts a curve welded to it) and one from its end.
l.add_curve([(-0.6, 0.2), (0.0, 0.2), (0.5, 0.2)])
sel(1)
assert l.extrude_selected() == [3]
l.set_point(3, (0.0, 0.0))
sel(0)
assert l.extrude_selected() == [4]
l.set_point(4, (-0.6, 0.0))
assert l.shape_count == 1 and len(l.shape_runs(0)) == 2, "one shape of two runs"
assert unstroked() == []

# Joining the two new ends: the shape runs on along the new segment, so it is drawn.
sel(4, 3)
assert ops.mesh.make_segment() == {'FINISHED'}
assert unstroked() == [], unstroked()

# Point 3 is now in the middle of a curve: F to the far end makes a segment of its own, drawn too.
sel(3, 2)
assert ops.mesh.make_segment() == {'FINISHED'}
assert unstroked() == [], unstroked()
assert l.shape_count == 1, "the line's shape draws it"
# The same two again: there is a segment already.
sel(3, 2)
assert ops.mesh.make_segment() == {'CANCELLED'}

# Next to a filled shape: the new segment gets a line of the same look, and the fill keeps going round.
ops.layer.new()
l = layers.active
assert l.add_primitive("BOX", [(-1, -1), (1, 1)], fill=True)
sel(0, 2)
assert ops.mesh.make_segment() == {'FINISHED'}
assert l.shape_count == 2 and l.shape_style(0)["has_fill"] and not l.shape_style(1)["has_fill"]
assert l.shape_outline(0)[1], "the box still goes all the way round"
assert unstroked() == []

print("Make Segment tests passed")
