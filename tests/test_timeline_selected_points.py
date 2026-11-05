"""The timeline's Selected Points row (under a drawing's Points) counts the points selected in the layers
being edited: points left selected in a layer that isn't edited any more stay stored, but don't make one. Keyed as a
whole drawing, the Drawing row turns red instead.

Run with:  OpenAnimate --background --python tests/test_timeline_selected_points.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="root", head_x="0", head_y="0", tail_x="0", tail_y="1", parent="")

ops.layer.new()
drawing = layers.active
drawing.key_whole_drawing = False  # keyed point by point here
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
drawing = next(x for x in layers.all() if x.id == drawing.id)
drawing.add_curve([(0, 0), (1, 0), (1, 1)], closed=False)
layers.active = drawing

# Points keyed on frame 5, and left selected.
doc.frame_current = 5
ops.mesh.select_all()
assert ops.mesh.translate(move_x="0.2", move_y="0") == {'FINISHED'}
doc.frame_current = 6
doc.frame_current = 5
assert any(p[2] for p in drawing.points), "points are selected"


def selected_section():
    """The red rows: Selected Points while the drawing's selected points count."""
    return ["Points" for r in oanim.wm.timeline_rows() if r[1] == "Selected Points" and r[2]]


# Editing the drawing: its selected points' keys are under Selected.
layers.active = drawing
assert "Points" in selected_section(), selected_section()

# Working on the bone layer instead: the drawing's points aren't being edited (their selection is kept,
# but isn't the selection now), so nothing is selected.
layers.active = rig
oanim.data.document.timeline_only_selected = False  # (on, a bone layer's drawings show only when picked)
assert "Points" in [r[1] for r in oanim.wm.timeline_rows()], "the timeline still shows the drawing's keys (inside the bone layer)"
assert "Points" not in selected_section(), selected_section()
oanim.data.document.timeline_only_selected = True
assert any(p[2] for p in drawing.points), "the selection is kept"

# Back on the drawing, it counts again.
layers.active = drawing
assert "Points" in selected_section(), selected_section()


# Keyed as a whole drawing there is no Selected Points row: the Drawing row turns red instead, while its
# points are selected and being edited (as a picked bone's rows do).
def drawing_row():
    return next(r for r in oanim.wm.timeline_rows() if r[1] == "Drawing")


drawing.key_whole_drawing = True
assert drawing_row()[2], ("the Drawing row is red with points selected", drawing_row())
ops.mesh.select_none()
assert not drawing_row()[2], ("nothing selected: not red", drawing_row())
ops.mesh.select_all()
layers.active = rig
oanim.data.document.timeline_only_selected = False  # (on, the bone layer's drawing shows only when picked)
assert not drawing_row()[2], ("the drawing isn't being edited: not red", drawing_row())

print("timeline selected points: ok")
