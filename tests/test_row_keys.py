"""Double-clicking a timeline row keys its channels there: anim.keys_insert with the row's layers and
filter (what anim.keys_select takes) and the frame clicked, the playhead going to that frame.

Run with:  OpenAnimate --background --python tests/test_row_keys.py
"""

import oanim

layers = oanim.data.layers
doc = oanim.data.document
doc.frame_current = 0
art = layers.active
art.key_whole_drawing = False  # keyed point by point here
art.key_together = False  # unfolded: keyed channel by channel
art.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 1.0)])

KEY_LAYER_TRANSFORM = 1 << 1
KEY_POINTS = 1 << 6
ids = str(art.id)

# A row of the layer's transform: its position and scale keyed on frame 12 (and the setup key).
assert oanim.ops.anim.keys_insert(frame="12", layers=ids, kinds=str(KEY_LAYER_TRANSFORM)) == {'FINISHED'}
assert doc.frame_current == 12, "the playhead goes there"
assert [k["frame"] for k in art.keyframes("position")] == [0, 12], art.keyframes("position")
assert art.keyframes("points[0].pos") == [], "only the row's channels"
assert oanim.wm.undo_history()[0][-1] == "Insert Keys in Row"

# The Selected section's points row: only the selected points.
art.select_points([1])
assert oanim.ops.anim.keys_insert(frame="20", layers=ids, kinds=str(KEY_POINTS), selected_only="1") == {'FINISHED'}
assert [k["frame"] for k in art.keyframes("points[1].pos")] == [0, 20]
assert art.keyframes("points[0].pos") == []

# What the frame shows is what gets keyed: a point moved on frame 20 keeps its place on 30.
art.select_points([1])
oanim.ops.mesh.translate(move_x="0.5", move_y="0")
assert oanim.ops.anim.keys_insert(frame="30", layers=ids, kinds=str(KEY_POINTS), selected_only="1") == {'FINISHED'}
keys = {k["frame"]: k["value"] for k in art.keyframes("points[1].pos")}
assert 30 in keys and abs(keys[30][0] - keys[20][0]) < 1e-5, keys

# One property's row (path=).
assert oanim.ops.anim.keys_insert(frame="40", layers=ids, path="rotation") == {'FINISHED'}
assert [k["frame"] for k in art.keyframes("rotation")] == [0, 40]

# A layer's own row: everything it has.
assert oanim.ops.anim.keys_insert(frame="50", layers=ids) == {'FINISHED'}
assert 50 in [k["frame"] for k in art.keyframes("points[0].pos")]
assert 50 in [k["frame"] for k in art.keyframes("opacity")]

print("row_keys: ok")
