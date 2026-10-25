"""Setup-only tools (Draw, Shape, Split Curve, Weight Select, Weight Paint) are picked on the setup frame
(0); away from it, asking for one says so ("press Tab") and leaves the tool as it was, and a setup-only
tool already picked stays picked (greyed out in the tool bar) rather than handing over to Select.

Run with:  OpenAnimate --background --python tests/test_tool_frames.py
"""

import oanim

doc = oanim.data.document
oanim.ops.layer.new()
assert doc.frame_current == 0
assert oanim.ops.wm.tool_draw() == {'FINISHED'} and oanim.wm.tool() == "DRAW"

doc.frame_current = 5
# Already picked: it stays (the tool bar greys it out and a click in the viewport says why).
assert oanim.wm.tool() == "DRAW"
assert oanim.ops.wm.tool_select() == {'FINISHED'} and oanim.wm.tool() == "SELECT"
for op in (oanim.ops.wm.tool_draw, oanim.ops.wm.tool_shape, oanim.ops.wm.tool_split_curve,
           oanim.ops.wm.tool_split_curve_temporary, oanim.ops.wm.tool_weight_select, oanim.ops.wm.tool_weight_paint):
    assert op() == {'CANCELLED'}, op
    assert oanim.wm.tool() == "SELECT"
# Tools for any frame still switch.
assert oanim.ops.wm.tool_transform_points() == {'FINISHED'} and oanim.wm.tool() == "TRANSFORM_POINTS"

doc.frame_current = 0
assert oanim.ops.wm.tool_shape() == {'FINISHED'} and oanim.wm.tool() == "SHAPE"
# Transform Layer (any layer) and Edit Text (a text layer's) work on any frame.
doc.frame_current = 5
assert oanim.ops.wm.tool_transform_layer() == {'FINISHED'} and oanim.wm.tool() == "TRANSFORM_LAYER"
# Its Show Grid Lines: on at first, switched from Python.
assert oanim.wm.layer_grid_lines() is True
assert oanim.wm.layer_grid_lines(False) is False and oanim.wm.layer_grid_lines(True) is True
assert oanim.ops.layer.new_text(text="Hi") == {'FINISHED'}
assert oanim.ops.wm.tool_edit_text() == {'FINISHED'} and oanim.wm.tool() == "EDIT_TEXT"
doc.frame_current = 0
print("tool frames: ok")
