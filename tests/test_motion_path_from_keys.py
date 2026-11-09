"""Own Motion without working the document out: a layer's or a point's own values on each frame come from its keys
where nothing else moves them (no action bone actions in them, no driver on its layer, not a reference, no dynamics),
and they are placed on copies of the document as shown kept between redraws, put back as they were after each. The
paths, the key handles and the control points come out as working every frame out on copies of the document makes
them; with a driver on the layer they are worked out.

Run with:  OpenAnimate --background --python tests/test_motion_path_from_keys.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def grab(kept):
    """Every path's dots, its keys' handles and its control points, as numbers."""
    out = []
    for p in wm.motion_paths(kept=kept):
        assert p["own_motion"], p["label"]
        out.append((p["label"], [tuple(q) for q in p["points"]],
                    [(h["frame"], h["in"], h["out"]) for h in p["handles"]]))
    out.append([(c["layer"], c["end"], c["index"], c["at"], c["curve"], c["in_tip"], c["out_tip"])
                for c in wm.motion_path_controls(kept=kept)])
    return out


def close(a, b, eps=1e-5):
    if a is None or b is None:
        return a is b
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    if isinstance(a, str):
        return a == b
    return abs(a - b) < eps


def same_ways(what):
    """From the keys (kept, and not) as worked out on copies of the document."""
    wm.motion_path_own_from_keys(True)
    kept, fresh = grab(True), grab(False)
    wm.motion_path_own_from_keys(False)
    worked = grab(False)
    wm.motion_path_own_from_keys(True)
    assert close(kept, worked), (what, "kept", kept, worked)
    assert close(fresh, worked), (what, "fresh", fresh, worked)
    return worked


def frames_worked(kept=False):
    """How many frames of the paths were worked out on copies of the document."""
    wm.motion_path_places_clear()
    wm.motion_paths(kept=kept)
    return wm.motion_path_places()


ops.wm.read_homefile()
doc.auto_key = False
doc.motion_paths = True
doc.path_all = True
doc.frame_end = 40
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert doc.path_own_motion

# A ball keyed 10-30 with control points between, in a group that slides and turns 0-40.
ops.layer.new()
ball = layers.active
ball.add_curve([(0.0, 0.0), (0.5, 0.0), (0.5, 0.5)])
ball.key_whole_drawing = False
for other in layers.all():
    other.selected = other.id == ball.id
assert ops.layer.group() == {'FINISHED'}
group = layers.active
doc.auto_key = True
doc.frame_current = 40
group.location_x = 8.0
group.rotation = 20.0
doc.frame_current = 10
ball.location_x, ball.location_y = 0.0, 0.0
doc.frame_current = 30
ball.location_x, ball.location_y = 4.0, 1.0
ball.rotation = 15.0
doc.frame_current = 20
ball.set_point(1, (0.5, 1.0))
doc.frame_current = 0
doc.auto_key = False
args = dict(layer=str(ball.id))
for f in (14, 22):
    assert ops.anim.motion_path_control_add(frame=str(f), **args) == {'FINISHED'}

# --- Two layers' paths, the ball's inside the group's: each placed with the other as shown --------------------------
for other in layers.all():
    other.selected = other.id in (ball.id, group.id)
layers.active = ball
assert ops.anim.motion_path_select(frame="30", **args) == {'FINISHED'}
labels = [p["label"] for p in wm.motion_paths()]
assert sorted(labels) == sorted([ball.name, group.name]), labels
for f in (0, 5, 12, 20, 33, 12):
    doc.frame_current = f
    same_ways("layers, frame %d" % f)
assert any(c for c in grab(False)[-1]), "control points to place"
doc.frame_current = 12
assert frames_worked()["frames"] > 0, "counted as worked out all the same"

# --- An edit, then the frame changed: placed again on the kept copies -------------------------------------------
at = next(p for p in wm.motion_paths() if p["label"] == ball.name)
dot = at["points"][30 - at["first"]]
assert ops.anim.motion_path_move(frame="30", x=str(dot[0] + 0.5), y=str(dot[1]), **args) == {'FINISHED'}
same_ways("an edit")
assert ops.screen.frame_offset_forward() == {'FINISHED'}
same_ways("the frame after an edit")

# --- A point's path: its own keys, its layer held as shown -------------------------------------------------------
for other in layers.all():
    other.selected = other.id == ball.id
layers.active = ball
ball.select_points([1])
assert [p["label"] for p in wm.motion_paths()] == [ball.name + " point 1"], [p["label"] for p in wm.motion_paths()]
for f in (0, 15, 20, 26, 15):
    doc.frame_current = f
    same_ways("a point, frame %d" % f)

# --- A driver on the layer: worked out, not taken from the keys --------------------------------------------------
ball.select_points([1], select=False)
for other in layers.all():
    other.selected = other.id == ball.id
assert [p["label"] for p in wm.motion_paths()] == [ball.name], [p["label"] for p in wm.motion_paths()]
doc.frame_current = 5
undriven = grab(False)
assert ops.anim.driver_add(layer=str(ball.id), path="position", component="1", expression="frame / 10") == {'FINISHED'}
for f in (5, 25, 5):
    doc.frame_current = f
    driven = same_ways("driven, frame %d" % f)
assert not close(driven, undriven), "the driver moves its path"

print("Motion path from keys tests passed")
