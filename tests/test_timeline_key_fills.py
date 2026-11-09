"""Timeline key fills: a frame whose keys are all fillers (Key Together's, Key Whole Drawing's: kept to hold the
rest, not posed) is drawn as a small grey diamond, listed as a row's filler_frames; posed keys on that frame make it
a key again. A group keying together is chained shut, so what it holds is seen here through pinned channels. (Keys outside the playback range are drawn faint: drawing only, not listed.)

Run with:  OpenAnimate --background --python tests/test_timeline_key_fills.py
"""

import oanim

ops = oanim.ops
layers = oanim.data.layers
doc = oanim.data.document
wm = oanim.wm
doc.auto_key = True
doc.auto_freeze_keys = False


def settle():
    doc.frame_current = doc.frame_current + 1
    doc.frame_current = doc.frame_current - 1


def row(name):
    return next(r for r in wm.timeline_row_details() if r["name"] == name)


def pinned(name):
    """The pinned row of layer `name`'s first point."""
    return row(name + ": points[0].pos")


ops.layer.new()
a = layers.active
a.name = "a"
a.add_curve([(0.0, 0.0), (1.0, 0.0)])
ops.layer.new()
b = layers.active
b.name = "b"
b.add_curve([(0.0, 3.0), (1.0, 3.0)])
for l in layers:
    l.selected = l.id in (a.id, b.id)
assert ops.layer.group() == {'FINISHED'}
group = layers.active
group.name = "group"

# a animated on 10; Key Together holds b there with fillers.
doc.frame_current = 10
a.set_point(1, (1.0, 1.0))
doc.frame_current = 0
group.key_together = True
settle()
assert any(k.get("filler") for k in b.keyframes("position")), b.keyframes("position")

layers.active = group
assert row("group")["chain"] == "TOGETHER" and [r["name"] for r in wm.timeline_row_details()] == ["Summary", "group"]
assert a.pin_channel("points[1].pos") == {'FINISHED'} and b.pin_channel("points[0].pos") == {'FINISHED'}
b_row = pinned("b")
assert 10 in b_row["frames"] and 10 in b_row["filler_frames"], b_row
a_row = row("a: points[1].pos")
assert 10 not in a_row["filler_frames"], ("a is posed on 10", a_row)
# The group's row: a's posed keys and b's fillers on 10 make it a key, not a filler.
assert 10 not in row("group")["filler_frames"]

# Posing b on 10 makes its keys there real.
doc.frame_current = 10
b.set_point(0, (0.5, 3.0))
doc.frame_current = 0
settle()
assert 10 in pinned("b")["frames"] and 10 not in pinned("b")["filler_frames"], pinned("b")

print("timeline key fills: ok")
