"""Shift A, Blender's Add menu on a drawing: a Point, Line, Square or Circle at the 2D cursor.

Run with:  OpenAnimate --background --python tests/test_add_menu.py
"""

import oanim

layer = oanim.data.layers.active
assert layer.type == "VECTOR"


def close(a, b):
    return abs(a[0] - b[0]) < 1e-4 and abs(a[1] - b[1]) < 1e-4


# Shift A takes the key on a drawing (on a bone layer Add Bone does).
assert oanim.wm.event_simulate("A", shift=True, area="Viewport")

# Point: one point, no curve, selected.
assert oanim.ops.mesh.primitive_point_add(x=0.3, y=-0.2) == {'FINISHED'}
assert (layer.point_count, layer.curve_count, layer.shape_count) == (1, 0, 0)
assert close(layer.points[0], (0.3, -0.2)) and layer.points[0][2]

# Line: two points, an open curve and a line-only shape, its middle at x, y.
assert oanim.ops.mesh.primitive_line_add(x=1.0, y=0.5, length=0.5) == {'FINISHED'}
assert (layer.point_count, layer.curve_count, layer.shape_count) == (3, 1, 1)
assert close(layer.points[1], (0.75, 0.5)) and close(layer.points[2], (1.25, 0.5))
assert [p[2] for p in layer.points] == [False, True, True], "the new line becomes the selection"
style = layer.shape_style(0)
assert style["has_outline"] and not style["has_fill"]
assert oanim.ops.mesh.primitive_line_add(x=0, y=0, length=1, angle=90) == {'FINISHED'}
assert close(layer.points[3], (0, -0.5)) and close(layer.points[4], (0, 0.5))

# Square and Circle: the Shape tool's box and circle, a given size about x, y.
assert oanim.ops.mesh.primitive_box_add(x=-1, y=0, width=0.5, height=0.5) == {'FINISHED'}
xs = [p[0] for p in layer.points[-4:]]
ys = [p[1] for p in layer.points[-4:]]
assert close((min(xs), min(ys)), (-1.25, -0.25)) and close((max(xs), max(ys)), (-0.75, 0.25))
assert oanim.ops.mesh.primitive_circle_add(x=0, y=1, width=0.5, height=0.5) == {'FINISHED'}
assert layer.curve_count == 4 and layer.shape_count == 4

# Undo takes each back.
oanim.ops.ed.undo()
assert layer.curve_count == 3

# Only on the setup frame, as drawing tools are.
oanim.data.document.frame_current = 5
assert oanim.ops.mesh.primitive_point_add(x=0, y=0) == {'CANCELLED'}
oanim.data.document.frame_current = 0

# A drawing inside a bone layer: Shift A is still its Add menu, not Add Bone (that is for the bone layer itself).
layers = oanim.data.layers
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
assert oanim.ops.layer.new() == {'FINISHED'}
inner = layers.active
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
layers.active = inner
bones_before = len(rig.bones)
assert not oanim.ops.armature.bone_primitive_add.poll(), "Add Bone waits for the bone layer itself"
assert oanim.wm.event_simulate("A", shift=True, area="Viewport")
assert len(rig.bones) == bones_before, ("no bone added", rig.bones)
assert oanim.ops.mesh.primitive_point_add(x=0.1, y=0.1) == {'FINISHED'} and inner.point_count == 1
layers.active = rig
assert oanim.wm.event_simulate("A", shift=True, area="Viewport")
assert len(rig.bones) == bones_before + 1, "on the bone layer it adds a bone"

print("Add menu tests passed")
