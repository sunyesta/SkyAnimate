"""Make Segment (F over the viewport, as Blender's for curves): two selected curve ends get a segment
between them. Ends of two curves join them into one; the two ends of one curve close it. Shapes
stopping at the joined ends run on along it (one each side become one); one that goes all the way
round is filled. Two points that aren't both ends get a segment of their own (test_make_segment_points.py).

Run with:  OpenAnimate --background --python tests/test_make_segment.py
"""

import oanim

layers = oanim.data.layers
wm = oanim.wm
oanim.data.document.frame_current = 0
art = layers.active


def counts():
    return (art.point_count, art.curve_count, art.shape_count)


def pick(*indices):
    art.select_points(range(art.point_count), False)
    art.select_points(list(indices))


# Two halves of an outline, as drawn one each side: 0-1-2-3 and 4-5-6-7 (the second drawn the other way).
art.add_curve([(-0.1, 0.1), (-0.5, 0.3), (-0.6, 0.2), (-0.15, -0.05)])
art.add_curve([(0.1, 0.1), (0.5, 0.3), (0.6, 0.2), (0.15, -0.05)])
oanim.ops.mesh.select_all()  # an undo step to come back to
assert counts() == (8, 2, 2)
assert [art.shape_style(i)["has_fill"] for i in range(2)] == [False, False]

# Not two ends (Blender's F between any two vertices): a segment of their own, welded to both.
pick(1, 5)
assert oanim.ops.mesh.make_segment() == {'FINISHED'}
assert counts() == (8, 3, 2), counts()
oanim.ops.ed.undo()
assert counts() == (8, 2, 2), counts()
# One point: nothing, and the status bar says why.
pick(3)
assert oanim.ops.mesh.make_segment() == {'CANCELLED'}
assert counts() == (8, 2, 2)

# The two inner ends: one curve, one shape running along both halves and the new segment.
pick(3, 7)
assert wm.event_simulate("F", area="Viewport")
assert counts() == (8, 1, 1), counts()
assert wm.undo_history()[0][-1] == "Make Segment"
assert art.shape_style(0)["has_fill"] is False, "still open: no fill yet"
outline = art.shape_outline(0)
assert len(outline) > 0

# The two outer ends, now the ends of one curve: it closes, and the shape is finished: filled.
pick(0, 4)
assert wm.event_simulate("F", area="Viewport")
assert counts() == (8, 1, 1)
assert art.shape_style(0)["has_fill"] is True
assert art.shape_count == 1 and art.shapes[0][1] == [0]
# Closed now, and already joined by a segment: nothing to make.
pick(0, 4)
assert oanim.ops.mesh.make_segment() == {'CANCELLED'}

# Undo brings both halves back.
oanim.ops.ed.undo()
oanim.ops.ed.undo()
assert counts() == (8, 2, 2), counts()

# A line and another line's start: the second runs on from the first, whichever way each was drawn.
oanim.ops.layer.new()
art = layers.active
art.add_curve([(0.0, 0.0), (1.0, 0.0)])
art.add_curve([(3.0, 0.0), (2.0, 0.0)])
pick(1, 3)
assert oanim.ops.mesh.make_segment() == {'FINISHED'}
assert counts() == (4, 1, 1)
xs = [round(p[0], 3) for p in art.evaluated()[0]]
assert sorted(xs) == [0.0, 1.0, 2.0, 3.0], xs
assert art.curve_handles(0) and len(art.curve_handles(0)) == 4

print("make_segment: ok")
