"""Paste Keys onto the layer the keys came from puts each point's keys on that same point, wherever it is now: a point
deleted before it (or added) changes its place, and pasting by place put the keys on the point after it. A point that
was deleted gets none.

Run with:  OpenAnimate --background --python tests/test_paste_keys_points.py
"""

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers
ops.wm.read_homefile()
doc.auto_key = True
doc.timeline_only_selected = False
ops.layer.new()
lay = layers.active
lay.key_whole_drawing = False
lay.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])


def frames(i):
    return [k["frame"] for k in lay.keyframes(f"points[{i}].pos")]


# The middle point keyed on frame 10, copied.
doc.frame_current = 10
ops.mesh.select_none()
lay.select_points([1])
assert lay.move_points((0.0, 0.5))
doc.frame_current = 11
assert frames(1) == [0, 10] and frames(0) == [] and frames(2) == [], (frames(0), frames(1), frames(2))
assert ops.anim.keys_select(layers=str(lay.id), frame_start="10", frame_end="10") == {'FINISHED'}
assert ops.anim.keys_copy() == {'FINISHED'}

# The first point deleted: the middle one is now the first.
doc.frame_current = 0
ops.mesh.select_none()
lay.select_points([0])
assert ops.mesh.delete_points() == {'FINISHED'}
doc.frame_current = 20
assert ops.anim.keys_paste() == {'FINISHED'}
assert frames(0) == [0, 10, 20], ("the copied point's keys on it", frames(0))
assert 20 not in frames(1), ("not on the point that took its place", frames(1))

# Deleted itself: nothing to paste onto.
doc.frame_current = 0
ops.mesh.select_none()
lay.select_points([0])
assert ops.mesh.delete_points() == {'FINISHED'}
doc.frame_current = 30
ops.anim.keys_paste()
assert 30 not in frames(0), ("a deleted point's keys go nowhere", frames(0))

print("test_paste_keys_points: all passed")
