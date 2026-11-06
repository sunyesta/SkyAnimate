"""The timeline's Summary row and a lone layer's row are one ("Summary / Ball 1") when they show the same keys: the
timeline shows that layer alone. Another layer shown, a pinned property or the camera keeps them apart.

Run with:  OpenAnimate --background --python tests/test_timeline_summary_merge.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm

doc.auto_key = True
ops.layer.new()
ball = layers.active
ball.name = "Ball 1"
ball.add_curve([(0.0, 0.0), (0.5, 0.0)])
doc.frame_current = 10
ball.location_x = 1.0
doc.frame_current = 0


def top():
    return [(r["depth"], r["label"], r["summary"], r["layer"]) for r in wm.timeline_row_details() if r["depth"] == 0]


rows = top()
assert rows == [(0, "Summary / Ball 1", True, ball.id)], rows
merged = wm.timeline_row_details()[0]
assert 10 in merged["frames"], merged
assert any(r["depth"] == 1 and r["name"] == "Position" for r in wm.timeline_row_details()), \
    "its channels right under it"

# A second layer shown: Summary on its own again.
ops.layer.new()
other = layers.active
for l in layers:
    l.selected = True
other.add_curve([(0.0, 1.0), (0.5, 1.0)])
doc.frame_current = 20
other.location_y = 2.0
doc.frame_current = 0
names = [r[1] for r in top()]
assert names[0] == "Summary" and "Ball 1" in names, names

# Only Selected with Ball 1 picked: it alone again, merged.
doc.timeline_only_selected = True
for l in layers:
    l.selected = l.id == ball.id
layers.active = ball
assert top() == [(0, "Summary / Ball 1", True, ball.id)], top()

# A pinned property keeps Summary apart.
assert ball.pin_channel("opacity") == {'FINISHED'}
names = [r[1] for r in top()]
assert names[:2] == ["Summary", "Pinned"] and "Ball 1" in names, names
print("Timeline summary merge: ok")
