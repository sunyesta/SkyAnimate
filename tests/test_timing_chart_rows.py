"""The timeline's timing chart is one row, shown only while a stretch is picked (teal): the path last picked on, with all
its ticks. Nothing picked (the playhead's stretch teal is not a pick), picks in different stretches or the tool put
away: hidden.

Run with:  OpenAnimate --background --python tests/test_timing_chart_rows.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def mover(name, y):
    ops.layer.new()
    lay = layers.active
    lay.name = name
    lay.add_curve([(0.0, 0.0), (0.1, 0.0)])
    lay.key_whole_drawing = False
    for f, x in ((10, 0.0), (20, 1.0), (30, 2.0)):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(lay.id)) == {'FINISHED'}
    return lay


doc.auto_key = False
doc.motion_paths = True
doc.path_before, doc.path_after = 40, 40
ball = mover("Ball", 0.0)
other = mover("Other", 1.0)
for lay in layers:
    lay.selected = lay.id in (ball.id, other.id)
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert ops.anim.motion_path_brush_set(edit_layer="1") == {'FINISHED'}
doc.frame_current = 15

assert wm.timeline_key_header()["mode"] == "PATH", ("the playhead's stretch is teal", wm.timeline_key_header())
assert not wm.timeline_key_header()["picked"]
assert wm.timing_chart_rows() == [], ("nothing picked: hidden, the playhead's stretch is not a pick",
                                      wm.timing_chart_rows())
assert ops.anim.motion_path_select(frame="15", layer=str(ball.id)) == {'FINISHED'}
assert wm.timing_chart_rows() == ["Ball"], ("a dot picked: its path's row alone", wm.timing_chart_rows())
assert ops.anim.motion_path_select(frame="16", layer=str(ball.id), extend="1") == {'FINISHED'}
assert wm.timing_chart_rows() == ["Ball"], "more of the same stretch"
assert ops.anim.motion_path_select(frame="25", layer=str(ball.id), extend="1") == {'FINISHED'}
assert wm.timing_chart_rows() == [], ("picks in two stretches: no teal, hidden", wm.timing_chart_rows())
assert ops.anim.motion_path_select(frame="25", layer=str(other.id)) == {'FINISHED'}
assert wm.timing_chart_rows() == ["Other"], ("the path last picked on", wm.timing_chart_rows())
assert ops.anim.motion_path_select(none="1") == {'FINISHED'}
assert wm.timing_chart_rows() == [], "picked nothing: hidden again"
assert ops.anim.motion_path_select(frame="15", layer=str(ball.id)) == {'FINISHED'}
assert wm.timing_chart_rows() == ["Ball"]
assert ops.wm.tool_select() == {'FINISHED'}
assert wm.timing_chart_rows() == [], "without the Motion Path tool: hidden"

print("timing chart rows: ok")
