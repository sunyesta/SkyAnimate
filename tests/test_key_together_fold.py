"""A group, bone layer or vector layer keying together (Key Together) is a chained timeline row: it never opens,
showing every key inside it, and clicking its chain turns Key Together off (an undo step). Folding any other row is a
view and keys nothing. Open, a layer's row is a heading with no keys of its own. A vector layer alone keys together
too: its move, turn, size, opacity and drawing as one movement, filler keys holding the rest, let go again when the
chain is clicked, the timing chart having one "<layer> together" row.

Run with:  OpenAnimate --background --python tests/test_key_together_fold.py
"""

import oanim

ops = oanim.ops
layers = oanim.data.layers
doc = oanim.data.document
wm = oanim.wm
doc.auto_key = True
doc.auto_freeze_keys = False


def frames(layer, path):
    try:
        return [k["frame"] for k in layer.keyframes(path)]
    except KeyError:
        return []


def fillers(layer, path):
    try:
        return [k["frame"] for k in layer.keyframes(path) if k.get("filler")]
    except KeyError:
        return []


def settle():
    doc.frame_current = doc.frame_current + 1
    doc.frame_current = doc.frame_current - 1


def open_layer_rows():
    """Opens every folded Layer row (a layer's own properties), so its property rows show."""
    while True:
        rs = wm.timeline_row_details(True)
        i = next((i for i, r in enumerate(rs) if r["name"] == "Layer" and not r["open"]), None)
        if i is None:
            return
        assert wm.timeline_row_fold(i, True)


def rows(as_shown=True):
    if as_shown:
        open_layer_rows()
    return wm.timeline_row_details(as_shown)


def row(name):
    return next((r for r in rows() if r["name"] == name), None)


def point_frames(layer):
    return sorted({k["frame"] for k in layer.keyframes("points[0].pos")})


# A drawing moved on 10 and reshaped on 20 (keyed as a whole drawing).
ops.layer.new()
ball = layers.active
ball.key_together = False  # open: its own rows show
ball.name = "Ball"
ball.add_curve([(0.0, 0.0), (1.0, 0.0)])
assert ball.key_whole_drawing and not ball.key_together
doc.frame_current = 10
ball.location_x = 1.0
doc.frame_current = 20
ball.set_point(1, (1.0, 1.0))
doc.frame_current = 0
settle()
assert frames(ball, "position") == [0, 10] and point_frames(ball) == [0, 20], (frames(ball, "position"),
                                                                               point_frames(ball))

# Open by default: a heading without keys, under the Summary (its own row, every key).
assert rows()[0]["summary"] and rows()[0]["frames"] == [0, 10, 20], rows()[0]
top = rows()[1]
assert (top["name"], top["label"], top["open"], top["frames"]) == ("Ball", "Ball", True, []), top
assert row("Position") and row("Drawing"), rows()

# Folded: a view, keying nothing; the row shows every key, its rows hidden.
assert wm.timeline_row_fold(1, False) and not ball.key_together
top = rows()[1]
assert not top["open"] and top["frames"] == [0, 10, 20] and top["chain"] == "", top
assert frames(ball, "position") == [0, 10] and point_frames(ball) == [0, 20], "nothing keyed"
assert wm.timeline_row_fold(1, True) and rows()[1]["open"]

# Key Together on (an undo step): chained, never open; the row shows every key; its rows hidden.
assert ops.anim.key_together_set(layer=str(ball.id), on="1") == {'FINISHED'} and ball.key_together
settle()
top = rows()[1]
assert top["label"] == "Ball" and not top["open"] and top["frames"] == [0, 10, 20], top
assert top["chain"] == "TOGETHER" and top["stretch_pickable"], top
assert not wm.timeline_row_fold(1, True) and not rows()[1]["open"], "a chained row never opens"
assert row("Position") is None and row("Drawing") is None, "chained: its rows hidden"
# Keyed as one: the move held on 20, the drawing held on 10, by filler keys.
assert frames(ball, "position") == [0, 10, 20] and 20 in fillers(ball, "position"), ball.keyframes("position")
assert point_frames(ball) == [0, 10, 20], point_frames(ball)
# An edit on a frame keys all of it there.
doc.frame_current = 25
ball.opacity = 0.5
doc.frame_current = 0
settle()
assert 25 in frames(ball, "position") and 25 in point_frames(ball), (frames(ball, "position"), point_frames(ball))
# The timing chart's one row for it all.
labels = [t["label"] for t in wm.motion_path_timing()]
assert labels == ["Ball together"], labels

# The chain clicked: Key Together off, an undo step; the filler keys never edited go; its own keys stay.
assert wm.timeline_row_chain(1)
assert not ball.key_together
assert wm.undo_history()[0][-1] == "Key Together", wm.undo_history()[0][-1]
settle()
assert frames(ball, "position") == [0, 10], frames(ball, "position")
assert point_frames(ball) == [0, 20], point_frames(ball)
assert frames(ball, "opacity") == [0, 20, 25], ("the edit made while folded stays, with the pose before it held",
                                                frames(ball, "opacity"))
assert rows()[1]["frames"] == [] and rows()[1]["open"]

# Undo chains it again, its fillers back.
assert ops.ed.undo() == {'FINISHED'}
assert ball.key_together and not rows()[1]["open"]
assert 20 in frames(ball, "position"), frames(ball, "position")
assert ops.ed.redo() == {'FINISHED'}
assert not ball.key_together and rows()[1]["open"]

# Key Together set in the Layer tab chains the row; cleared, it opens.
ball.key_together = True
assert not rows()[1]["open"] and rows()[1]["chain"] == "TOGETHER"
ball.key_together = False
assert rows()[1]["open"] and rows()[1]["chain"] == ""
assert not wm.timeline_row_chain(1), "no chain to click"

# A group: chained with Key Together, folding freely without; a layer inside chained too while the group is open.
ops.layer.new()
other = layers.active
other.key_together = False  # open: its own rows show
other.name = "Other"
other.add_curve([(0.0, 2.0), (1.0, 2.0)])
doc.frame_current = 5
other.location_y = 1.0
doc.frame_current = 0
for l in layers:
    l.selected = l.id in (ball.id, other.id)
assert ops.layer.group() == {'FINISHED'}
group = layers.active
group.name = "Group"
settle()
assert row("Group")["open"] and row("Group")["frames"] == [] and row("Ball")["open"], rows()
at = next(i for i, r in enumerate(rows()) if r["name"] == "Group")
assert wm.timeline_row_fold(at, False) and not group.key_together
assert not row("Group")["open"] and row("Ball") is None and row("Group")["frames"] == [0, 5, 10, 20, 25], rows()
assert wm.timeline_row_fold(at, True) and row("Group")["open"]
assert ops.anim.key_together_set(layer=str(group.id), on="1") == {'FINISHED'} and group.key_together
assert not row("Group")["open"] and row("Ball") is None and row("Group")["chain"] == "TOGETHER", rows()
assert wm.timeline_row_chain(at) and not group.key_together and row("Group")["open"]
other.key_together = True
assert not row("Other")["open"] and row("Other")["chain"] == "TOGETHER" and row("Group")["open"]
other.key_together = False

# A pick inside a chained row never opens it: red instead.
group.key_together = True
layers.active = ball
ops.mesh.select_all()
assert not row("Group")["open"] and row("Group")["red"] and group.key_together, row("Group")
ops.mesh.select_none()
assert not row("Group")["red"]

# Keyed between poses while keying together: the poses either side hold what they showed (their fillers become its own keys),
# so the edit changes it only between them; kept when the row is opened.
group.key_together = False
settle()
ops.layer.new()
hop = layers.active
hop.key_together = False  # open: its own rows show
hop.name = "Hop"
hop.add_curve([(0.0, -2.0), (1.0, -2.0)])
doc.frame_current = 30
hop.location_x = 3.0  # moved on 0 and 30 only
doc.frame_current = 10
hop.rotation = 0.5  # turned on 10 and 20: the other poses
doc.frame_current = 20
hop.rotation = 1.0
doc.frame_current = 0
hop.key_together = True
settle()
assert fillers(hop, "position") == [10, 20], hop.keyframes("position")


def hop_y():
    out = []
    for f in (0, 10, 15, 20, 30):
        doc.frame_current = f
        out.append(round(hop.location_y, 3))
    doc.frame_current = 0
    return out


doc.frame_current = 15
hop.location_y = 2.0
doc.frame_current = 0
settle()
assert hop_y() == [0.0, 0.0, 2.0, 0.0, 0.0], ("the poses on 10 and 20 hold", hop_y())
assert fillers(hop, "position") == [], ("the poses either side are its own now", hop.keyframes("position"))
assert 15 in fillers(hop, "rotation") and 30 in fillers(hop, "rotation"), ("only the channel edited",
                                                                         hop.keyframes("rotation"))
hop.key_together = False
settle()
assert frames(hop, "position") == [0, 10, 15, 20, 30], frames(hop, "position")
assert hop_y() == [0.0, 0.0, 2.0, 0.0, 0.0], ("opened, they stay", hop_y())

# Only a layer that can key together.
assert not ops.anim.key_together_set(layer="999999") == {'FINISHED'}

print("key together fold: ok")
