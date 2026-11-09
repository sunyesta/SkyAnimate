"""Motion paths: the edge cases a bug sweep found, each kept fixed. The Path Timing a stretch shows is how it plays (a
Custom stretch whose keys moved, Back and Elastic going past their key, a Key Whole Drawing whose first point stays
still), and a dot slid to where it is shown changes nothing else; a Cycle key's loop is not offered for timing; Time
Together gives Stretch Shape and Alt S to the linked layer; nan and inf arguments never reach the keys; Python's
keyframe_set keeps a stretch's negative speeds; control points stay where they are when a key is added or deleted in
their stretch; X deletes every key of a diamond and a turn's key is clicked as a key; the setup key's handles go back
to Auto; G on control-point handles of two paths keeps each its own; shape brushes and Blend Speed at strength 0 keep
an overshoot; a drawing's path is pinned, saved and opened as the drawing's, and the shape brushes leave it alone.

Run with:  OpenAnimate --background --python tests/test_motion_path_robust.py
"""

import math
import os
import tempfile

import oanim

oanim.data.document.auto_freeze_keys = False  # keyed channel by channel here, not holding the poses either side

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def finite(v):
    """Every number in `v` (a key, a list of keys, a path...) is neither nan nor inf."""
    if isinstance(v, dict):
        return all(finite(x) for x in v.values())
    if isinstance(v, (list, tuple)):
        return all(finite(x) for x in v)
    if isinstance(v, float):
        return math.isfinite(v)
    return True


def start(timing=False):
    """A fresh document with the Motion Path tool; `timing` as the timing tests have it (the Edit brush, no falloff)."""
    assert ops.wm.read_homefile() == {'FINISHED'}
    oanim.data.document.auto_freeze_keys = False  # keyed channel by channel here, not holding the poses either side
    doc.motion_paths = True
    doc.path_before, doc.path_after = 3, 3
    doc.auto_key = False
    assert ops.wm.tool_motion_path() == {'FINISHED'}
    if timing:
        assert ops.anim.motion_path_brush_set(brush="EDIT", proportional="0") == {'FINISHED'}


def only(layer):
    for other in layers:
        other.selected = other.id == layer.id
    layers.active = layer


def key(layer, frame, channel="position"):
    return next(k for k in layer.keyframes(channel) if k["frame"] == frame)


def frames(layer, channel="position"):
    return [k["frame"] for k in layer.keyframes(channel)]


def where(layer, frame):
    doc.frame_current = frame
    return (layer.location_x, layer.location_y)


def mover(keys, interp=None):
    """A new layer, its origin moved to keys {frame: (x, y)} through the path (each key's interp set after it)."""
    ops.layer.new()
    layer = layers.active
    layer.key_together = False  # unfolded: its own paths' timing
    layer.add_curve([(0.0, 0.0), (0.1, 0.0)])
    doc.frame_current = 0
    layer.location_x, layer.location_y = -5.0, -5.0
    only(layer)
    for f, (x, y) in keys.items():
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(layer.id)) == {'FINISHED'}
        if interp:
            layer.keyframe_set("position", f, interp=interp)
    return layer


def line_layer(keys, interp="LINEAR"):
    """A new layer, its origin keyed along the x axis, {frame: x}, every key `interp`."""
    ops.layer.new()
    layer = layers.active
    layer.key_together = False  # unfolded: its own paths' timing
    layer.add_curve([(0.0, 0.0), (0.1, 0.0)])
    doc.frame_current = 0
    layer.location_x, layer.location_y = -5.0, -5.0
    only(layer)
    for f, x in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y="0", layer=str(layer.id)) == {'FINISHED'}
        if f not in frames(layer):
            doc.frame_current = f
            layer.keyframe_insert("position", f)
        layer.keyframe_set("position", f, interp=interp)
    last = max(keys)
    assert ops.anim.motion_path_move(frame=str(last), x=str(keys[last]), y="0", layer=str(layer.id)) == {'FINISHED'}
    return layer


def path_of(layer):
    return next(p for p in oanim.wm.motion_paths() if p["layer"] == layer.id)


def stretches(layer):
    row = next(t for t in oanim.wm.motion_path_timing() if t["label"] == path_of(layer)["label"])
    return row["stretches"]


def shown(layer, first):
    """The Path Timing progress of the stretch starting on `first`."""
    return next(s for s in stretches(layer) if s["start"] == first)["progress"]


def played(layer, first, last, a, b):
    """How far along x from a to b the layer is on each frame first..last, as it plays."""
    return [(where(layer, f)[0] - a) / (b - a) for f in range(first, last + 1)]


# --- A Custom stretch whose keys moved closer: the timing shown is how it plays --------------------------------------
start(timing=True)
box = line_layer({10: 0.0, 20: 1.0, 30: 2.0})
args = dict(layer=str(box.id))
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", frame="15", **args) == {'FINISHED'}
assert len(key(box, 20)["speeds"]) == 10, "painted for its 10 frames"
assert close(shown(box, 10), played(box, 10, 20, 0.0, 1.0)), "as it was painted, the timing shows how it plays"
ops.anim.keys_deselect_all()
assert ops.anim.keys_select(frame_start="20", layers=str(box.id)) == {'FINISHED'}
assert ops.anim.keys_offset(frames="-4") == {'FINISHED'}
# Its timing is a curve through the stretch's time, so it stretches with the stretch: Ease In over 6 frames.
assert key(box, 16)["interp"] == 'CUSTOM' and len(key(box, 16)["speeds"]) == 6, "its curve played over 6 frames"
plays = played(box, 10, 16, 0.0, 1.0)
assert close(plays, [(f / 6) ** 2 for f in range(7)]), ("the Ease In curve played over 6 frames", plays)
assert close(shown(box, 10), plays), ("the timing shown is how it plays now", shown(box, 10), plays)
assert ops.anim.motion_path_timing_blend(frame="13", smoothing="0", repeat="1", radius="0", **args) == {'FINISHED'}
assert close(played(box, 10, 16, 0.0, 1.0), plays), ("blending with no smoothing leaves every dot where it was",
                                                   played(box, 10, 16, 0.0, 1.0))
assert len(key(box, 16)["speeds"]) == 6, "now painted for its 6 frames"

# The same through the timeline's - : a frame taken out of the stretch under the playhead.
ball = line_layer({10: 0.0, 20: 1.0})
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", frame="15", layer=str(ball.id)) == {'FINISHED'}
doc.frame_current = 15
assert ops.anim.span_frames_add(amount="-3") == {'FINISHED'}
end = [k for k in ball.keyframes("position") if k["frame"] > 10][0]
assert end["frame"] == 17 and len(end["speeds"]) == 7, (end["frame"], len(end["speeds"]))
plays = played(ball, 10, 17, 0.0, 1.0)
assert close(shown(ball, 10), plays), ("after -, the timing shown is how it plays", shown(ball, 10), plays)


# --- A Back stretch: its timing shows it going past the key, and one dot slides alone --------------------------------
for interp in ("BACK", "ELASTIC"):
    start(timing=True)
    box = line_layer({10: 0.0, 20: 1.0}, interp)
    args = dict(layer=str(box.id))
    before = played(box, 10, 20, 0.0, 1.0)
    assert max(before) > 1.05, (interp, "goes past its key", before)
    assert close(shown(box, 10), before), (interp, "the timing shows it going past its key", shown(box, 10), before)
    over = [f for f in range(11, 20) if before[f - 10] > 1.0 + 1e-3]
    assert over and set(over) <= set(path_of(box)["overshoot"]), (interp, "its frames past the key are marked", over,
                                                                   path_of(box)["overshoot"])
    doc.frame_current = 12
    assert ops.anim.motion_path_slide(frame="15", progress=str(before[5] + 0.01), **args) == {'FINISHED'}
    after = played(box, 10, 20, 0.0, 1.0)
    assert close(after[5], before[5] + 0.01), (interp, "the slid dot moved", after[5], before[5])
    moved = [10 + i for i in range(11) if i != 5 and not close(after[i], before[i])]
    assert not moved, (interp, "sliding one dot a little moves no other", moved, before, after)


# --- Key Whole Drawing, one point keyed in place and one moving: the timing shown is the moving one's ------------------
# Smooth, timed by how it moves (an eased mode is timed by the mode alone): the still point has no motion to time.
start(timing=True)
doc.auto_key = True
ops.layer.new()
line = layers.active
line.key_together = False  # unfolded: its own paths' timing
assert line.key_whole_drawing
only(line)
doc.frame_current = 0
line.add_curve([(0.0, 0.0), (1.0, 0.0)])
doc.frame_current = 10
line.set_point(1, (1.0, 1.0))
doc.frame_current = 20
line.set_point(1, (1.0, 1.2))
for channel in ("points[0].pos", "points[1].pos"):
    line.keyframe_set(channel, 10, interp="SMOOTH")
    line.keyframe_set(channel, 20, interp="SMOOTH")
doc.frame_current = 0
assert frames(line, "points[0].pos") == [0, 10, 20] and close(key(line, 10, "points[0].pos")["value"], (0.0, 0.0)), \
    "point 0 keyed on 10 where it was"
line.select_points([0, 1])
assert ops.wm.tool_motion_path() == {'FINISHED'}


def point_1_y():
    out = []
    for f in range(0, 11):
        doc.frame_current = f
        out.append(line.points[1][1])
    return out


row = next(t for t in oanim.wm.motion_path_timing() if t["label"] == line.name + " drawing")
plays = point_1_y()
assert not close(plays, [f / 10 for f in range(11)], 0.01) and close(plays[10], 1.0), ("point 1 not even, as Smooth", plays)
assert close(row["stretches"][0]["progress"], plays), ("the timing shown is the moving point's",
                                                      row["stretches"][0]["progress"], plays)
assert ops.anim.motion_path_slide(frame="5", progress=str(row["stretches"][0]["progress"][5]), proportional="0",
                                  layer=str(line.id), drawing="1") == {'FINISHED'}
assert close(point_1_y(), plays), ("sliding a dot to where it is shown changes nothing", point_1_y())


# --- A Cycle key: the loop after it is not offered for timing -------------------------------------------------------
start(timing=True)
box = line_layer({10: 0.0, 20: 1.0, 30: 2.0})
box.keyframe_set("position", 20, interp="CYCLE")
args = dict(layer=str(box.id))
listed = [(s["start"], s["end"]) for s in stretches(box)]
assert (20, 30) not in listed, ("the loop after the Cycle key is not a stretch to time", listed)
assert (10, 20) not in listed, ("nor the stretch into it (re-timing it would lose the loop)", listed)
loop = [where(box, f) for f in range(10, 31)]
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", frame="25", **args) == {'CANCELLED'}, "the loop not shaped"
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", frame="15", **args) == {'CANCELLED'}, "nor the way in"
assert close([where(box, f) for f in range(10, 31)], loop), "the motion as it was"
assert key(box, 20)["interp"] == 'CYCLE' and key(box, 30)["interp"] == 'LINEAR', "both keys as they were"


# --- Time Together: Stretch Shape and Alt S reach the linked layer too -------------------------------------------------
for name, call in (
    ("Stretch Shape", lambda a: ops.anim.motion_path_stretch_shape(shape="EASE_IN", frame="15", **a)),
    ("Alt S", lambda a: ops.anim.motion_path_scale_spacing(frame="15", factor="0.5", proportional="0", size="3", **a)),
):
    start(timing=True)
    b = line_layer({10: 0.0, 20: 1.0}, "EASE_IN_OUT")
    a = line_layer({10: 0.0, 20: 1.0}, "EASE_IN_OUT")
    for layer in layers:
        layer.selected = layer.id in (a.id, b.id)
    layers.active = a
    ops.anim.keys_deselect_all()
    doc.time_together = True
    assert call(dict(layer=str(a.id))) == {'FINISHED'}, name
    ka, kb = key(a, 20), key(b, 20)
    assert ka["interp"] == 'CUSTOM', (name, "A re-timed", ka["interp"])
    assert kb["interp"] == 'CUSTOM' and close(kb["speeds"], ka["speeds"]), (name, "B given the same timing",
                                                                          kb["interp"], kb.get("speeds"))
    assert close(played(b, 10, 20, 0.0, 1.0), played(a, 10, 20, 0.0, 1.0)), (name, "B moves as A does")


# --- nan and inf arguments never reach the keys ----------------------------------------------------------------------
start(timing=True)
box = line_layer({10: 0.0, 20: 1.0})
args = dict(layer=str(box.id))
even = played(box, 10, 20, 0.0, 1.0)


def keys_finite(layer, label):
    for channel in ("position", "rotation"):
        assert finite(layer.keyframes(channel)), (label, "a", channel, "key went nan or inf", layer.keyframes(channel))
    assert finite([where(layer, f) for f in range(0, 31)]), (label, "the layer went nan or inf")
    assert finite(oanim.wm.motion_paths()), (label, "a path went nan or inf")


def check(label, result, unchanged=False):
    keys_finite(box, label)
    if unchanged:
        assert close(played(box, 10, 20, 0.0, 1.0), even), (label, "taken as left out: nothing changed",
                                                            played(box, 10, 20, 0.0, 1.0))
    if result == {'FINISHED'}:
        ops.ed.undo()
    assert close(played(box, 10, 20, 0.0, 1.0), even), (label, "undone")


for v in ("nan", "inf", "-inf"):
    check("dot_speed speed=" + v, ops.anim.motion_path_dot_speed(frame="15", speed=v, **args), unchanged=True)
    check("slide progress=" + v, ops.anim.motion_path_slide(frame="15", progress=v, **args), unchanged=True)
    check("scale_spacing factor=" + v, ops.anim.motion_path_scale_spacing(frame="15", factor=v, proportional="0",
                                                                          size="3", **args), unchanged=True)
    check("scale_spacing size=" + v, ops.anim.motion_path_scale_spacing(frame="15", factor="0.5", proportional="1",
                                                                        size=v, **args))
    check("timing_blend smoothing=" + v, ops.anim.motion_path_timing_blend(frame="15", smoothing=v, repeat="1",
                                                                          radius="3", **args), unchanged=True)
    check("timing_blend radius=" + v, ops.anim.motion_path_timing_blend(frame="15", smoothing="0.5", repeat="1",
                                                                       radius=v, **args))
    check("slide x=" + v, ops.anim.motion_path_slide(frame="15", x=v, y="0", **args))
    check("slide size=" + v, ops.anim.motion_path_slide(frame="15", progress="0.7", proportional="1", size=v, **args))

start()
ball = mover({10: (0.0, 0.0), 20: (2.0, 1.0), 30: (4.0, 0.0)})
args = dict(layer=str(ball.id))
for label, call in (
    ("motion_path_move x=nan", lambda: ops.anim.motion_path_move(frame="20", x="nan", y="0", **args)),
    ("motion_path_move x=inf", lambda: ops.anim.motion_path_move(frame="20", x="inf", y="0", **args)),
    ("motion_path_handle x=nan", lambda: ops.anim.motion_path_handle(frame="20", side="OUT", x="nan", y="0", **args)),
    ("motion_path_breakdown factor=nan", lambda: ops.anim.motion_path_breakdown(frame="15", factor="nan", **args)),
):
    result = call()
    keys_finite(ball, label)
    if result == {'FINISHED'}:
        ops.ed.undo()

start()
box = mover({10: (0.0, 0.0), 20: (1.0, 0.0), 30: (2.0, 0.0)})
doc.frame_current = 20
stroked = 0
for brush in ("GRAB", "SMOOTH", "BLEND_SPEED"):
    # Strokes starting on the key on 20, (1, 0), with a nan or inf point in them.
    for points in ("1,0;nan,0.2", "1,0;1,inf", "1,0;-inf,0.2;1,0.2", "nan,nan;1,0;1,0.2"):
        result = ops.anim.motion_path_stroke(brush=brush, points=points, radius="0.5")
        keys_finite(box, brush + " stroke through " + points)
        if result == {'FINISHED'}:
            stroked += 1
            ops.ed.undo()
assert stroked, "some of the strokes reached the path"


# --- Python's keyframe_set keeps a stretch's negative speeds --------------------------------------------------------
start(timing=True)
box = line_layer({10: 0.0, 20: 1.0})
assert ops.anim.motion_path_slide(frame="18", progress="1.15", layer=str(box.id)) == {'FINISHED'}
speeds = key(box, 20)["speeds"]
assert min(speeds) < 0.0, ("going back to the key after 18 is a negative speed", speeds)
x18 = where(box, 18)[0]
assert close(x18, 1.15), x18
box.keyframe_set("position", 20, speeds=speeds)
assert close(key(box, 20)["speeds"], speeds), ("written back as read", key(box, 20)["speeds"], speeds)
assert close(where(box, 18)[0], x18), ("writing back what keyframes() gave changes nothing", where(box, 18)[0])


# --- Control points stay where they are when a key is added or deleted in their stretch ------------------------------
def control_point(layer, first, last, j=0):
    """Control point j of the stretch first..last, where it is (stored so)."""
    return tuple(key(layer, last)["path_controls"][j])


start()
ball = mover({10: (0.0, 0.0), 30: (4.0, 0.0)})
args = dict(layer=str(ball.id))
assert ops.anim.motion_path_control_add(frame="20", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="30", index="0", x="2", y="2", **args) == {'FINISHED'}
assert close(control_point(ball, 10, 30), (2.0, 2.0)), control_point(ball, 10, 30)
assert ops.anim.motion_path_key_add(frame="15", type="KEYFRAME", **args) == {'FINISHED'}
assert frames(ball) == [0, 10, 15, 30], frames(ball)
assert len(key(ball, 30)["path_controls"]) == 1 and not key(ball, 15)["path_controls"], "after the new key, on 15-30"
assert close(control_point(ball, 15, 30), (2.0, 2.0)), ("a key added before it: still at (2, 2)",
                                                       control_point(ball, 15, 30))

# A key added where the stretch has all but arrived (an Ease In, slowing into its key: no curve split there): the
# control point before it goes to the stretch into the new key, still where it was.
start()
ball = mover({10: (0.0, 0.0), 30: (4.0, 0.0)})
args = dict(layer=str(ball.id))
assert ops.anim.motion_path_control_add(frame="20", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="30", index="0", x="2", y="2", **args) == {'FINISHED'}
ball.keyframe_set("position", 30, interp="EASE_IN", ease="QUINT")
assert close(where(ball, 28), (4.0, 0.0)), ("all but there by 28", where(ball, 28))
assert ops.anim.motion_path_key_add(frame="28", type="KEYFRAME", **args) == {'FINISHED'}
assert frames(ball) == [0, 10, 28, 30], frames(ball)
assert len(key(ball, 28)["path_controls"]) == 1 and not key(ball, 30)["path_controls"], (
    "before the new key, on 10-28", key(ball, 28)["path_controls"], key(ball, 30)["path_controls"])
assert close(control_point(ball, 10, 28), (2.0, 2.0)), control_point(ball, 10, 28)

start()
ball = mover({10: (0.0, 0.0), 20: (2.0, 0.0), 30: (4.0, 0.0)})
args = dict(layer=str(ball.id))
assert ops.anim.motion_path_control_add(frame="25", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="30", index="0", x="3", y="2", **args) == {'FINISHED'}
assert close(control_point(ball, 20, 30), (3.0, 2.0)), control_point(ball, 20, 30)
assert ops.anim.motion_path_key_delete(frame="20", **args) == {'FINISHED'}
assert frames(ball) == [0, 10, 30], frames(ball)
assert close(control_point(ball, 10, 30), (3.0, 2.0)), ("the key starting its stretch deleted: still at (3, 2)",
                                                       control_point(ball, 10, 30))

# The key ending a control point's stretch deleted: it goes on to the stretch that joins the keys either side.
start()
ball = mover({10: (0.0, 0.0), 20: (2.0, 0.0), 30: (4.0, 0.0)})
args = dict(layer=str(ball.id))
assert ops.anim.motion_path_control_add(frame="15", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="20", index="0", x="1", y="2", **args) == {'FINISHED'}
assert ops.anim.motion_path_key_delete(frame="20", **args) == {'FINISHED'}
assert frames(ball) == [0, 10, 30], frames(ball)
assert len(key(ball, 30)["path_controls"]) == 1, ("kept on the joined stretch", key(ball, 30)["path_controls"])
assert close(control_point(ball, 10, 30), (1.0, 2.0)), control_point(ball, 10, 30)


# --- X on a frame keyed for position and rotation: both keys go, and the diamond ------------------------------------
start()
ball = mover({10: (0.0, 0.0), 20: (2.0, 1.0), 30: (4.0, 0.0)})
args = dict(layer=str(ball.id))
doc.frame_current = 20
ball.rotation = 45.0
doc.auto_key = True
doc.frame_current = 21
doc.frame_current = 20
doc.auto_key = False
assert frames(ball) == [0, 10, 20, 30] and frames(ball, "rotation") == [0, 20], (frames(ball), frames(ball, "rotation"))
assert ops.anim.motion_path_select(frame="20", **args) == {'FINISHED'}
assert key(ball, 20)["selected"] and key(ball, 20, "rotation")["selected"], "a diamond is all its keys"
assert ops.anim.motion_path_keys_delete() == {'FINISHED'}
assert frames(ball) == [0, 10, 30], ("the position key gone", frames(ball))
assert frames(ball, "rotation") == [0], ("the rotation key gone too", frames(ball, "rotation"))
assert path_of(ball)["keys"] == [10, 30], ("no diamond left on 20", path_of(ball)["keys"])


# --- A frame keyed only by rotation: clicking it selects it as a key, not a dot ---------------------------------------
start()
ball = mover({10: (0.0, 0.0), 30: (4.0, 0.0)})
args = dict(layer=str(ball.id))
doc.auto_key = True
doc.frame_current = 20
ball.rotation = 30.0
doc.frame_current = 0
doc.auto_key = False
assert frames(ball) == [0, 10, 30] and frames(ball, "rotation") == [0, 20], (frames(ball), frames(ball, "rotation"))
doc.frame_current = 15
assert 20 in path_of(ball)["keys"], ("its diamond is drawn", path_of(ball)["keys"])
assert ops.anim.motion_path_select(frame="20", **args) == {'FINISHED'}
assert oanim.wm.motion_path_dots() == [], ("not picked as a dot", oanim.wm.motion_path_dots())
assert key(ball, 20, "rotation")["selected"], "selected as a key"


# --- The setup key's dragged handles go back to Auto ----------------------------------------------------------------
start()
ball = mover({10: (0.0, 0.0), 20: (2.0, 1.0)})
args = dict(layer=str(ball.id))
doc.frame_current = 5
assert ops.anim.motion_path_handle(frame="0", side="OUT", x="-5", y="-3", **args) == {'FINISHED'}
assert key(ball, 0)["path_handles"] == 'ALIGNED', key(ball, 0)["path_handles"]
assert ops.anim.motion_path_handle_select(frame="0", out="1", **args) == {'FINISHED'}
assert ops.anim.motion_path_handles_reset() == {'FINISHED'}, "X with the setup key's handle picked"
assert key(ball, 0)["path_handles"] == 'AUTO', ("its handles back to Auto", key(ball, 0)["path_handles"])
assert ops.anim.motion_path_handle(frame="0", side="OUT", x="-5", y="-3", **args) == {'FINISHED'}
assert key(ball, 0)["path_handles"] == 'ALIGNED', key(ball, 0)["path_handles"]
assert ops.anim.motion_path_select(frame="0", **args) == {'FINISHED'}
assert key(ball, 0)["selected"], "the setup key selected"
assert ops.anim.motion_path_handles_type(mode="AUTO") == {'FINISHED'}, "V Auto with the setup key selected"
assert key(ball, 0)["path_handles"] == 'AUTO', ("V Auto sets it back too", key(ball, 0)["path_handles"])


# --- G on control-point handles picked on two paths: each keeps its own ---------------------------------------------
start()
A = mover({10: (0.0, 0.0), 30: (4.0, 0.0)})
B = mover({10: (0.0, 3.0), 30: (4.0, 3.0)})
for layer in layers:
    layer.selected = layer.id in (A.id, B.id)
a, b = dict(layer=str(A.id)), dict(layer=str(B.id))
assert ops.anim.motion_path_control_add(frame="20", **a) == {'FINISHED'}
assert ops.anim.motion_path_control_add(frame="20", **b) == {'FINISHED'}
assert ops.anim.motion_path_handle_select(frame="30", control="0", **a) == {'FINISHED'}
assert ops.anim.motion_path_controls_align(aligned="0") == {'FINISHED'}
assert key(A, 30)["path_control_handles"][0]["aligned"] is False, "A's control point unaligned"
assert key(B, 30)["path_control_handles"][0]["aligned"] is True, "B's aligned"
assert ops.anim.motion_path_handle_select(frame="30", control="0", control_handle="1", **a) == {'FINISHED'}
assert ops.anim.motion_path_handle_select(frame="30", control="0", control_handle="1", extend="1", **b) == {'FINISHED'}
assert ops.anim.motion_path_handle_select(frame="30", control="0", control_handle="2", extend="1", **b) == {'FINISHED'}
hb_was = key(B, 30)["path_control_handles"][0]
assert ops.anim.motion_path_keys_transform(kind="G", x="0", y="0.5") == {'FINISHED'}
hb = key(B, 30)["path_control_handles"][0]
assert hb["aligned"] is True, ("B's control point still aligned", hb)
assert all(close(hb[s], (hb_was[s][0], hb_was[s][1] + 0.5)) for s in ("in", "out")), \
    ("both of B's handles moved up 0.5", hb_was, hb)
assert key(A, 30)["path_control_handles"][0]["aligned"] is False, "A's still unaligned"


# --- Shape brushes keep a Back stretch's overshoot, and Blend Speed at strength 0 leaves it -------------------------
for interp in ("BACK", "BOUNCE", "ELASTIC"):
    start()
    box = mover({10: (0.0, 0.0), 20: (2.0, 0.0), 30: (2.0, 2.0)})
    box.keyframe_set("position", 20, interp=interp)
    before = [where(box, f)[0] for f in range(10, 21)]
    doc.frame_current = 20
    assert ops.anim.motion_path_stroke(brush="GRAB", points="2,0;2,0.02", radius="0.3", strength="1") == {'FINISHED'}
    after = [where(box, f)[0] for f in range(10, 21)]
    assert key(box, 20)["interp"] == 'CUSTOM', (interp, key(box, 20)["interp"])
    assert close(after, before, 5e-3), (interp, "a tiny Grab keeps its timing, past the key and back", before, after)
    if interp == "BACK":
        assert max(after) > 2.15, ("still going past the key", after)

for interp in ("EASE_IN_OUT", "BACK", "BOUNCE", "ELASTIC"):
    start()
    box = mover({10: (0.0, 0.0), 20: (2.0, 0.0), 30: (2.0, 2.0)})
    box.keyframe_set("position", 20, interp=interp)
    before = [where(box, f) for f in range(10, 31)]
    doc.frame_current = 15
    assert ops.anim.motion_path_stroke(brush="BLEND_SPEED", points="1,0;2,1", radius="3", strength="0") == {'FINISHED'}
    after = [where(box, f) for f in range(10, 31)]
    assert close(after, before, 5e-3), (interp, "Blend Speed at strength 0 moves no dot", before, after)

# A stroke's strength is the brush's, 0 to 1: a million grabs no further than 1 does.
ends = []
for strength in ("1", "1e6"):
    start()
    box = mover({10: (0.0, 0.0), 20: (2.0, 0.0), 30: (2.0, 2.0)})
    doc.frame_current = 20
    assert ops.anim.motion_path_stroke(brush="GRAB", points="2,0;2,0.5", radius="0.3", strength=strength) == {'FINISHED'}
    ends.append(where(box, 20))
assert close(ends[0], ends[1]), ("strength past 1 is 1", ends)


# --- A drawing's path pinned and coloured is the drawing's, saved and opened as such ---------------------------------
def drawing_layer():
    ops.layer.new()
    face = layers.active
    face.key_together = False  # unfolded: its own paths' timing
    assert face.key_whole_drawing
    face.add_curve([(0.0, 0.0), (1.0, 0.0)])
    doc.auto_key = True
    doc.frame_current = 10
    face.set_point(0, (0.0, 1.0))
    doc.frame_current = 20
    face.set_point(0, (0.0, 2.0))
    doc.frame_current = 15
    doc.auto_key = False
    return face


def other_layer():
    ops.layer.new()
    other = layers.active
    other.key_together = False  # unfolded: its own paths' timing
    only(other)
    return other


def paths():
    return {p["label"]: p for p in oanim.wm.motion_paths()}


start()
face = drawing_layer()
other = other_layer()
assert list(paths()) == [other.name], list(paths())
assert ops.anim.motion_path_pin(layer=str(face.id), drawing="1", pinned="1") == {'FINISHED'}
assert sorted(paths()) == sorted([other.name, face.name + " drawing"]), ("the drawing's path pinned, not the origin's",
                                                                         list(paths()))
assert ops.anim.motion_path_pin(layer=str(face.id), drawing="1", pinned="0") == {'FINISHED'}
assert list(paths()) == [other.name], ("unpinned, it goes", list(paths()))

start()
face = drawing_layer()
assert ops.anim.motion_path_style(layer=str(face.id), drawing="1", color="1,0,0") == {'FINISHED'}
assert ops.anim.motion_path_pin(layer=str(face.id), drawing="1", pinned="1") == {'FINISHED'}
other = other_layer()
file = os.path.join(tempfile.mkdtemp(prefix="oanim_motion_path_robust_"), "drawing_style.oanim")
assert ops.wm.save_mainfile(filepath=file) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=file) == {'FINISHED'}
face = next(layer for layer in layers.all() if layer.name == face.name)
other = next(layer for layer in layers.all() if layer.name == other.name)
only(other)
doc.frame_current = 15
opened = paths()
assert sorted(opened) == sorted([other.name, face.name + " drawing"]), ("opened, still the drawing's path",
                                                                        list(opened))
drawn = opened[face.name + " drawing"]
assert drawn["pinned"] and close(drawn["color"][:3], (1.0, 0.0, 0.0)), ("pinned and red", drawn)
assert ops.anim.motion_path_pin(layer=str(face.id), drawing="1", pinned="0") == {'FINISHED'}
assert list(paths()) == [other.name], ("unpinned after opening, it goes", list(paths()))


# --- Grab and Smooth on a drawing's path do nothing: it has no place of its own --------------------------------------
start()
face = drawing_layer()
assert frames(face) == [], frames(face)
for brush in ("GRAB", "SMOOTH"):
    assert ops.anim.motion_path_stroke(brush=brush, layer=str(face.id), drawing="1", points="0,0;0.5,0.5",
                                       radius="0.5", strength="1") == {'CANCELLED'}, brush
    assert frames(face) == [], (brush, "no position keys added to the layer", frames(face))
    assert close(where(face, 10), (0.0, 0.0)), (brush, "the layer stays put")

# --- A path drawn Relative To a parent that turns: its key handles lie along it as drawn ------------------------------
def in_group(layer):
    """`layer` put in a new group, which becomes the active layer."""
    only(layer)
    assert ops.layer.group() == {'FINISHED'}
    return layers.active


def heading(a, b):
    return math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))


start()
doc.path_own_motion = False  # (Own Motion holds the group as shown: Relative To is what's tried here)
ops.layer.new()
ball = layers.active
ball.key_together = False  # unfolded: its own paths' timing
ball.add_curve([(0.0, 0.0), (0.1, 0.0)])
group = in_group(ball)
doc.auto_key = True
doc.frame_current = 40
group.rotation = 90.0
doc.frame_current = 0
for f, x in ((10, 1.0), (20, 2.0), (30, 3.0)):
    doc.frame_current = f
    ball.location_x, ball.location_y = x, 0.0
doc.frame_current = 0
doc.auto_key = False
assert frames(ball) == [0, 10, 20, 30], frames(ball)
group.keyframe_set("rotation", 40, interp="LINEAR")
only(ball)
assert ops.anim.motion_path_relative(to="PARENT", layer=str(group.id)) == {'FINISHED'}
doc.path_before, doc.path_after = 40, 40
for at in (0, 20):
    doc.frame_current = at
    p = path_of(ball)
    pts = p["points"]
    for kf in (10, 20):
        h = next(h for h in p["handles"] if h["frame"] == kf)
        i = kf - p["first"]
        along = heading(pts[i - 1], pts[i + 1])
        assert close(along, 45.0 if at == 20 else 0.0, 0.5), ("the path drawn in the group as it is on", at, along)
        assert math.dist(pts[i], h["out"]) < 0.5, ("the handle starts at its dot", at, kf, h["out"], pts[i])
        assert close(heading(pts[i], h["out"]), along, 0.5), (
            "the handle lies along the path as drawn, not as it turns", at, kf, heading(pts[i], h["out"]), along)


# --- Grab and Smooth take a Relative To Parent path's dots where they are drawn --------------------------------------
start()
doc.path_own_motion = False  # (it holds the group as on the frame shown instead: Relative To is what's tried here)
ops.layer.new()
art = layers.active
art.key_together = False  # unfolded: its own paths' timing
art.key_whole_drawing = False
art.key_together = False  # unfolded: keyed channel by channel
art.add_curve([(0.0, 0.0), (0.1, 0.0)])
group = in_group(art)
doc.auto_key = True
doc.frame_current = 30
group.location_x = 10.0
doc.frame_current = 31
doc.auto_key = False
only(art)
for f, (x, y) in {10: (0, 0), 20: (1, 1), 30: (2, 0)}.items():
    assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(art.id)) == {'FINISHED'}
doc.frame_current = 20
assert ops.anim.motion_path_relative(to="PARENT", layer=str(group.id)) == {'FINISHED'}
d10 = path_of(art)["points"][10 - path_of(art)["first"]]
assert close(d10, (3.333, 0.0), 1e-2), ("key 10's dot drawn where the group is on frame 20", d10)
before = key(art, 10)["value"]
for brush in ("GRAB", "SMOOTH"):
    assert ops.anim.motion_path_stroke(brush=brush, points=f"{d10[0]},{d10[1]};{d10[0]},{d10[1] + 0.5}",
                                       radius="0.3", strength="1") == {'FINISHED'}, (brush, "on the drawn dot")
    assert not close(key(art, 10)["value"], before, 1e-5), (brush, "key 10 moves", key(art, 10)["value"])
    assert ops.ed.undo() == {'FINISHED'}
    assert close(key(art, 10)["value"], before, 1e-5), (brush, "undone", key(art, 10)["value"])
assert ops.anim.motion_path_stroke(brush="GRAB", points="0,0;0,0.5", radius="0.3",
                                   strength="1") == {'CANCELLED'}, "no dot is drawn where key 10 really is"
assert close(key(art, 10)["value"], before), key(art, 10)["value"]

print("test_motion_path_robust: all passed")
