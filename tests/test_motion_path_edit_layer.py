"""The Motion Path tool's Edit Layer (its toolbar's checkbox, edit_layer= on anim.motion_path_brush_set): on, the layer
itself is edited: its origin's path alone shows, clicks never pick its bones or points, the Transform Layer box's edits
are keyed whatever Auto Keying says, and the timeline's teal is on its transform's stretch. Off (the default): its
bones and points, the teal never on the layer's own position. Kept in the tool settings.

Run with:  OpenAnimate --background --python tests/test_motion_path_edit_layer.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm

doc.motion_paths = True
doc.path_before, doc.path_after = 3, 3
doc.auto_key = False

ops.layer.new()
ball = layers.active
ball.name = "Ball"
ball.add_curve([(0.0, 0.0), (0.5, 0.0)])
ball.key_whole_drawing = False
ball.key_together = False  # unfolded: keyed channel by channel
for f, x in ((10, 0.0), (20, 1.0)):
    assert ops.anim.motion_path_move(frame=str(f), x=str(x), y="0", layer=str(ball.id)) == {'FINISHED'}
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert wm.path_brush_settings()["edit_layer"] is False, "off by default"


def labels():
    return [p["label"] for p in wm.motion_paths()]


# --- Off: the layer's position is never shaded teal; its points are what's picked ---------------------------------
doc.frame_current = 15
assert labels() == ["Ball"], labels()
assert wm.timeline_key_header()["mode"] != "PATH", ("off: no teal on the layer's position", wm.timeline_key_header())
ball.select_points([0])
traced = labels()
assert traced and "Ball" not in traced, ("off: the picked point's path", traced)
ops.mesh.select_none()
assert wm.motion_path_pick(ball.location_x, 0.0, 100.0, False, False), "off: a click on a point picks it"

# --- On -------------------------------------------------------------------------------------------------------------
assert ops.anim.motion_path_brush_set(edit_layer="1") == {'FINISHED'}
assert wm.path_brush_settings()["edit_layer"] is True
ball.select_points([0])
assert labels() == ["Ball"], ("on: the layer's origin alone, whatever points are picked", labels())
h = wm.timeline_key_header()
assert h["mode"] == "PATH" and h["label"].startswith("Ball "), ("on: teal on the layer's stretch", h)
assert not wm.motion_path_pick(ball.location_x, 0.0, 100.0, False, True), "on: clicks never pick points"

# The box's edits keyed, Auto Keying off.
doc.frame_current = 15
ball.rotation = 0.5
wm.layer_gizmo_finish("Rotate Layer")
assert 15 in [k["frame"] for k in ball.keyframes("rotation")], ("keyed on frame 15", ball.keyframes("rotation"))
doc.frame_current = 10
doc.frame_current = 15
assert abs(ball.rotation - 0.5) < 1e-5, "kept there"

# Kept in the tool settings: picking another brush leaves it on.
# G R S and Snap work on the layer itself, as the Transform Layer tool's, never the points picked.
ball.select_points([0])
points_was = [tuple(p[:2]) for p in ball.points]
loc_was = (ball.location_x, ball.location_y)
wm.set_cursor((0.3, 0.4))
assert ops.view.snap_selected_to_cursor() == {'FINISHED'}
assert [tuple(p[:2]) for p in ball.points] == points_was, "on: Snap leaves the points as they were"
assert (ball.location_x, ball.location_y) != loc_was, ("on: Snap moves the layer", loc_was)
ops.ed.undo()
ops.mesh.select_none()
assert wm.event_simulate("G", area="Viewport"), "on: G moves the layer with no point picked"
assert ops.anim.motion_path_brush_set(brush="GRAB") == {'FINISHED'}
assert wm.path_brush_settings()["edit_layer"] is True
assert ops.anim.motion_path_brush_set(brush="EDIT", edit_layer="0") == {'FINISHED'}
assert wm.path_brush_settings()["edit_layer"] is False
print("Motion path edit layer: ok")
