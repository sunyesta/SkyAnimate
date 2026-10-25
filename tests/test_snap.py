"""Snap (Shift S, Blender's pie): the 2D cursor to the origin, the selection's middle, the active element
or the grid; the selection to the cursor (each point onto it, or its middle with Offset), the active
element or the grid. Points, picked bones and an active Media layer.

Run with:  OpenAnimate --background --python tests/test_snap.py
"""

import os

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm
doc.frame_current = 0


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def xy(l, i):
    return tuple(l.points[i][:2])


layer = layers.active
layer.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True, fill=True)
layer.location_x = 0.5  # the layer moved: snapping works where things show
layer.select_points(range(4))
assert ops.mesh.translate(move_x="0") == {'FINISHED'}

# The cursor: to the selection's middle (where it shows), the origin, the grid.
assert ops.view.snap_cursor_to_selected() == {'FINISHED'}
assert close(wm.cursor(), (1.0, 0.5)), wm.cursor()
assert ops.view.snap_cursor_to_center() == {'FINISHED'}
assert close(wm.cursor(), (0.0, 0.0))
wm.set_cursor((0.23, -0.37))
assert ops.view.snap_cursor_to_grid() == {'FINISHED'}
assert close(wm.cursor(), (0.2, -0.4)), wm.cursor()

# Selection to Cursor, Keep Offset: the shape moves whole, its middle on the cursor.
wm.set_cursor((2.0, 2.0))
assert ops.view.snap_selected_to_cursor(use_offset="1") == {'FINISHED'}
assert close(xy(layer, 0), (2.0 - 0.5 - 0.5, 1.5)) and close(xy(layer, 2), (2.0 - 0.5 + 0.5, 2.5)), layer.points
ops.ed.undo()
# Without: every selected point onto the cursor.
layer.select_points(range(4), False)
layer.select_points([1, 2])
assert ops.view.snap_selected_to_cursor() == {'FINISHED'}
assert close(xy(layer, 1), (1.5, 2.0)) and close(xy(layer, 2), (1.5, 2.0)), layer.points
assert close(xy(layer, 0), (0.0, 0.0)), "unselected points stay"
ops.ed.undo()

# Selection to Grid: each point to its nearest grid point.
layer.select_points(range(4))
assert ops.mesh.translate(move_x="0.03", move_y="0.04") == {'FINISHED'}
assert ops.view.snap_selected_to_grid() == {'FINISHED'}
assert close(xy(layer, 0), (0.0, 0.0)) and close(xy(layer, 2), (1.0, 1.0)), layer.points

# Active: the active shape's middle.
layer.set_active_shape(0)
assert ops.view.snap_cursor_to_active() == {'FINISHED'}
assert close(wm.cursor(), (1.0, 0.5)), wm.cursor()

# Bones: the middle of the picked bones' bases; Selection to Cursor moves them together.
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="leg", head_x="0", head_y="-1", tail_x="1", tail_y="-1", parent="")
ops.armature.select_all()
assert ops.view.snap_cursor_to_selected() == {'FINISHED'}
assert close(wm.cursor(), (0.0, -0.5)), wm.cursor()
wm.set_cursor((1.0, 1.0))
assert ops.view.snap_selected_to_cursor() == {'FINISHED'}
arm = next(b for b in rig.bones if b["name"] == "arm")
assert close(arm["head"], (1.0, 1.5)), arm["head"]

# A Media layer: its centre to the cursor.
here = os.path.dirname(os.path.abspath(__file__))
assert ops.media.add(filepath=os.path.join(here, "data", "media", "still.png")) == {'FINISHED'}
media = layers.active
wm.set_cursor((0.4, -0.3))
assert ops.view.snap_selected_to_cursor() == {'FINISHED'}
assert close(media.to_document((0.0, 0.0)), (0.4, -0.3))

# Nothing selected: the selection snaps can't run; the cursor ones say so.
ops.layer.new()
assert ops.view.snap_selected_to_cursor() == {'CANCELLED'}
assert ops.view.snap_cursor_to_selected() == {'CANCELLED'}

print("snap: ok")
