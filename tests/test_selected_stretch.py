"""The selected stretch, one for the key header, the timeline's rows, the timing chart and the Path Timing panel: with the
Motion Path tool, the stretch the picks are in (a dot's; a key's on the side of the playhead, arriving with the playhead
on it), teal on the row of the channel moving the picked dot (the row it is folded into when that is folded away);
picks in different stretches make no teal (the header grey, "N stretches", editing them all; the panel shows one of
them, its arrows stepping through them); with one stretch picked the panel's arrows pick the stretch beside. Alt click
between keys in a timeline row picks that row's dots on the frame (on a key, the key) and goes there; without the tool
it selects the stretch's end key.

Run with:  OpenAnimate --background --python tests/test_selected_stretch.py
"""

import oanim

ops, doc, layers, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm

ops.wm.read_homefile()
doc.auto_key = True
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
for f, x, a in ((10, 1.0, 0.3), (20, 2.0, 0.6), (30, 3.0, 0.2)):
    doc.frame_current = f
    rig.bone_set("arm", position=(x, 0.5 * x), angle=a)
assert ops.anim.keyframe_insert() == {'FINISHED'}, "an undo step holding the keys"
doc.frame_current = 15
doc.path_all = True
assert ops.wm.tool_motion_path() == {'FINISHED'}
doc.motion_paths = False  # the tool shows the paths with the overlay off too: the stretch is there all the same
rig.select_bone("arm")
HEAD = dict(layer=str(rig.id), bone="arm", head="1")
TIP = dict(layer=str(rig.id), bone="arm")


def rows():
    return wm.timeline_row_details(True)


def row(label):
    return next(i for i, r in enumerate(rows()) if r["label"] == label)


def teal():
    return {r["label"]: r["teal"] for r in rows() if r["teal"]}


def dots():
    return sorted((d["head"], d["frame"]) for d in wm.motion_path_dots())


def pos_keys():
    return {k["frame"]: k for k in rig.keyframes('bones["arm"].position')}


def selected():
    return [f for f, k in pos_keys().items() if k["selected"]]


def none():
    ops.anim.motion_path_select(none="1")


none()

# --- Nothing picked: the stretch under the playhead, teal on the moving channel's row alone ---------------------------
h = wm.timeline_key_header()
assert h["mode"] == "PATH" and not h["picked"] and tuple(h["span"]) == (10, 20), h
ARM = h["targets"][0][1]
assert h["targets"] == [(rig.id, ARM, False, 0)], ("the arm's tip", h)
assert teal() == {"Angle": (10, 20)}, ("the tip turns: its Angle row, not Summary, Bones or the bone's", teal())

# The overlay on: the same.
doc.motion_paths = True
assert teal() == {"Angle": (10, 20)}, teal()
doc.motion_paths = False

# --- A picked dot: its stretch, on its channel's row; the panel shows it ---------------------------------------------
assert ops.anim.motion_path_select(frame="25", **HEAD) == {'FINISHED'}
h = wm.timeline_key_header()
assert h["mode"] == "PATH" and h["picked"] and tuple(h["span"]) == (20, 30), ("the dot's stretch, teal", h)
assert h["targets"] == [(rig.id, ARM, True, 0)], h
assert teal() == {"Transform": (20, 30)}, ("the head moves: its Transform row alone", teal())
p = wm.motion_path_timing_panel()
assert (p["head"], p["start"], p["end"], p["of"]) == (True, 20, 30, None), ("the panel shows it too", p)
# Folded away: the row it is folded into.
assert wm.timeline_row_fold(row("arm"), False)
assert teal() == {"arm": (20, 30)}, ("the bone's row, as its channels are folded into it", teal())
assert wm.timeline_row_fold(row("arm"), True)
assert teal() == {"Transform": (20, 30)}

# --- A picked key: the stretch on the playhead's side -----------------------------------------------------------------
none()
assert ops.anim.motion_path_select(frame="20", **HEAD) == {'FINISHED'}
for frame, span in ((15, (10, 20)), (20, (10, 20)), (25, (20, 30))):
    doc.frame_current = frame
    h = wm.timeline_key_header()
    assert h["mode"] == "PATH" and tuple(h["span"]) == span, ("playhead %d" % frame, h)
    # A bone's key is on all its channels: both its paths' rows.
    assert teal() == {"Transform": span, "Angle": span}, (frame, teal())
# The playhead after it: the header edits the stretch leaving it (the next key), the key itself kept as picked.
before = {f: k["interp"] for f, k in pos_keys().items()}
assert wm.timeline_key_header_call("anim.keys_interpolation_set", interp="LINEAR")
after = {f: k["interp"] for f, k in pos_keys().items()}
assert after[30] == "LINEAR" and after[20] == before[20], ("the stretch leaving the key", before, after)
assert selected() == [20], ("the picked key stays picked, the next isn't left selected", selected())
ops.ed.undo()
ops.ed.redo()
assert selected() == [20], ("the undo step holds the user's pick alone", selected())
doc.frame_current = 15

# --- Picks in different stretches: no teal; grey, "2 stretches", editing both; the panel steps through them -----------
none()
assert ops.anim.motion_path_select(frame="15", **HEAD) == {'FINISHED'}
assert ops.anim.motion_path_select(frame="25", extend="1", **HEAD) == {'FINISHED'}
h = wm.timeline_key_header()
assert h["mode"] == "STRETCHES" and h["editing"] and h["label"] == "2 stretches", h
r, g, b = h["color"]
assert abs(r - g) < 8 and abs(g - b) < 8, ("grey", h["color"])
assert teal() == {}, ("no teal anywhere", teal())
p = wm.motion_path_timing_panel()
assert (p["start"], p["of"]) == (20, (2, 2)), ("the last picked, 2 of 2", p)
assert ops.anim.motion_path_timing_step(step="1") == {'FINISHED'}
p = wm.motion_path_timing_panel()
assert (p["start"], p["of"]) == (10, (1, 2)), ("round to the first", p)
assert dots() == [(True, 15), (True, 25)], ("stepping keeps the picks", dots())
assert ops.anim.motion_path_timing_step(step="-1") == {'FINISHED'}
assert wm.motion_path_timing_panel()["of"] == (2, 2)
assert wm.timeline_key_header_call("anim.keys_interpolation_set", interp="STEP")
after = {f: k["interp"] for f, k in pos_keys().items()}
assert after[20] == "STEP" and after[30] == "STEP" and after[10] != "STEP", ("both stretches' keys", after)
ops.ed.undo()

# --- Other keys selected as well (not the paths' own): red, as before ------------------------------------------------
none()
assert ops.anim.motion_path_select(frame="15", **HEAD) == {'FINISHED'}
assert wm.timeline_key_press(row("Position"), rows()[row("Position")]["frames"][-1], shift=True)
h = wm.timeline_key_header()
assert h["mode"] == "SELECTED", ("the layer's own key too: red", h)
assert teal() == {}, teal()

# --- One stretch on two paths: teal on both rows ----------------------------------------------------------------------
none()
assert ops.anim.motion_path_select(frame="15", **HEAD) == {'FINISHED'}
assert ops.anim.motion_path_select(frame="12", extend="1", **TIP) == {'FINISHED'}
h = wm.timeline_key_header()
assert h["mode"] == "PATH" and h["label"] == "2 paths  10-20" and len(h["targets"]) == 2, h
assert teal() == {"Transform": (10, 20), "Angle": (10, 20)}, teal()

# --- One stretch picked: the panel's arrows pick the one beside, the frame going there ---------------------------------
none()
assert ops.anim.motion_path_select(frame="15", **HEAD) == {'FINISHED'}
assert ops.anim.motion_path_timing_step(step="1") == {'FINISHED'}
assert dots() == [(True, 25)] and doc.frame_current == 25, ("the next stretch picked", dots(), doc.frame_current)
assert tuple(wm.timeline_key_header()["span"]) == (20, 30)
p = wm.motion_path_timing_panel()
assert (p["head"], p["start"]) == (True, 20), p
assert ops.anim.motion_path_timing_step(step="-1") == {'FINISHED'}
assert dots() == [(True, 15)] and doc.frame_current == 15, (dots(), doc.frame_current)

# --- Alt click in a row: its dots on that frame, the frame going there; not an undo step -------------------------------
none()
steps = len(wm.undo_history()[0])
assert wm.timeline_row_alt_click(row("Transform"), 17)
assert dots() == [(True, 17)] and doc.frame_current == 17, ("the head's dot", dots(), doc.frame_current)
assert teal() == {"Transform": (10, 20)}
assert wm.timeline_row_alt_click(row("Angle"), 14, shift=True)
assert dots() == [(False, 14), (True, 17)] and doc.frame_current == 14, ("Shift adds the tip's", dots())
assert wm.timeline_row_alt_click(row("arm"), 22)
assert dots() == [(False, 22), (True, 22)], ("a bone's row: its tip and head", dots())
summary = next(i for i, r in enumerate(rows()) if r["summary"])
assert wm.timeline_row_alt_click(summary, 12)
assert dots() == [(False, 12), (True, 12)], ("the Summary: every path shown", dots())
assert wm.timeline_row_alt_click(row("Transform"), 20)
assert dots() == [] and selected() == [20] and doc.frame_current == 20, ("on a key: the key", dots(), selected())
assert len(wm.undo_history()[0]) == steps, "no undo step"

# A row with no path traced does nothing: the bone layer's own position (Edit Layer off).
none()
assert not wm.timeline_row_alt_click(row("Position"), 15), "no path in that row"
assert dots() == [] and selected() == []

# --- Without the Motion Path tool: the stretch's end key, the frame going there ----------------------------------------
assert ops.wm.tool_select() == {'FINISHED'}
assert wm.timeline_row_alt_click(row("Transform"), 23)
assert selected() == [30] and doc.frame_current == 23, (selected(), doc.frame_current)

print("Selected stretch tests passed")
