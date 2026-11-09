"""What the paths are worked out from is kept while the document stays the same, as the viewport keeps it
(MotionPathKept): each frame worked out on a copy, and the places the handles and control points come from. A frame
change only places them again (Own Motion holds the rest as the frame shown); picking keys on the paths changes nothing
they come from. An edit, or an undo, works them out again. Kept or not, they come out the same.

Run with:  OpenAnimate --background --python tests/test_motion_path_kept.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def close(a, b, eps=1e-4):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def paths(kept):
    return {p["label"]: p["points"] for p in wm.motion_paths(kept=kept)}


def controls(kept):
    return [(c["index"], c["at"], c["in_tip"], c["out_tip"]) for c in wm.motion_path_controls(kept=kept)
            if c["end"] == 30]


def same_as_fresh(what):
    """Kept and worked out afresh come out the same (the fresh ones counted out of what the kept ones cost)."""
    k_paths, k_controls = paths(True), controls(True)
    used = wm.motion_path_places()
    f_paths, f_controls = paths(False), controls(False)
    assert k_paths.keys() == f_paths.keys() and all(close(k_paths[n], f_paths[n]) for n in f_paths), (what, "paths")
    assert close(k_controls, f_controls), (what, k_controls, f_controls)
    return used, k_paths


ops.wm.read_homefile()
doc.auto_key = False
doc.motion_paths = True
doc.path_before, doc.path_after = 40, 40
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert doc.path_own_motion

# A ball keyed 10-30 with control points between, in a group that slides 0-40: Own Motion holds the group as the
# frame shown, so a frame change moves the paths with no change to what they are worked out from.
ops.layer.new()
ball = layers.active
ball.add_curve([(0.0, 0.0), (0.1, 0.0)])
ball.key_whole_drawing = False
for other in layers.all():
    other.selected = other.id == ball.id
assert ops.layer.group() == {'FINISHED'}
group = layers.active
doc.auto_key = True
doc.frame_current = 40
group.location_x = 8.0
doc.frame_current = 0
doc.auto_key = False
assert [k["frame"] for k in group.keyframes("position")][-1] == 40, group.keyframes("position")
layers.active = ball
for other in layers.all():
    other.selected = other.id == ball.id
ball.location_x, ball.location_y = -5.0, -5.0
args = dict(layer=str(ball.id))
doc.auto_key = True
for f, (x, y) in ((10, (0.0, 0.0)), (30, (4.0, 0.0))):
    doc.frame_current = f
    ball.location_x, ball.location_y = x, y
doc.frame_current = 0
doc.auto_key = False
for f in (10, 30):
    ball.keyframe_set("position", f, interp="LINEAR")
for f in (14, 18, 22, 26):
    assert ops.anim.motion_path_control_add(frame=str(f), **args) == {'FINISHED'}
assert ops.screen.frame_offset_forward() == {'FINISHED'}
n = len(controls(False))
assert n == 4, controls(False)

# --- The first time: every frame, every place -------------------------------------------------------------------
paths(True)
controls(True)
wm.motion_path_places_clear()
used, before = same_as_fresh("kept at once")
assert used["frames"] == 0 and used["places"] == 0, ("nothing changed: all of it kept", used)

# --- A frame change: placed again, nothing worked out ------------------------------------------------------------
for step in range(3):
    assert ops.screen.frame_offset_forward() == {'FINISHED'}
    wm.motion_path_places_clear()
    used, now = same_as_fresh("frame %d" % doc.frame_current)
    assert used["frames"] == 0 and used["places"] == 0, ("a frame change works nothing out again", used)
label = next(iter(before))
assert not close(now[label], before[label]), "the group held as the new frame: the path moved with it"

# --- Picking keys and control points: nothing they come from changes ---------------------------------------------
for pick in (lambda: ops.anim.motion_path_select(frame="10", **args),
             lambda: ops.anim.motion_path_select(frame="30", extend="1", **args),
             lambda: ops.anim.motion_path_handle_select(frame="30", control="1", **args),
             lambda: ops.anim.motion_path_deselect_all(),
             lambda: ops.anim.motion_path_select(none="1")):
    assert pick() == {'FINISHED'}
    wm.motion_path_places_clear()
    used, _ = same_as_fresh("a pick")
    assert used["frames"] == 0 and used["places"] == 0, ("picking keys keeps them", used)

# --- An edit works them out again, and an undo ---------------------------------------------------------------------
assert ops.anim.motion_path_move(frame="30", x="4.0", y="1.0", **args) == {'FINISHED'}
wm.motion_path_places_clear()
used, moved = same_as_fresh("an edit")
assert used["frames"] > 0 and used["places"] > 0, ("an edit works the paths out again", used)
assert not close(moved[label], now[label]), "the edit shows"
assert ops.ed.undo() == {'FINISHED'}
wm.motion_path_places_clear()
used, undone = same_as_fresh("an undo")
assert used["frames"] > 0, ("an undo works them out again", used)
assert close(undone[label], now[label]), "back as it was"

# --- Keys changed with no change counted: an edit not yet keyed, recorded as the frame changes ---------------------
doc.auto_key = True
edited = doc.frame_current
ball.location_y = ball.location_y + 2.0
paths(True)
controls(True)
assert ops.screen.frame_offset_forward() == {'FINISHED'}
assert any(k["frame"] == edited for k in ball.keyframes("position")), "recorded as the frame changed"
wm.motion_path_places_clear()
used, keyed = same_as_fresh("keys changed")
assert used["frames"] > 0, ("changed keys work them out again", used)
assert not close(keyed[label], now[label]), "the new key shows"
doc.auto_key = False

print("Motion path kept tests passed")
