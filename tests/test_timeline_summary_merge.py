"""The timeline's Summary row is always its own row, never merged into a lone layer's ("Summary / Ball 1" is gone):
it shows every key, never folds and keys nothing together, the one view of the whole shot's timing. A lone layer gets
its own row under it, a heading while open (no keys of its own), its keys on it once folded (Key Together).

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


def summary():
    return next(r for r in wm.timeline_row_details(True) if r["summary"])


# A lone layer, open: the Summary with every key, then the layer's own row, a heading.
assert top() == [(0, "Summary", True, 0), (0, "Ball 1", False, ball.id)], top()
assert 10 in summary()["frames"] and summary()["fold"] == "", ("every key, no fold arrow", summary())
assert wm.timeline_row_details()[1]["frames"] == [], "the layer open: no keys of its own"
assert any(r["depth"] == 1 and r["name"] == "Position" for r in wm.timeline_row_details()), \
    "its channels right under it"
assert not wm.timeline_row_fold(0, False), "the Summary doesn't fold"

# Folded (Key Together on): still two rows, the layer's with its keys.
assert wm.timeline_row_fold(1, False)
assert ball.key_together
assert top() == [(0, "Summary", True, 0), (0, "Ball 1", False, ball.id)], top()
assert 10 in wm.timeline_row_details()[1]["frames"]
assert wm.timeline_row_fold(1, True) and not ball.key_together

# A second layer shown: the Summary over both.
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
assert summary()["frames"] == [0, 10, 20], summary()

# Only Selected with Ball 1 picked: the Summary over it alone, still apart.
doc.timeline_only_selected = True
for l in layers:
    l.selected = l.id == ball.id
layers.active = ball
assert top() == [(0, "Summary", True, 0), (0, "Ball 1", False, ball.id)], top()

# A pinned property: Summary, Pinned, then the layer.
assert ball.pin_channel("opacity") == {'FINISHED'}
names = [r[1] for r in top()]
assert names[:2] == ["Summary", "Pinned"] and "Ball 1" in names, names
print("Timeline summary merge: ok")
