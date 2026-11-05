"""Breakdown keys (Blender's key type BREAKDOWN): a pose on the way between two main keys. The stretches either side are
timed as one movement from main key to main key, so Ease eases only at the main keys; the timing tools shape the span
as one and move the breakdown to the frame the new timing reaches its pose; dragging it in the timeline slides it
between its neighbours, the pose staying. Alt click on a motion path makes one. The type is saved, set with
keyframe_set(type=) or anim.keys_interpolation_set(type=) (new keys' type when no key is selected), and kept together
across Key Whole Drawing. Frame 0, the setup, is never a breakdown.

Run with:  OpenAnimate --background --python tests/test_breakdown_keys.py
"""

import os
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.auto_key = False


def close(a, b, eps=1e-3):
    return abs(a - b) < eps


def x_at(layer, frame):
    doc.frame_current = frame
    return layer.location_x


def key(layer, path, frame):
    return next(k for k in layer.keyframes(path) if k["frame"] == frame)


def frames(layer, path):
    return [k["frame"] for k in layer.keyframes(path)]


def path_of(layer):
    return next(p for p in oanim.wm.motion_paths() if p["layer"] == layer.id)


def stretches(layer):
    return next(t for t in oanim.wm.motion_path_timing() if t["label"] == path_of(layer)["label"])["stretches"]


def only(layer):
    for other in layers:
        other.selected = other.id == layer.id
    layers.active = layer


def line_layer(keys, interp):
    """A layer moving along x through `keys` ({frame: x}), every key `interp`."""
    ops.layer.new()
    lay = layers.active
    lay.add_curve([(0.0, 0.0), (0.1, 0.0)])
    doc.frame_current = 0
    lay.location_x, lay.location_y = -5.0, -5.0
    only(lay)
    for f, x in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y="0", layer=str(lay.id)) == {'FINISHED'}
    for f in keys:
        lay.keyframe_set("position", f, interp=interp)
    return lay


doc.motion_paths = True
assert ops.wm.tool_motion_path() == {'FINISHED'}

# --- A span eased as one: Ease only at the main keys, the pose exact on the breakdown --------------------------------
plain = line_layer({10: 0.0, 20: 1.0, 30: 2.0}, "EASE_IN_OUT")
assert key(plain, "position", 20)["type"] == "KEYFRAME", "keys are main keys unless made breakdowns"
eased_at_20 = x_at(plain, 20) - x_at(plain, 19)
assert eased_at_20 < 0.05, ("a main key: Ease slows into it", eased_at_20)
assert close(x_at(plain, 15), 0.5)

plain.keyframe_set("position", 20, type="BREAKDOWN")
assert key(plain, "position", 20)["type"] == "BREAKDOWN"
assert close(x_at(plain, 20), 1.0), "the breakdown's pose, exactly on its frame"
into, out = x_at(plain, 20) - x_at(plain, 19), x_at(plain, 21) - x_at(plain, 20)
assert into > 0.12 and close(into, out, 1e-2), ("no easing at a breakdown: full speed through it", into, out)
assert x_at(plain, 11) - x_at(plain, 10) < 0.02, "eased out of the main key at the span's start"
assert x_at(plain, 30) - x_at(plain, 29) < 0.02, "eased into the main key at the span's end"
assert x_at(plain, 15) < 0.35, ("the first half of an ease in-out over 10-30, not one over 10-20", x_at(plain, 15))

# One stretch in the timing chart, its breakdown marked.
sp = [s for s in stretches(plain) if s["start"] >= 10]
assert [(s["start"], s["end"]) for s in sp] == [(10, 30)], sp
assert sp[0]["breakdowns"] == [20], sp

# --- Frame 0 is never a breakdown ------------------------------------------------------------------------------------
try:
    plain.keyframe_set("position", 0, type="BREAKDOWN")
    raise AssertionError("frame 0 made a breakdown")
except ValueError:
    pass

# --- Saved and opened ------------------------------------------------------------------------------------------------
path = os.path.join(tempfile.mkdtemp(), "breakdowns.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
plain = next(l for l in layers if l.id == plain.id)
assert key(plain, "position", 20)["type"] == "BREAKDOWN", "saved"
assert key(plain, "position", 10)["type"] == "KEYFRAME"
assert ops.wm.tool_motion_path() == {'FINISHED'}

# --- Dragged in the timeline: the pose stays, only when it is reached changes; it stops short of its neighbours ------
only(plain)
assert ops.anim.keys_deselect_all() == {'FINISHED'}
assert ops.anim.keys_select(frame_start="20", layers=str(plain.id)) == {'FINISHED'}
assert ops.anim.keys_offset(frames="4") == {'FINISHED'}
assert frames(plain, "position")[-3:] == [10, 24, 30], frames(plain, "position")
assert close(x_at(plain, 24), 1.0), "the pose stays, reached later"
assert ops.anim.keys_offset(frames="20") == {'FINISHED'}
assert frames(plain, "position")[-3:] == [10, 29, 30], ("stopped a frame before the main key", frames(plain, "position"))
assert ops.anim.keys_offset(frames="-40") == {'FINISHED'}
assert frames(plain, "position")[-3:] == [10, 11, 30], ("and a frame after the one before", frames(plain, "position"))
assert close(x_at(plain, 11), 1.0)

# A main key dragged is not stopped (it is not a breakdown).
assert ops.anim.keys_deselect_all() == {'FINISHED'}
assert ops.anim.keys_select(frame_start="30", layers=str(plain.id)) == {'FINISHED'}
assert ops.anim.keys_offset(frames="5") == {'FINISHED'}
assert frames(plain, "position")[-1] == 35, frames(plain, "position")

# --- Even spacing moves the breakdown to where the even timing reaches its pose --------------------------------------
lumpy = line_layer({10: 0.0, 14: 1.0, 30: 2.0}, "LINEAR")
lumpy.keyframe_set("position", 14, type="BREAKDOWN")
assert close(x_at(lumpy, 14), 1.0)
assert ops.anim.motion_path_even_spacing(frame="12", layer=str(lumpy.id)) == {'FINISHED'}
assert frames(lumpy, "position")[-3:] == [10, 20, 30], ("halfway along, at the span's even speed", frames(lumpy, "position"))
assert key(lumpy, "position", 20)["type"] == "BREAKDOWN"
assert close(x_at(lumpy, 20), 1.0), "its pose, exactly"
for f in range(11, 30):
    assert close(x_at(lumpy, f) - x_at(lumpy, f - 1), 0.1, 2e-3), (f, x_at(lumpy, f) - x_at(lumpy, f - 1))
assert [s["breakdowns"] for s in stretches(lumpy) if s["start"] == 10] == [[20]]

# Blend Speed in the chart (Shift click) evens the span as one too, the breakdown moving with it.
bumpy = line_layer({10: 0.0, 13: 1.0, 30: 2.0}, "LINEAR")
bumpy.keyframe_set("position", 13, type="BREAKDOWN")
for _ in range(30):
    ops.anim.motion_path_timing_blend(frame="14", smoothing="1", repeat="3", radius="20", layer=str(bumpy.id))
bk = [k for k in bumpy.keyframes("position") if k["type"] == "BREAKDOWN"]
assert len(bk) == 1 and bk[0]["frame"] > 14, ("blended toward even, the breakdown later", bk)
assert close(x_at(bumpy, bk[0]["frame"]), 1.0), "its pose on its new frame"

# --- Alt click on a path: a breakdown, never a main key ------------------------------------------------------------
alt = line_layer({10: 0.0, 30: 2.0}, "SMOOTH")
assert ops.anim.motion_path_key_add(frame="18", layer=str(alt.id)) == {'FINISHED'}
assert key(alt, "position", 18)["type"] == "BREAKDOWN"
assert ops.anim.motion_path_key_add(frame="22", type="KEYFRAME", layer=str(alt.id)) == {'FINISHED'}
assert key(alt, "position", 22)["type"] == "KEYFRAME", "a main key only when asked for"
assert ops.anim.motion_path_key_add(frame="24", type="WOBBLY", layer=str(alt.id)) == {'CANCELLED'}
assert 24 not in frames(alt, "position")
# Alt Shift (the slide along the arc) makes one too.
assert ops.anim.motion_path_breakdown(frame="26", factor="0.5", layer=str(alt.id)) == {'FINISHED'}
assert key(alt, "position", 26)["type"] == "BREAKDOWN"

# --- The new-key type, and the selected keys' ------------------------------------------------------------------------
only(alt)
assert ops.anim.keys_deselect_all() == {'FINISHED'}
assert ops.anim.keys_interpolation_set(type="BREAKDOWN") == {'FINISHED'}
alt.keyframe_insert("position", 40)
assert key(alt, "position", 40)["type"] == "BREAKDOWN", "new keys: breakdowns"
alt.keyframe_insert("rotation", 0)
assert key(alt, "rotation", 0)["type"] == "KEYFRAME", "never on the setup frame"
assert ops.anim.keys_interpolation_set(type="KEYFRAME") == {'FINISHED'}
alt.keyframe_insert("position", 45)
assert key(alt, "position", 45)["type"] == "KEYFRAME"
assert ops.anim.keys_interpolation_set(type="WOBBLY") == {'CANCELLED'}

assert ops.anim.keys_select(frame_start="45", layers=str(alt.id)) == {'FINISHED'}
assert ops.anim.keys_interpolation_set(type="BREAKDOWN") == {'FINISHED'}
assert key(alt, "position", 45)["type"] == "BREAKDOWN", "the selected key made one"
assert key(alt, "position", 45)["interp"] == "SMOOTH", "its interpolation left alone"
assert ops.ed.undo() == {'FINISHED'}
assert key(alt, "position", 45)["type"] == "KEYFRAME", "one undo step"

# The timing chart's menu sets it on the key behind a diamond.
assert ops.anim.motion_path_key_interp(frame="22", type="BREAKDOWN", layer=str(alt.id)) == {'FINISHED'}
assert key(alt, "position", 22)["type"] == "BREAKDOWN"

# Paste Interpolation and Use for New Keys carry the interpolation, not the type.
assert ops.anim.keys_deselect_all() == {'FINISHED'}
assert ops.anim.keys_select(frame_start="22", layers=str(alt.id)) == {'FINISHED'}
assert ops.anim.interpolation_copy() == {'FINISHED'}
assert ops.anim.interpolation_use_for_new_keys() == {'FINISHED'}
alt.keyframe_insert("position", 50)
assert key(alt, "position", 50)["type"] == "KEYFRAME", "Use for New Keys: the interpolation only"
assert ops.anim.keys_deselect_all() == {'FINISHED'}
assert ops.anim.keys_select(frame_start="50", layers=str(alt.id)) == {'FINISHED'}
assert ops.anim.interpolation_paste() == {'FINISHED'}
assert key(alt, "position", 50)["type"] == "KEYFRAME", "Paste Interpolation: the interpolation only"

# --- Key Whole Drawing: the drawing's keys stay together -----------------------------------------------------------
doc.auto_key = True
ops.layer.new()
whole = layers.active
assert whole.key_whole_drawing
only(whole)
doc.frame_current = 0
whole.add_curve([(0.0, 0.0), (1.0, 0.0)])
doc.frame_current = 12
whole.set_point(1, (1.0, 2.0))
doc.frame_current = 24
whole.set_point(1, (1.0, 0.0))
doc.frame_current = 0
assert ops.anim.motion_path_key_interp(frame="12", type="BREAKDOWN", layer=str(whole.id), drawing="1") == {'FINISHED'}
doc.frame_current = 5
for p in ["points[0].pos", "points[1].pos", "points[0].radius", "curves[0].points[1].curvature"]:
    assert key(whole, p, 12)["type"] == "BREAKDOWN", (p, key(whole, p, 12))
assert ops.anim.keys_deselect_all() == {'FINISHED'}
assert ops.anim.keys_select(frame_start="12", layers=str(whole.id)) == {'FINISHED'}
assert ops.anim.keys_interpolation_set(type="KEYFRAME") == {'FINISHED'}
doc.frame_current = 6
for p in ["points[0].pos", "points[1].pos", "points[0].radius"]:
    assert key(whole, p, 12)["type"] == "KEYFRAME", (p, key(whole, p, 12))

# --- Key Together: the group's keys on a frame stay together ---------------------------------------------------------
ops.layer.new()
ta = layers.active
ta.add_curve([(0.0, 0.0), (1.0, 0.0)])
ops.layer.new()
tb = layers.active
tb.add_curve([(0.0, 3.0), (1.0, 3.0)])
for l in layers:
    l.selected = l.id in (ta.id, tb.id)
assert ops.layer.group() == {'FINISHED'}
group = layers.active
group.key_together = True
doc.frame_current = 10
ta.set_point(1, (1.0, 1.0))
doc.frame_current = 20
ta.set_point(1, (1.0, 0.0))
doc.frame_current = 0
assert 10 in frames(tb, "points[0].pos"), frames(tb, "points[0].pos")
# The group's row selects every key on the frame; Key Type makes them all breakdowns.
assert ops.anim.keys_deselect_all() == {'FINISHED'}
only(group)
assert ops.anim.keys_select(frame_start="10", layers=",".join(str(l.id) for l in (group, ta, tb))) == {'FINISHED'}
assert ops.anim.keys_interpolation_set(type="BREAKDOWN") == {'FINISHED'}
doc.frame_current = 3
for lay, p in ((ta, "points[0].pos"), (tb, "points[0].pos"), (tb, "position"), (group, "rotation")):
    assert key(lay, p, 10)["type"] == "BREAKDOWN", (lay.name, p, key(lay, p, 10))

print("breakdown keys: ok")
