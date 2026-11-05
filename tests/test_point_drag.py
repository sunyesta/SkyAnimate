"""Dragging a point in the Select tool moves it as G does (Blender's tweak); a click still only picks. What a press
grabs: the nearest shown point within reach, or in Edges mode the segment under it (its two points); nothing with Shift
or Ctrl held, or on empty space (those drags box-select). Dragging an unpicked point picks it alone first; dragging
one already picked moves the whole selection."""

import oanim

ops, layers, wm = oanim.ops, oanim.data.layers, oanim.wm

ops.layer.new()
art = layers.active
art.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
art.add_curve([(0.0, 3.0), (1.0, 3.0)])
me = art.id
ops.mesh.select_none()

# --- What a press grabs ---------------------------------------------------------------------------------------------
assert wm.select_press_grab(1.05, 0.02) == [(me, [1])], ("the point under it", wm.select_press_grab(1.05, 0.02))
assert wm.select_press_grab(0.97, 3.0) == [(me, [4])], "the nearest one"
assert wm.select_press_grab(5.0, 5.0) == [], "empty space: a box"
assert wm.select_press_grab(0.5, 0.0) == [], "a line in Points mode: a box, as a click picks nothing there"
assert wm.select_press_grab(1.0, 0.0, shift=True) == [], "Shift: a box that adds"
assert wm.select_press_grab(1.0, 0.0, ctrl=True) == [], "Ctrl: a box that takes away"
assert wm.select_press_grab(1.5, 0.0, zoom=1000.0) == [], "out of reach when zoomed in"

# --- Dragging: unpicked, it alone; picked, the whole selection -------------------------------------------------------
art.select_points([0, 2])
assert wm.select_press_grab(1.0, 0.0, drag=True) == [(me, [1])], "an unpicked point is picked alone, then moves"
ops.mesh.select_none()
art.select_points([0, 2])
moving = wm.select_press_grab(0.0, 0.0, drag=True)
assert moving == [(me, [0, 2])], ("a picked point brings the whole selection", moving)

# --- Edges mode: the segment under the press, both its points ---------------------------------------------------------
assert ops.mesh.select_mode_edge() == {'FINISHED'}
ops.mesh.select_none()
assert wm.select_press_grab(0.5, 0.01) == [(me, [0, 1])], wm.select_press_grab(0.5, 0.01)
assert wm.select_press_grab(1.5, 0.0, drag=True) == [(me, [1, 2])], "dragging an edge picks and moves its two points"
assert wm.select_press_grab(5.0, 5.0) == [], "empty space: a box"
ops.mesh.select_mode_point()

print("Point drag tests passed")
