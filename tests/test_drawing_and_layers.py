"""Milestone 2 data: vector drawings, shapes, curve editing, layers, groups, references, masking.

Run with:  OpenAnimate --background --python tests/test_drawing_and_layers.py
"""

import oanim

layers = oanim.data.layers
layer = layers.active
assert layer.name == "Layer 1" and layer.type == "VECTOR"


def counts(l):
    return (l.point_count, l.curve_count, l.shape_count)


# --- Curves and shapes ----------------------------------------------------------
square = [(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)]
layer.add_curve(square, closed=True, fill=True)
assert counts(layer) == (4, 1, 1), counts(layer)
line = layer.add_curve([(1.0, 0.0), (1.5, 0.2), (2.0, 0.0)])
assert counts(layer) == (7, 2, 2)

# Select the open line's points (4, 5, 6) and peak them, then delete the middle one.
oanim.ops.mesh.select_none()
layer.select_points([4, 5, 6])
assert oanim.ops.mesh.peak() == {'FINISHED'}
oanim.ops.mesh.select_none()
layer.select_points([5])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert counts(layer) == (6, 2, 2), "the line joins across the gap, and its shape stays"
assert layer.shapes[1][1] == [1], layer.shapes

# A point deleted from a loop: the loop closes across the gap and its filled shape stays.
layer.add_curve([(3.0, 0.0), (4.0, 0.0), (4.5, 0.5), (4.0, 1.0), (3.5, 1.5), (3.0, 1.0)], closed=True, fill=True)
assert counts(layer) == (12, 3, 3)
oanim.ops.mesh.select_none()
layer.select_points([10])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert counts(layer) == (11, 3, 3), "deleting a point from a shape keeps the shape"
assert layer.shapes[2][1] == [2], layer.shapes
oanim.ops.mesh.select_none()
layer.select_points([6, 10])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert counts(layer) == (9, 3, 3), "the first and last points at once, across where the loop wraps round"
# Down to two points, a loop is a line: its fill no longer goes round.
oanim.ops.mesh.select_none()
layer.select_points([8])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert counts(layer) == (8, 3, 2), counts(layer)
oanim.ops.mesh.select_none()
layer.select_points([6, 7])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert counts(layer) == (6, 2, 2), counts(layer)

# A line down to one point is gone, and its shape with it.
oanim.ops.mesh.select_none()
layer.select_points([5])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert counts(layer) == (4, 1, 1), counts(layer)

# Deleting points at a longer line's end (or both ends) keeps its stroke on what is left; only a filled
# shape that no longer goes round is removed.
layer.add_curve([(1.0, 0.0), (1.5, 0.2), (2.0, 0.0), (2.5, 0.2), (3.0, 0.0)])
assert counts(layer) == (9, 2, 2), counts(layer)
oanim.ops.mesh.select_none()
layer.select_points([4, 8])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert counts(layer) == (7, 2, 2), ("the stroke stays", counts(layer))
assert layer.shapes[1][1] == [1], layer.shapes
assert len(layer.shown()[1][1][0]) == 3
oanim.ops.mesh.select_none()
layer.select_points([4, 5, 6])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert counts(layer) == (4, 1, 1), counts(layer)

# F over the viewport is Make Segment (tests/test_make_segment.py): with a closed loop's points
# selected it has no ends to join, and makes no shape.
oanim.ops.mesh.select_none()
layer.select_points([0, 1, 2, 3])
assert oanim.ops.mesh.make_segment() == {'CANCELLED'}
assert layer.shape_count == 1

# Keys over the viewport: A selects all, Alt A deselects, X deletes.
assert oanim.wm.event_simulate("A", area="Viewport")
assert all(p[2] for p in layer.points)
assert oanim.wm.event_simulate("A", alt=True, area="Viewport")
assert not any(p[2] for p in layer.points)
assert oanim.wm.event_simulate("2", area="Viewport"), "2 switches to edge selection"
assert oanim.wm.event_simulate("3", area="Viewport"), "3 switches to stroke selection"
layer.select_points([0])
assert oanim.ops.mesh.select_mode_stroke() == {'FINISHED'}
assert sum(p[2] for p in layer.points) == 4, "stroke mode selects the whole square"
oanim.ops.mesh.select_mode_point()

# Undo restores deleted geometry.
before = counts(layer)
assert oanim.wm.event_simulate("DEL", area="Viewport"), "Delete deletes the selected points at once (X opens a menu)"
assert layer.point_count == before[0] - 4
oanim.ops.ed.undo()
assert counts(layer) == before

# Duplicate (Shift D) copies the selected curve and its shapes, and selects the copy.
before = counts(layer)
oanim.ops.mesh.select_none()
layer.select_points([0, 1, 2, 3])
assert oanim.wm.event_simulate("D", shift=True, area="Viewport")
assert counts(layer) == (before[0] + 4, before[1] + 1, before[2] + 1), counts(layer)
assert [p[2] for p in layer.points[-4:]] == [True] * 4 and not any(p[2] for p in layer.points[:-4])
oanim.ops.ed.undo()
assert counts(layer) == before
oanim.ops.mesh.select_none()
layer.select_points([0])
assert oanim.ops.mesh.duplicate_move() == {'CANCELLED'}, "one point has no curve piece to copy"

# Part of a stroke: the copy gets its own line, in the same look, without the fill it can't close.
before = counts(layer)
shapes_before = layer.shape_count
oanim.ops.mesh.select_none()
layer.select_points([0, 1, 2])
assert oanim.ops.mesh.duplicate_move() in ({'FINISHED'}, {'RUNNING_MODAL'})
assert counts(layer) == (before[0] + 3, before[1] + 1, before[2] + 1), counts(layer)
new = layer.shape_style(layer.shape_count - 1)
old = layer.shape_style(0)
assert new["has_outline"] and not new["has_fill"], new
assert new["line"] == old["line"] and new["line_width"] == old["line_width"] and new["material"] == old["material"]
oanim.ops.ed.undo()
assert counts(layer) == before and layer.shape_count == shapes_before
oanim.ops.mesh.select_none()

# Part of a fill with no outline: the copy gets a shape in the same look (a fill, no line: it never gains a
# stroke the original didn't have).
oanim.ops.layer.new()
fill_only = layers.active
fill_only.add_curve([(0, 0), (1, 0), (1, 1), (0, 1)], closed=True, fill=True)
fill_only.set_shape_style(0, has_outline=False, line_width=0.0)
assert not fill_only.shape_style(0)["has_outline"]
oanim.ops.mesh.select_none()
fill_only.select_points([0, 1, 2])
assert oanim.ops.mesh.duplicate_move() in ({'FINISHED'}, {'RUNNING_MODAL'})
assert fill_only.shape_count == 2, fill_only.shapes
copy_style = fill_only.shape_style(1)
assert copy_style["has_fill"] and not copy_style["has_outline"], copy_style
oanim.ops.ed.undo()
# Separate (P) takes the same piece to a layer of its own, the same look.
oanim.ops.mesh.select_none()
fill_only.select_points([0, 1, 2])
assert oanim.ops.mesh.separate() == {'FINISHED'}
piece = next(l for l in layers.all() if l.name.startswith(fill_only.name) and l.id != fill_only.id)
assert piece.shape_count == 1 and not piece.shape_style(0)["has_outline"], piece.shapes
oanim.ops.ed.undo()
oanim.ops.mesh.select_none()

# Handles picked on their own stay behind: duplicating and moving the copy (on frame 1, as the move ends) leaves
# the original's handles where they were (they used to swing over with the copy, keyed there).
oanim.ops.layer.new()
picked = layers.active
picked.add_curve([(0, 0), (1, 0.5), (2, 0)])
for frame in (0, 1):
    oanim.data.document.frame_current = frame
    oanim.ops.mesh.select_none()
    picked.select_points([0, 1, 2])
    picked.select_handle(0, 1, "IN")
    handles_before = picked.curve_handles(0)
    assert oanim.ops.mesh.duplicate_move() in ({'FINISHED'}, {'RUNNING_MODAL'})
    assert oanim.ops.mesh.translate(keep_poses="0", move_x="0.5", move_y="0.5", setup="0") == {'FINISHED'}
    assert picked.curve_handles(0) == handles_before, ("the original's handles stay", frame, picked.curve_handles(0))
    oanim.ops.ed.undo()
    oanim.ops.ed.undo()
oanim.data.document.frame_current = 0
oanim.ops.mesh.select_none()
layers.active = layer

# Point radius: 1 by default; Mean Radius moves every selected point by the same amount.
oanim.ops.mesh.select_none()
assert layer.mean_radius is None and all(r == 1.0 for r in layer.radii)
layer.select_points([0, 1])
layer.mean_radius = 0.5
assert layer.radii[0] == layer.radii[1] == 0.5 and layer.radii[2] == 1.0
layer.select_points([2])
assert abs(layer.mean_radius - 2.0 / 3.0) < 1e-6
layer.mean_radius = 0.5
assert [round(r, 4) for r in layer.radii[:3]] == [0.3333, 0.3333, 0.8333], "differences are kept"
layer.mean_radius = 0.0
assert [round(r, 4) for r in layer.radii[:3]] == [0.0, 0.0, 0.3333], "each point moves by the same amount, stopping at 0"
layer.mean_radius = 1.0
oanim.ops.mesh.select_none()


# Hide Edge: the outline skips a hidden segment, the fill keeps its boundary.
oanim.ops.layer.new()
box = layers.active
box.add_curve([(0, 0), (1, 0), (1, 1), (0, 1)], closed=True, fill=True)
oanim.ops.mesh.select_none()
box.select_points([0, 1])
assert box.set_selected_edges_visible(False) == 1 and box.hidden_edges == [(0, 0)]
assert box.shape_outline(0)[1], "the shape stays closed, so its fill is unchanged"
box.select_points([2])
assert box.set_selected_edges_visible(False) == 2 and box.hidden_edges == [(0, 0), (0, 1)]
assert box.set_selected_edges_visible(True) == 2 and box.hidden_edges == []
box.select_points([3], select=True)
box.set_selected_edges_visible(False)
assert box.hidden_edges == [(0, 0), (0, 1), (0, 2), (0, 3)], "a fully selected closed curve has four edges"
oanim.ops.layer.delete()
layers.active = layer

# Arranging shapes (Page Up/Down), on a layer of three overlapping squares, bottom to top.
oanim.ops.layer.new()
stack = layers.active
for offset in (0.0, 0.2, 0.4):
    stack.add_curve([(offset, 0), (offset + 1, 0), (offset + 1, 1), (offset, 1)], closed=True, fill=True)


def order():
    return [curves[0] for _, curves in stack.shapes]


stack.select_points([0, 1, 2, 3])
assert oanim.wm.event_simulate("PAGE_UP", area="Viewport") and order() == [1, 0, 2]
assert oanim.wm.event_simulate("PAGE_UP", shift=True, area="Viewport") and order() == [1, 2, 0]
assert oanim.ops.mesh.shape_raise() == {'CANCELLED'}, "already on top: nothing to undo"
assert oanim.wm.event_simulate("PAGE_DOWN", shift=True, area="Viewport") and order() == [0, 1, 2]
assert oanim.ops.mesh.shape_lower() == {'CANCELLED'}
stack.select_points([4, 5, 6, 7])
assert oanim.ops.mesh.shape_raise_to_front() == {'FINISHED'} and order() == [2, 0, 1], "selected shapes keep their order"
assert oanim.ops.mesh.shape_lower() == {'FINISHED'} and order() == [0, 1, 2], "the block moves down one step"
oanim.ops.ed.undo()
assert order() == [2, 0, 1]
oanim.ops.mesh.select_none()
stack.select_points([9])
assert oanim.ops.mesh.shape_raise() == {'FINISHED'} and order() == [0, 2, 1], "one selected point selects its shape"
oanim.ops.mesh.select_none()
assert oanim.ops.mesh.shape_raise() == {'CANCELLED'}, "nothing selected, nothing to arrange"
oanim.ops.layer.delete()
layers.active = layer

# --- Layers -----------------------------------------------------------------------
assert oanim.ops.layer.new_group() == {'FINISHED'}
group = layers.active
assert group.type == "GROUP" and group.name == "Group 1"
assert oanim.ops.layer.new() == {'FINISHED'}
inner = layers.active
assert inner.parent is None, "a new layer goes beside the active one, at the same level"

# Group a layer, then check the tree.
layers.active = layer
assert oanim.ops.layer.group() == {'FINISHED'}
assert layer.parent is not None and layer.parent.type == "GROUP"
holder = layer.parent

# Group With Selection: the active layer and the Ctrl-clicked ones go into one new group, top first.
oanim.ops.layer.new()
a = layers.active
oanim.ops.layer.new()
b = layers.active
a.select = True
assert oanim.ops.layer.group() == {'FINISHED'}
picked = layers.active
assert picked.type == "GROUP" and [c.name for c in picked.children] == [b.name, a.name]
assert not a.select, "the extra selection clears"

# Mask With Layer Below: b clips to a (Photoshop's clipping mask).
layers.active = b
assert oanim.ops.mask.with_layer_below() == {'FINISHED'}
assert b.masks == [] and b.clip_to_below
assert a.masking == [b.name]
layers.active = a
assert oanim.ops.mask.with_layer_below() == {'CANCELLED'}, "nothing below the bottom layer"
layers.active = picked
oanim.ops.layer.delete()

# --- References and masking ----------------------------------------------------------
layers.active = layer
assert oanim.ops.layer.make_reference() == {'FINISHED'}
ref = layers.active
assert ref.reference_source == layer and ref.points == layer.points
assert oanim.ops.mesh.select_all.poll(), "a reference's shapes can be picked (to change their looks)"

# The reference keeps its own masks while showing the original's drawing.
assert oanim.ops.mask.add(layer=str(ref.id), sources=str(holder.id)) == {'FINISHED'}
assert ref.masks[0]["layers"] == [holder.name] and layer.masks == []
oanim.ops.mask.remove(layer=str(ref.id))

# The original's edits show through the reference.
layer.add_curve([(0, 0), (0.1, 0.1)])
assert ref.point_count == layer.point_count

# Breaking the reference keeps the drawing but stops following.
assert oanim.ops.layer.break_reference() == {'FINISHED'}
assert ref.reference_source is None and ref.point_count == layer.point_count
layer.add_curve([(0, 0), (0.2, 0.2)])
assert ref.point_count == layer.point_count - 2

# Deleting an original turns its references into ordinary layers that keep the artwork.
layers.active = layer
oanim.ops.layer.make_reference()
ref2 = layers.active
layers.active = layer
assert oanim.ops.layer.delete() == {'FINISHED'}
assert ref2.reference_source is None and ref2.point_count > 0

# --- Mean Position (Point panel) and shape Position (Shape panel) ------------------------------
oanim.ops.layer.new()
pos_layer = oanim.data.layers.active
pos_layer.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True, fill=True)
pos_layer.add_curve([(3.0, 0.0), (4.0, 0.0)])
assert pos_layer.shape_position(0) == (0.5, 0.5)
oanim.ops.mesh.select_none()
assert pos_layer.mean_position is None, "nothing selected"
pos_layer.select_points([0, 1])
assert pos_layer.mean_position == (0.5, 0.0)
pos_layer.mean_position = (0.5, 0.25)
assert [p[:2] for p in pos_layer.points[:2]] == [(0.0, 0.25), (1.0, 0.25)], "both selected points move"
assert pos_layer.points[2][:2] == (1.0, 1.0), "others stay"
pos_layer.set_shape_position(1, (0.0, 2.0))
assert pos_layer.points[4][:2] == (-0.5, 2.0) and pos_layer.points[5][:2] == (0.5, 2.0)
assert pos_layer.points[0][:2] == (0.0, 0.25), "only that shape's points move"
# On a later frame, a shape move keys its points (auto-keying).
oanim.data.document.frame_current = 12
pos_layer.set_shape_position(1, (0.0, 3.0))
oanim.data.document.frame_current = 0
assert pos_layer.shape_position(1) == (0.0, 2.0), "frame 0 keeps the setup position"
oanim.data.document.frame_current = 12
assert pos_layer.shape_position(1) == (0.0, 3.0)
oanim.data.document.frame_current = 0

# --- Delete Edges (X > Edges) deletes the selected edges; points left on no curve go too ----------------------
oanim.data.document.frame_current = 0
oanim.ops.layer.new()
edge_layer = oanim.data.layers.active
edge_layer.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True, fill=True)
assert oanim.ops.mesh.select_mode_edge() == {'FINISHED'}
oanim.ops.mesh.select_none()
edge_layer.select_points([0, 1])  # the bottom edge
assert oanim.wm.event_simulate("X", area="Viewport"), "X opens the Delete menu"
assert edge_layer.point_count == 4, "the menu only opens: nothing is deleted yet"
assert oanim.ops.mesh.delete_edges() == {'FINISHED'}  # its Edges
assert edge_layer.point_count == 4, "both points of the deleted edge stay"
assert edge_layer.curve_count == 1 and edge_layer.shape_count == 1
outline, closed = edge_layer.shape_outline(0, 4)
assert not closed, "the loop opened where the edge was"
# A middle edge of a line splits it into two lines.
edge_layer.add_curve([(3.0, 0.0), (4.0, 0.0), (5.0, 0.0), (6.0, 0.0)])
oanim.ops.mesh.select_none()
edge_layer.select_points([5, 6])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert edge_layer.point_count == 8 and edge_layer.curve_count == 3, (edge_layer.point_count, edge_layer.curve_count)
# A line that was a single edge goes, and its points with it: left on no curve, they'd be loose (as Blender's
# Delete Edges takes the vertices it leaves loose). Points still on another curve stay.
oanim.ops.mesh.select_none()
edge_layer.select_points([6, 7])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert edge_layer.point_count == 6 and edge_layer.curve_count == 2, (edge_layer.point_count, edge_layer.curve_count)
# The end edge of a line: its end point goes, the line one point shorter.
edge_layer.add_curve([(3.0, 2.0), (4.0, 2.0), (5.0, 2.0)])
oanim.ops.mesh.select_none()
edge_layer.select_points([7, 8])
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert edge_layer.point_count == 8 and edge_layer.curve_count == 3, (edge_layer.point_count, edge_layer.curve_count)
assert [tuple(p[:2]) for p in edge_layer.points[6:]] == [(3.0, 2.0), (4.0, 2.0)], edge_layer.points[6:]
assert oanim.ops.mesh.select_mode_point() == {'FINISHED'}

# --- X's Delete menu: Points, Edges, Dissolve Points -----------------------------------------------
oanim.ops.layer.new()
menu_layer = oanim.data.layers.active
oanim.ops.mesh.select_none()
assert not oanim.wm.event_simulate("X", area="Viewport"), "no menu with nothing selected"
# Dissolve: the line stays one line through the gap.
menu_layer.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0), (3.0, 0.5)])
menu_layer.select_points([1, 2])
assert oanim.ops.mesh.dissolve_points() == {'FINISHED'}
assert menu_layer.point_count == 2 and menu_layer.curve_count == 1, (menu_layer.point_count, menu_layer.curve_count)
# Dissolving a corner of a filled square leaves a filled triangle.
menu_layer.add_curve([(5.0, 0.0), (6.0, 0.0), (6.0, 1.0), (5.0, 1.0)], closed=True, fill=True)
oanim.ops.mesh.select_none()
menu_layer.select_points([4])
assert oanim.ops.mesh.dissolve_points() == {'FINISHED'}
assert menu_layer.point_count == 5 and menu_layer.shape_count == 2, (menu_layer.point_count, menu_layer.shape_count)
outline, closed = menu_layer.shape_outline(1, 4)
assert closed, "the shape still closes"
# Dissolve keeps the curve's shape: the joined segment's handles are fitted to what was there. Every
# other point of an 8-point circle (and three in a row of a 12-point one) leave the outline within 1%
# of its width (it was 9% and 20% before the fit).
import math


def far(a, b):
    worst = 0.0
    for p in a:
        best = 1e9
        for i in range(len(b)):
            q, r = b[i], b[(i + 1) % len(b)]
            dx, dy = r[0] - q[0], r[1] - q[1]
            t = max(0.0, min(1.0, ((p[0] - q[0]) * dx + (p[1] - q[1]) * dy) / max(dx * dx + dy * dy, 1e-12)))
            best = min(best, math.hypot(p[0] - q[0] - t * dx, p[1] - q[1] - t * dy))
        worst = max(worst, best)
    return worst


for n, gone in ((8, [1, 3, 5, 7]), (12, [3, 4, 5])):
    oanim.ops.layer.new()
    ring = oanim.data.layers.active
    ring.add_curve([(math.cos(2 * math.pi * k / n), math.sin(2 * math.pi * k / n)) for k in range(n)], closed=True, fill=True)
    before, _ = ring.shape_outline(0, 64)
    oanim.ops.mesh.select_none()
    ring.select_points(gone)
    assert oanim.ops.mesh.dissolve_points() == {'FINISHED'} and ring.point_count == n - len(gone)
    after, _ = ring.shape_outline(0, 64)
    assert far(after, before) < 0.02, (n, far(after, before))
    oanim.ops.layer.delete()
oanim.data.layers.active = menu_layer

# Delete Points: the points go, and lines left too short with them.
oanim.ops.mesh.select_none()
menu_layer.select_points([0])
assert oanim.ops.mesh.delete_points() == {'FINISHED'}
assert menu_layer.point_count == 3 and menu_layer.curve_count == 1, (menu_layer.point_count, menu_layer.curve_count)
# Delete Edges with no edge between the selected points does nothing.
oanim.ops.mesh.select_none()
menu_layer.select_points([0])
assert oanim.ops.mesh.delete_edges() == {'CANCELLED'}

# --- Local View (/): the picked layers alone, and back ---------------------------------------------------
layers = oanim.data.layers
assert oanim.wm.local_view() == []
for l in layers.all():
    l.select = False
layers.active = menu_layer
shown_before = [l.visible for l in layers.all()]
assert oanim.wm.event_simulate("SLASH", area="Viewport"), "/ is Local View"
assert oanim.wm.local_view() == [menu_layer.name], oanim.wm.local_view()
assert [l.visible for l in layers.all()] == shown_before, "a view: no layer's visibility changes"
assert oanim.ops.view.local_view() == {'FINISHED'} and oanim.wm.local_view() == [], "again: back to everything"
# Several picked layers go in together; nothing undoes or saves it.
others = [l for l in layers.all() if l.name != menu_layer.name][:1]
for l in others:
    l.select = True
assert oanim.ops.view.local_view() == {'FINISHED'}
assert sorted(oanim.wm.local_view()) == sorted([menu_layer.name] + [l.name for l in others]), oanim.wm.local_view()
assert oanim.wm.event_simulate("NUMPAD_SLASH", area="Viewport") and oanim.wm.local_view() == []

# --- Show Active (. over the Layers editor): opens the groups the active layer is in ----------------------
oanim.ops.layer.new()
inner = layers.active
inner.name = "inner"
assert oanim.ops.layer.group() == {'FINISHED'}
middle = layers.active
assert oanim.ops.layer.group() == {'FINISHED'}
outer = layers.active
outer.expanded = False
middle.expanded = False
layers.active = inner
assert oanim.wm.event_simulate("PERIOD", area="Layers"), ". is Show Active"
assert outer.expanded and middle.expanded, "every group holding it opens"
assert oanim.wm.undo_history()[0][-1] != "Show Active", "a view, not an undo step"
assert oanim.wm.event_simulate("NUMPAD_PERIOD", area="Layers")
outer.select = False
layers.active = outer
oanim.ops.layer.delete()

print("Drawing and layer tests passed")
