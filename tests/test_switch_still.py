"""Playing past drawings a switch hides: one with no keys, no action data and nothing action bone actions added comes
out the same on every frame, so while no edit could have reached it (the main loop's stamp of edits holds) it is
neither recorded nor evaluated; one with keys always is; and a new stamp (an edit the app saw coming) looks at
everything again.

Run with:  OpenAnimate --background --python tests/test_switch_still.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0
doc.auto_key = True
base = layers.active


def drawing(name, sw):
    layers.active = sw
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(-0.2, -0.1), (0.2, -0.1), (0.2, 0.1), (-0.2, 0.1)], closed=True, fill=True)
    assert oanim.ops.layer.move_to_group(group=str(sw.id)) == {'FINISHED'}
    return l


layers.active = base
oanim.ops.layer.new_switch()
sw = layers.active
sw.name = "Views"
front, side, waving = drawing("Front", sw), drawing("Side", sw), drawing("Waving", sw)
assert oanim.ops.layer.switch_set(layer=str(sw.id), child="Front") == {'FINISHED'}
# Waving has keys of its own: hidden or not, it plays.
doc.frame_current = 10
waving.location_x = 1.0
doc.frame_current = 0
assert [k["frame"] for k in waving.keyframes("position")] == [0, 10]
assert sw.switch_active == "Front"

# --- Playing: the first frame change looks at everything and finds Side still -------------------------
oanim.wm.frame_sync_playing(1, 100)
oanim.wm.frame_sync_playing(2, 100)

# An edit nothing said happened (a real one comes with a new stamp: a Python run, an operator, the UI): Side is
# left as it is while the stamp holds. That it isn't recorded shows it was passed over.
side.location_x = 0.5
oanim.wm.frame_sync_playing(3, 100)
assert side.keyframes("position") == [], "hidden and still: not recorded"
assert abs(side.location_x - 0.5) < 1e-6, "nor evaluated"

# The hidden drawing with keys still plays on every frame.
oanim.wm.frame_sync_playing(5, 100)
assert abs(waving.location_x - 0.5) < 1e-3, ("hidden with keys: evaluated", waving.location_x)

# --- A new stamp: everything is looked at again, and the edit is recorded ------------------------------
oanim.wm.frame_sync_playing(6, 101)
assert [k["frame"] for k in side.keyframes("position")] == [0, 5], side.keyframes("position")

# Side has keys now: never passed over again.
oanim.wm.frame_sync_playing(7, 101)
side.location_x = 0.75
oanim.wm.frame_sync_playing(8, 101)
assert 7 in [k["frame"] for k in side.keyframes("position")], "animated: recorded under the same stamp"

# --- A still drawing shown on another frame shows as it is ----------------------------------------------
oanim.wm.frame_sync_playing(9, 102)
oanim.wm.frame_sync_playing(10, 102)
assert oanim.ops.layer.switch_set(layer=str(sw.id), child="Waving") == {'FINISHED'}
oanim.wm.frame_sync_playing(11, 103)
assert sw.switch_active == "Waving"
assert abs(front.location_x) < 1e-6 and front.keyframes("position") == []

# --- Frame changes made from Python (or operators, undo) never pass anything over ------------------------
front.location_x = 0.25
doc.frame_current = 12
assert [k["frame"] for k in front.keyframes("position")] == [0, 11], front.keyframes("position")

print("switch still drawings OK")
