"""The timeline's chained rows and folded diamonds. A layer keying together, or a Drawing row keying the whole drawing,
is chained: it never opens, its chain turns the setting off (an undo step), Alt click picks a stretch on it (as on a
single channel's row; a folder, a layer's heading or the Summary picks none), and a search finds it by a layer or bone
inside it. On a chained or folded row and the Summary, with bones or points picked, a frame where only other channels
have keys (fillers aside) is hollow. A drawing keyed whole shows its picked points' paths, its timing the drawing's.

Run with:  OpenAnimate --background --python tests/test_timeline_chain.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm
doc.frame_current = 0
doc.auto_key = True
doc.auto_freeze_keys = False
doc.timeline_only_selected = False


def settle():
    doc.frame_current = doc.frame_current + 1
    doc.frame_current = doc.frame_current - 1


def rows(as_shown=True):
    return wm.timeline_row_details(as_shown)


def index(name):
    return next(i for i, r in enumerate(rows()) if r["name"] == name)


def row(name):
    return rows()[index(name)]


def frames(layer, path):
    return [k["frame"] for k in layer.keyframes(path)]


# --- A bone layer: the arm turned on 10, the leg on 20 ---------------------------------------------------------------
ops.layer.new_bone()
rig = layers.active
rig.name = "rig"
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.select_none()
ops.armature.bone_add(name="leg", head_x="0", head_y="0", tail_x="0", tail_y="-1", parent="")
doc.frame_current = 10
rig.bone_set("arm", angle=0.5)
doc.frame_current = 20
rig.bone_set("leg", angle=0.5)
doc.frame_current = 0
settle()
assert not rig.key_together and row("rig")["open"] and row("rig")["chain"] == ""
ops.armature.select_none()
rig.select_bone("arm")

# Folded (a view): every key, the leg's frame hollow for the picked arm; nothing keyed.
arm_frames = frames(rig, 'bones["arm"].angle')
assert 20 not in arm_frames and 10 in arm_frames, arm_frames
assert wm.timeline_row_fold(index("rig"), False) and not rig.key_together
assert not row("rig")["open"] and 20 in row("rig")["frames"], row("rig")
assert row("rig")["hollow_frames"] == [20], ("only the leg is keyed on 20", row("rig"))
assert row("Summary")["hollow_frames"] == [20], row("Summary")
assert frames(rig, 'bones["arm"].angle') == arm_frames, "folding keys nothing"
assert not row("rig")["stretch_pickable"] and not wm.timeline_row_alt_click(index("rig"), 15), \
    "a folded layer's row picks no stretch"
assert not row("Summary")["stretch_pickable"]
# Nothing picked: all full.
rig.select_bone("")
assert row("rig")["hollow_frames"] == [] and row("Summary")["hollow_frames"] == [], row("rig")
rig.select_bone("arm")
assert wm.timeline_row_fold(index("rig"), True) and row("rig")["open"]

# --- Keying together: chained shut; fillers hold the arm on 20, still hollow there (a filler isn't its own key) --------
assert ops.anim.key_together_set(layer=str(rig.id), on="1") == {'FINISHED'} and rig.key_together
settle()
assert any(k.get("filler") and k["frame"] == 20 for k in rig.keyframes('bones["arm"].angle')), \
    rig.keyframes('bones["arm"].angle')
r = row("rig")
assert r["chain"] == "TOGETHER" and not r["open"] and r["red"] and r["stretch_pickable"], r
assert r["hollow_frames"] == [20], ("the arm's key on 20 is a filler", r)
assert not wm.timeline_row_fold(index("rig"), True) and not row("rig")["open"], "never opens"
assert "arm" not in [x["name"] for x in rows()], "its rows hidden"

# Alt click on it picks the stretch there: its end keys, for everything it holds.
assert wm.timeline_row_alt_click(index("rig"), 15)
picked = [k["frame"] for k in rig.keyframes('bones["leg"].angle') if k["selected"]]
assert picked == [20], ("the stretch's end key", rig.keyframes('bones["leg"].angle'))

# A search finds it by a bone inside it; not by letters nothing in it has.
assert ops.anim.timeline_filter(search="leg") == {'FINISHED'}
assert "rig" in [x["name"] for x in rows()], rows()
assert ops.anim.timeline_filter(search="qqq") == {'FINISHED'}
assert "rig" not in [x["name"] for x in rows()], rows()
assert ops.anim.timeline_filter(search="") == {'FINISHED'}

# The chain clicked: Key Together off, an undo step; the row opens again.
assert wm.timeline_row_chain(index("rig")) and not rig.key_together
assert wm.undo_history()[0][-1] == "Key Together", wm.undo_history()[0][-1]
assert row("rig")["open"] and row("rig")["chain"] == ""
assert ops.ed.undo() == {'FINISHED'} and rig.key_together
assert ops.ed.redo() == {'FINISHED'} and not rig.key_together

# --- A drawing keyed whole: its Drawing row chained; its picked points' paths shown, their timing the drawing's --------
ops.layer.new()
art = layers.active
art.name = "art"
art.key_together = False  # open: its Drawing row shows
assert art.key_whole_drawing
art.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
doc.frame_current = 10
art.set_point(1, (1.0, 1.0))
doc.frame_current = 0
settle()
for other in layers:
    other.selected = other.id == art.id
layers.active = art
assert frames(art, "points[0].pos") == [0, 10], "keyed whole"
d = row("Drawing")
assert d["chain"] == "DRAWING" and not d["open"] and d["stretch_pickable"], d
assert "Points" not in [x["name"] for x in rows()]
assert not wm.timeline_row_alt_click(index("art"), 5), "the layer's heading picks none"

ops.mesh.select_none()
art.select_points([1])
doc.motion_paths = True
assert ops.wm.tool_motion_path() == {'FINISHED'}
labels = [p["label"] for p in wm.motion_paths()]
assert "art point 1" in labels and "art point 0" not in labels, ("the picked point's path", labels)
timing = [t["label"] for t in wm.motion_path_timing()]
assert "art drawing" in timing and not any("point" in t for t in timing), ("the drawing's timing", timing)
ids = art.point_ids
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", frame="5", layer=str(art.id), point=str(ids[1])) == {
    'FINISHED'}
for path in ("points[0].pos", "points[1].pos", "points[2].pos"):
    k = next(k for k in art.keyframes(path) if k["frame"] == 10)
    assert k["interp"] == "CUSTOM", ("timed as one drawing", path, k)

# Its chain clicked: Key Whole Drawing off, an undo step; Points again.
assert wm.timeline_row_chain(index("Drawing")) and not art.key_whole_drawing
assert ops.ed.undo() == {'FINISHED'} and art.key_whole_drawing
assert ops.ed.redo() == {'FINISHED'} and not art.key_whole_drawing
assert "Points" in [x["name"] for x in rows()], [x["name"] for x in rows()]
assert ops.anim.key_together_set(layer=str(rig.id), drawing="1") == {'CANCELLED'}, "a bone layer has no drawing"

print("timeline chain: ok")
