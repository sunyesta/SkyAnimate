"""The timeline's key header follows the path being edited: with the Motion Path tool and nothing picked on the path,
it edits the stretch under the playhead (teal), its end key selected only while the header acts, never into an undo
step; picked dots or selected keys make it red; without the tool it is as before.

Run with:  OpenAnimate --background --python tests/test_key_header_path.py
"""

import oanim

wm = oanim.wm
ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers

doc.auto_key = True
ops.layer.new()
ball = layers.active
ball.add_curve([(0.0, 0.0), (0.5, 0.0)])
for frame, x in ((10, 1.0), (20, 2.0), (30, 3.0)):
    doc.frame_current = frame
    ball.location_x = x
doc.frame_current = 30
assert ops.anim.keyframe_insert() == {'FINISHED'}, "an undo step holding the keys"
doc.frame_current = 15
ops.anim.motion_path_select(none="1")


def keys(channel="position"):
    return {k["frame"]: k for k in ball.keyframes(channel)}


def selected():
    return [f for f, k in keys().items() if k["selected"]]


# --- Without the Motion Path tool: as before -------------------------------------------------------------------------
assert ops.wm.tool_select() == {'FINISHED'}
doc.motion_paths = True
h = wm.timeline_key_header()
assert h["mode"] == "NEW" and not h["editing"], h

# --- The tool, nothing picked: the stretch under the playhead, teal --------------------------------------------------
assert ops.wm.tool_motion_path() == {'FINISHED'}
h = wm.timeline_key_header()
assert h["mode"] == "PATH" and h["editing"], h
assert tuple(h["span"]) == (10, 20), h
assert "10-20" in h["label"], h
r, g, b = h["color"]
assert g > r and b > r, ("teal", h["color"])
doc.frame_current = 20
assert tuple(wm.timeline_key_header()["span"]) == (10, 20), "on a key: the stretch it ends"
doc.frame_current = 21
assert tuple(wm.timeline_key_header()["span"]) == (20, 30), "the playhead moved on: so does the header"
doc.frame_current = 35
assert wm.timeline_key_header()["mode"] == "NEW", "past the last key: no stretch"
doc.frame_current = 15

# --- A change goes to that stretch's key alone, and leaves nothing selected ------------------------------------------
before = {f: k["interp"] for f, k in keys().items()}
assert wm.timeline_key_header_call("anim.keys_interpolation_set", interp="LINEAR")
after = {f: k["interp"] for f, k in keys().items()}
assert after[20] == "LINEAR", after
assert all(after[f] == before[f] for f in after if f != 20), ("only the stretch's key", before, after)
assert selected() == [], ("the key isn't left selected", selected())
ops.ed.undo()
assert keys()[20]["interp"] == before[20], "one undo step"
assert selected() == [], "and the undone state holds no selection either"
ops.ed.redo()
assert keys()[20]["interp"] == "LINEAR"
assert selected() == [], "nor the redone one"

# --- Something picked on the path: red, on that --------------------------------------------------------------------
assert ops.anim.motion_path_select(frame="25", layer=str(ball.id)) == {'FINISHED'}
h = wm.timeline_key_header()
assert h["mode"] == "SELECTED", h
assert wm.timeline_key_header_call("anim.keys_interpolation_set", interp="STEP")
assert keys()[30]["interp"] == "STEP", "a picked dot: its stretch's key"
assert keys()[20]["interp"] == "LINEAR", "not the playhead's"
ops.anim.motion_path_select(none="1")
assert ops.anim.motion_path_select(frame="10", layer=str(ball.id)) == {'FINISHED'}
assert wm.timeline_key_header()["mode"] == "SELECTED", "a picked key"
ops.anim.motion_path_select(none="1")
assert wm.timeline_key_header()["mode"] == "PATH", "nothing picked again: teal"

print("Key header path tests passed")
