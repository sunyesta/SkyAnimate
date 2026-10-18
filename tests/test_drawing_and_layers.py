"""Milestone 2 data: vector drawings, shapes, curve editing, layers, groups, references, masking.

Run with:  SkyAnimate --background --python tests/test_drawing_and_layers.py
"""

import sky

layers = sky.data.layers
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
sky.ops.mesh.select_none()
layer.select_points([4, 5, 6])
assert sky.ops.mesh.peak() == {'FINISHED'}
sky.ops.mesh.select_none()
layer.select_points([5])
assert sky.ops.mesh.delete() == {'FINISHED'}
assert counts(layer) == (6, 2, 1), "deleting a point drops shapes on that curve"

# Create Shape from a selected closed loop: select the square and press F over the viewport.
sky.ops.mesh.select_none()
layer.select_points([0, 1, 2, 3])
assert sky.wm.event_simulate("F", area="Viewport")
assert layer.shape_count == 2

# Keys over the viewport: A selects all, Alt A deselects, X deletes.
assert sky.wm.event_simulate("A", area="Viewport")
assert all(p[2] for p in layer.points)
assert sky.wm.event_simulate("A", alt=True, area="Viewport")
assert not any(p[2] for p in layer.points)
assert sky.wm.event_simulate("2", area="Viewport"), "2 switches to edge selection"
assert sky.wm.event_simulate("3", area="Viewport"), "3 switches to stroke selection"
layer.select_points([0])
assert sky.ops.mesh.select_mode_stroke() == {'FINISHED'}
assert sum(p[2] for p in layer.points) == 4, "stroke mode selects the whole square"
sky.ops.mesh.select_mode_point()

# Undo restores deleted geometry.
before = counts(layer)
assert sky.wm.event_simulate("X", area="Viewport")
assert layer.point_count == before[0] - 4
sky.ops.ed.undo()
assert counts(layer) == before

# Duplicate (Shift D) copies the selected curve and its shapes, and selects the copy.
before = counts(layer)
sky.ops.mesh.select_none()
layer.select_points([0, 1, 2, 3])
assert sky.wm.event_simulate("D", shift=True, area="Viewport")
assert counts(layer) == (before[0] + 4, before[1] + 1, before[2] + 2), counts(layer)
assert [p[2] for p in layer.points[-4:]] == [True] * 4 and not any(p[2] for p in layer.points[:-4])
sky.ops.ed.undo()
assert counts(layer) == before
sky.ops.mesh.select_none()
layer.select_points([0])
assert sky.ops.mesh.duplicate_move() == {'CANCELLED'}, "one point has no curve piece to copy"

# Point radius: 1 by default; Mean Radius moves every selected point by the same amount.
sky.ops.mesh.select_none()
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
sky.ops.mesh.select_none()

# Corner type: Round by default; Sharp for the selected points only.
sky.ops.mesh.select_none()
assert all(c == "ROUND" for c in layer.corners)
layer.select_points([1, 2])
layer.set_selected_corner("SHARP")
assert layer.corners[1:3] == ["SHARP", "SHARP"] and layer.corners[0] == "ROUND"
try:
    layer.set_selected_corner("BEVEL")
    raise AssertionError("only ROUND and SHARP")
except ValueError:
    pass
layer.set_selected_corner("ROUND")
sky.ops.mesh.select_none()

# Hide Edge: the outline skips a hidden segment, the fill keeps its boundary.
sky.ops.layer.new()
box = layers.active
box.add_curve([(0, 0), (1, 0), (1, 1), (0, 1)], closed=True, fill=True)
sky.ops.mesh.select_none()
box.select_points([0, 1])
assert box.set_selected_edges_visible(False) == 1 and box.hidden_edges == [(0, 0)]
assert box.shape_outline(0)[1], "the shape stays closed, so its fill is unchanged"
box.select_points([2])
assert box.set_selected_edges_visible(False) == 2 and box.hidden_edges == [(0, 0), (0, 1)]
assert box.set_selected_edges_visible(True) == 2 and box.hidden_edges == []
box.select_points([3], select=True)
box.set_selected_edges_visible(False)
assert box.hidden_edges == [(0, 0), (0, 1), (0, 2), (0, 3)], "a fully selected closed curve has four edges"
sky.ops.layer.delete()
layers.active = layer

# Arranging shapes (Page Up/Down), on a layer of three overlapping squares, bottom to top.
sky.ops.layer.new()
stack = layers.active
for offset in (0.0, 0.2, 0.4):
    stack.add_curve([(offset, 0), (offset + 1, 0), (offset + 1, 1), (offset, 1)], closed=True, fill=True)


def order():
    return [curves[0] for _, curves in stack.shapes]


stack.select_points([0, 1, 2, 3])
assert sky.wm.event_simulate("PAGE_UP", area="Viewport") and order() == [1, 0, 2]
assert sky.wm.event_simulate("PAGE_UP", shift=True, area="Viewport") and order() == [1, 2, 0]
assert sky.ops.mesh.shape_raise() == {'CANCELLED'}, "already on top: nothing to undo"
assert sky.wm.event_simulate("PAGE_DOWN", shift=True, area="Viewport") and order() == [0, 1, 2]
assert sky.ops.mesh.shape_lower() == {'CANCELLED'}
stack.select_points([4, 5, 6, 7])
assert sky.ops.mesh.shape_raise_to_front() == {'FINISHED'} and order() == [2, 0, 1], "selected shapes keep their order"
assert sky.ops.mesh.shape_lower() == {'FINISHED'} and order() == [0, 1, 2], "the block moves down one step"
sky.ops.ed.undo()
assert order() == [2, 0, 1]
sky.ops.mesh.select_none()
stack.select_points([9])
assert sky.ops.mesh.shape_raise() == {'FINISHED'} and order() == [0, 2, 1], "one selected point selects its shape"
sky.ops.mesh.select_none()
assert sky.ops.mesh.shape_raise() == {'CANCELLED'}, "nothing selected, nothing to arrange"
sky.ops.layer.delete()
layers.active = layer

# --- Layers -----------------------------------------------------------------------
assert sky.ops.layer.new_group() == {'FINISHED'}
group = layers.active
assert group.type == "GROUP" and group.name == "Group 1"
assert sky.ops.layer.new() == {'FINISHED'}
inner = layers.active
assert inner.parent is None, "a new layer goes beside the active one, at the same level"

# Group a layer, then check the tree.
layers.active = layer
assert sky.ops.layer.group() == {'FINISHED'}
assert layer.parent is not None and layer.parent.type == "GROUP"
holder = layer.parent

# --- References and masking ----------------------------------------------------------
layers.active = layer
assert sky.ops.layer.make_reference() == {'FINISHED'}
ref = layers.active
assert ref.reference_source == layer and ref.points == layer.points
assert not sky.ops.mesh.select_all.poll(), "a reference's drawing is edited through its original"

# The reference keeps its own masking while showing the original's drawing.
holder.group_mask = "HIDE_ALL"
ref.mask_mode = "ADD_INVISIBLE"
assert ref.mask_mode == "ADD_INVISIBLE" and layer.mask_mode == "MASKED"
try:
    ref.mask_mode = "SIDEWAYS"
    raise AssertionError("unknown enum values are rejected")
except ValueError:
    pass

# The original's edits show through the reference.
layer.add_curve([(0, 0), (0.1, 0.1)])
assert ref.point_count == layer.point_count

# Breaking the reference keeps the drawing but stops following.
assert sky.ops.layer.break_reference() == {'FINISHED'}
assert ref.reference_source is None and ref.point_count == layer.point_count
layer.add_curve([(0, 0), (0.2, 0.2)])
assert ref.point_count == layer.point_count - 2

# Deleting an original turns its references into ordinary layers that keep the artwork.
layers.active = layer
sky.ops.layer.make_reference()
ref2 = layers.active
layers.active = layer
assert sky.ops.layer.delete() == {'FINISHED'}
assert ref2.reference_source is None and ref2.point_count > 0

print("Drawing and layer tests passed")
