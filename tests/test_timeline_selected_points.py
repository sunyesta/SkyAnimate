"""The timeline's Selected section counts the points selected in the layers being edited: points left
selected in a layer that isn't edited any more stay stored, but don't put a Points row under Selected.

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
    rows = oanim.wm.timeline_rows()
    names = [r[1] for r in rows]
    if "Selected" not in names:
        return []
    at = names.index("Selected")
    depth = rows[at][0]
    out = []
    for r in rows[at + 1:]:
        if r[0] <= depth:
            break
        out.append(r[1])
    return out


# Editing the drawing: its selected points' keys are under Selected.
layers.active = drawing
assert "Points" in selected_section(), selected_section()

# Working on the bone layer instead: the drawing's points aren't being edited (their selection is kept,
# but isn't the selection now), so nothing is selected.
layers.active = rig
assert "Points" in [r[1] for r in oanim.wm.timeline_rows()], "the timeline still shows the drawing's keys (under All)"
assert "Points" not in selected_section(), selected_section()
assert any(p[2] for p in drawing.points), "the selection is kept"

# Back on the drawing, it counts again.
layers.active = drawing
assert "Points" in selected_section(), selected_section()

print("timeline selected points: ok")
