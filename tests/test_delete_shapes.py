"""mesh.delete_shapes: delete shapes by name or id (the Shapes list's right-click Delete Shape), keeping
their curves and points for the other shapes on them.

Run with:  OpenAnimate --background --python tests/test_delete_shapes.py
"""

import oanim

layers = oanim.data.layers
layer = layers.active
layer.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True, fill=True)
layer.add_curve([(2.0, 0.0), (3.0, 0.0), (3.0, 1.0)], closed=True, fill=True)
layer.add_curve([(4.0, 0.0), (5.0, 0.0), (5.0, 1.0)], closed=True, fill=True)
names = [name for name, _ in layer.shapes]
assert len(names) == 3, names
keep = names[-1]
points = layer.point_count
# An undo step with the shapes there (add_curve isn't one).
assert oanim.ops.mesh.select_all() == {'FINISHED'}

assert oanim.ops.mesh.delete_shapes(shapes="nope") == {'CANCELLED'}, "an unknown name changes nothing"
assert [n for n, _ in layer.shapes] == names
assert oanim.ops.mesh.delete_shapes(shapes="|".join(names[:-1])) == {'FINISHED'}
assert [n for n, _ in layer.shapes] == [keep], layer.shapes
assert layer.point_count == points, "the curve and its points stay"
assert oanim.wm.undo_history()[0][-1] in ("Delete Shape", "Delete Shapes"), oanim.wm.undo_history()[0][-1]
oanim.ops.ed.undo()
assert [n for n, _ in layer.shapes] == names, "one undo step"

print("delete_shapes: ok")
