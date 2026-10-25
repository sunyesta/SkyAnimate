"""Editing on a detail level: the detail points the shape's level shows are edited as points of their
own, so moving a coarser point there leaves them where they are; below their level they ride the
coarser curve as before. Playing keyed coarse moves still carries them, whatever level is shown.

Run with:  OpenAnimate --background --python tests/test_detail_edit_level.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True


def close(a, b, eps=1e-5):
    return all(abs(x - y) < eps for x, y in zip(a, b))


layer = layers.active
layer.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True, fill=True)
ops.mesh.select_all()
assert ops.mesh.detail_level_set(level="1") == {'FINISHED'}
d1 = layer.split_segment(0, 0, 0.5)
assert layer.detail["levels"][d1] == 1, layer.detail
ops.mesh.select_all()
assert ops.mesh.detail_level_set(level="2") == {'FINISHED'}
d2 = layer.split_segment(0, 1, 0.5)  # between d1 and point 1: level 2
assert layer.detail["levels"][d2] == 2, layer.detail


def where(i):
    return tuple(layer.points[i][:2])


def move_point(i, x, y):
    ops.mesh.select_none()
    layer.select_points([i])
    assert ops.mesh.translate(move_x=str(x), move_y=str(y)) == {'FINISHED'}


def level(n):
    ops.mesh.select_all()
    ops.mesh.detail_level_set(level=str(n))
    assert layer.detail["shape_levels"] == [n], layer.detail


# On level 2 (everything shown): moving base point 1 leaves both detail points where they are.
level(2)
a, b = where(d1), where(d2)
move_point(1, 0.0, 0.3)
assert close(where(d1), a) and close(where(d2), b), (where(d1), a, where(d2), b)
ops.ed.undo()

# On level 1: the level 1 point stays; the level 2 point (hidden) rides its segment.
level(1)
a, b = where(d1), where(d2)
move_point(1, 0.0, 0.3)
assert close(where(d1), a), (where(d1), a)
assert not close(where(d2), b), "hidden detail rides"
ops.ed.undo()

# On level 0: the detail rides, as before.
level(0)
a = where(d1)
move_point(1, 0.0, 0.3)
assert not close(where(d1), a)
ops.ed.undo()

# Keyed on level 0 (the detail riding), then played back on level 2: it still rides the keys.
doc.frame_current = 10
move_point(1, 0.0, 0.3)
keyed = where(d1)
doc.frame_current = 0
level(2)
rest = where(d1)
doc.frame_current = 10
assert close(where(d1), keyed), (where(d1), keyed)
doc.frame_current = 5
assert not close(where(d1), rest) and not close(where(d1), keyed), "between the keys it rides part way"

print("detail_edit_level: ok")
