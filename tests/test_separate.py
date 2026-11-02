"""Separate (P, Blender's Separate > Selection): the selected points, the curve pieces between them and
the shapes they cover go to a new layer above, in the same group, with the layer's settings; the
original loses those pieces, keeping points other pieces still use. The new layer becomes the active
one. One undo step.

Run with:  OpenAnimate --background --python tests/test_separate.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm
doc.frame_current = 0


def close(a, b, eps=1e-5):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def names():
    return [l.name for l in layers.all()]


layer = layers.active
layer.name = "Face"
layer.add_curve([(0, 0), (1, 0), (1, 1), (0, 1)], closed=True, fill=True)     # points 0-3
layer.add_curve([(2, 0), (3, 0), (3, 1), (2, 1)], closed=True, fill=True)     # points 4-7
layer.opacity = 0.5
ops.mesh.select_none()
assert ops.mesh.separate() == {'CANCELLED'}, "nothing selected"

# The second box: P takes it, fill and outline, into "Face 1" just above.
layer.select_points([4, 5, 6, 7])
assert wm.event_simulate("P", area="Viewport")
new = next(l for l in layers.all() if l.name == "Face 1")
assert names().index("Face 1") == names().index("Face") - 1, names()
assert new.point_count == 4 and new.curve_count == 1 and new.shape_count == layer.shape_count, \
    (new.point_count, new.curve_count, new.shape_count, layer.shape_count)
assert layer.point_count == 4 and layer.curve_count == 1
assert sorted(p[:2] for p in new.points) == sorted([(2, 0), (3, 0), (3, 1), (2, 1)])
assert abs(new.opacity - 0.5) < 1e-6, "the layer's settings come along"
assert not any(p[2] for p in layer.points) and not any(p[2] for p in new.points)
assert layers.active.name == "Face 1", "the new layer is the one picked"
assert oanim.wm.undo_history()[0][-1] == "Separate", oanim.wm.undo_history()[0][-1]
ops.ed.undo()
assert "Face 1" not in names() and layers.active.name == "Face" and layers.active.point_count == 8

# A piece of an open line: the segment between the two selected points goes; the line splits there,
# keeping both points (the rest still uses them).
ops.layer.new()
line = layers.active
line.name = "Line"
line.add_curve([(0, 2), (1, 2), (2, 2), (3, 2)])
ops.mesh.select_none()
line.select_points([1, 2])
assert ops.mesh.separate() == {'FINISHED'}
piece = next(l for l in layers.all() if l.name == "Line 1")
assert piece.point_count == 2 and piece.curve_count == 1
assert line.point_count == 4 and line.curve_count == 2, (line.point_count, line.curve_count)

# Inside a bone layer: the new layer sits beside it, in the same bone layer.
ops.ed.undo()
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
layers.active = line
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
line = next(l for l in layers.all() if l.name == "Line")
layers.active = line
ops.mesh.select_none()
line.select_points([0, 1])
assert ops.mesh.separate() == {'FINISHED'}
piece = next(l for l in layers.all() if l.name == "Line 1")
assert piece.parent and piece.parent.id == rig.id, piece.parent
assert layers.active.id == piece.id

# Two layers picked: the active one's new layer is active, the other's is picked, the originals aren't.
for name, x in (("Back", 5), ("Front", 7)):
    ops.layer.new()
    layers.active.name = name
    layers.active.add_curve([(x, 0), (x + 1, 0), (x + 1, 1)], closed=True, fill=True)
back = next(l for l in layers.all() if l.name == "Back")
front = next(l for l in layers.all() if l.name == "Front")
layers.active = front
back.selected = True
ops.mesh.select_all()
assert ops.mesh.separate() == {'FINISHED'}
picked = {l.name: l.selected for l in layers.all() if l.name in ("Back", "Front", "Back 1", "Front 1")}
assert layers.active.name == "Front 1", layers.active.name
assert picked == {"Back": False, "Front": False, "Back 1": True, "Front 1": False}, picked

print("separate: ok")
