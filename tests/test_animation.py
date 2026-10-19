"""Animated channels: auto-keying, frame 0 as the setup frame, Moho's interpolation modes, holds
and on-twos, I and Alt I, keyframe jumps, undo and saving.

Run with:  SkyAnimate --background --python tests/test_animation.py
"""

import pathlib
import tempfile

import sky

doc = sky.data.document
layer = sky.data.layers.active
assert doc.auto_key, "auto-keying starts on"
assert doc.frame_current == 0, "a new document opens on the setup frame"


def frames(path):
    return [k["frame"] for k in layer.keyframes(path)]


def x_at(frame):
    doc.frame_current = frame
    return layer.location_x


# --- Auto-keying --------------------------------------------------------------------------------
doc.frame_current = 10
layer.location_x = 1.0
doc.frame_current = 20  # changing the frame records the edit at frame 10
assert frames("position") == [0, 10], "the first key also keys frame 0 with the old value"
assert layer.keyframes("position")[0]["value"] == (0.0, 0.0)
assert layer.keyframes("position")[1]["value"] == (1.0, 0.0)
assert layer.location_x == 1.0, "after the last key the value holds"
assert x_at(0) == 0.0 and x_at(10) == 1.0
assert abs(x_at(5) - 0.5) < 1e-5, "Smooth between two keys is a straight line"
assert frames("rotation") == [], "only what changed is keyed"

# Frame 0 is the setup frame: an edit there changes the frame-0 key, not the later ones.
doc.frame_current = 0
layer.location_x = -1.0
doc.frame_current = 10
assert frames("position") == [0, 10] and layer.keyframes("position")[0]["value"] == (-1.0, 0.0)
assert layer.location_x == 1.0
assert abs(x_at(5) - 0.0) < 1e-5

# An unanimated value edited at frame 0 just changes everywhere.
doc.frame_current = 0
layer.opacity = 0.5
doc.frame_current = 7
assert layer.opacity == 0.5 and frames("opacity") == []

# --- Interpolation --------------------------------------------------------------------------------
doc.frame_current = 0
layer.location_x = 0.0
doc.frame_current = 1


def mode(interp, **settings):
    layer.keyframe_set("position", 0, interp=interp, hold=0, interval=1, **settings)
    for name, value in settings.items():
        assert abs(layer.keyframes("position")[0][name] - value) < 1e-6


mode("LINEAR")
assert abs(x_at(5) - 0.5) < 1e-5 and abs(x_at(2) - 0.2) < 1e-5
mode("EASE_IN_OUT")
assert abs(x_at(5) - 0.5) < 1e-5 and x_at(2) < 0.2 and x_at(8) > 0.8, "slow at both ends"
mode("EASE_IN")
assert x_at(2) > 0.2 and x_at(9) < 1.0 and x_at(9) > 0.95, "slows only on arrival"
mode("EASE_OUT")
assert x_at(2) < 0.2 and abs(x_at(5) - 0.25) < 1e-5, "slows only on departure"
mode("STEP")
assert x_at(9) == 0.0 and x_at(10) == 1.0
mode("BACK")
assert max(x_at(f) for f in range(1, 10)) > 1.0, "Back overshoots before settling"
mode("BOUNCE")
assert all(0.0 <= x_at(f) <= 1.0 + 1e-5 for f in range(0, 11))
mode("ELASTIC")
assert max(x_at(f) for f in range(1, 10)) > 1.0
mode("NOISY", noise_amplitude=0.05)
assert any(abs(x_at(f) - f / 10) > 1e-4 for f in range(1, 10)), "Noisy jitters around the line"
assert all(abs(x_at(f) - f / 10) <= 0.05 + 1e-5 for f in range(1, 10))

# Hold and interval (on 2s).
mode("LINEAR")
layer.keyframe_set("position", 0, hold=4)
assert x_at(4) == 0.0 and abs(x_at(7) - 0.5) < 1e-5, "holds 4 frames, then moves"
layer.keyframe_set("position", 0, hold=0, interval=2)
assert abs(x_at(5) - 0.4) < 1e-5 and abs(x_at(4) - 0.4) < 1e-5, "on 2s the value steps every second frame"
layer.keyframe_set("position", 0, interval=1)

# Cycle: after its key the motion plays again from an earlier frame.
doc.frame_current = 20
layer.location_x = 0.0
doc.frame_current = 21
assert frames("position") == [0, 10, 20]
layer.keyframe_set("position", 10, interp="LINEAR")
layer.keyframe_set("position", 20, interp="CYCLE", cycle_back=20)
assert abs(x_at(25) - 0.5) < 1e-5 and abs(x_at(30) - 1.0) < 1e-5 and abs(x_at(45) - 0.5) < 1e-5
layer.keyframe_set("position", 20, interp="LINEAR")

try:
    layer.keyframe_set("position", 0, interp="WOBBLE")
    raise AssertionError("unknown modes are refused")
except ValueError:
    pass

# --- Auto-keying off ------------------------------------------------------------------------------
doc.auto_key = False
doc.frame_current = 15
layer.location_x = 5.0
doc.frame_current = 16
assert frames("position") == [0, 10, 20] and layer.location_x != 5.0, "an animated value snaps back"
layer.rotation = 30.0
doc.frame_current = 3
assert layer.rotation == 30.0 and frames("rotation") == [], "an unanimated value just changes"
doc.auto_key = True

# --- Points --------------------------------------------------------------------------------------
doc.frame_current = 0
curve = layer.add_curve([(0.0, 0.0), (1.0, 0.0)])
doc.frame_current = 12
layer.set_point(1, (1.0, 1.0))
doc.frame_current = 6
assert frames("points[1].pos") == [0, 12] and frames("points[0].pos") == []
assert abs(layer.points[1][1] - 0.5) < 1e-5, "points move between their keys"

# --- I, Alt I and keyframe jumps --------------------------------------------------------------
doc.frame_current = 30
assert sky.ops.anim.keyframe_insert() == {'FINISHED'}
assert 30 in frames("position") and 30 in frames("rotation") and 30 in frames("points[0].pos")
assert 30 in layer.key_frames
sky.ops.ed.undo()
assert 30 not in frames("rotation") and doc.frame_current == 30, "undo removes the keys, keeps the frame"
sky.ops.ed.redo()
assert 30 in frames("rotation")
assert sky.ops.anim.keyframe_delete() == {'FINISHED'} and 30 not in frames("rotation")
assert sky.ops.anim.keyframe_delete() == {'CANCELLED'}, "no keys left on this frame"

doc.frame_current = 0
assert sky.ops.screen.keyframe_jump_next() == {'FINISHED'} and doc.frame_current == 10
assert sky.ops.screen.keyframe_jump_next() == {'FINISHED'} and doc.frame_current == 12
assert sky.ops.screen.keyframe_jump_prev() == {'FINISHED'} and doc.frame_current == 10

# --- Smooth holds still between equal keys (no overshoot) ----------------------------------
doc.frame_current = 0
sky.ops.layer.new()
held = sky.data.layers.active
for f, x in ((0, 0.0), (38, 1.0), (84, 0.0), (116, 0.0)):
    doc.frame_current = f
    held.location_x = x
    held.keyframe_insert("position")
assert [k["frame"] for k in held.keyframes("position")] == [0, 38, 84, 116]
for f in range(84, 117, 4):
    doc.frame_current = f
    assert abs(held.location_x) < 1e-6, (f, held.location_x)
for f in range(0, 85, 2):
    doc.frame_current = f
    assert -1e-6 <= held.location_x <= 1.0 + 1e-6, ("never past its keys", f, held.location_x)
sky.data.layers.active = layer

# --- Editing keys in the timeline ------------------------------------------------------------
doc.frame_current = 0
frames_before = layer.key_frames
assert sky.ops.anim.keys_select(frame_start="10") == {'FINISHED'}
assert sky.ops.anim.keys_offset(frames="5") == {'FINISHED'}
assert 15 in frames("position") and 10 not in frames("position"), frames("position")
assert sky.ops.anim.keys_scale(factor="2") == {'FINISHED'}, "scales about the current frame (0)"
assert 30 in frames("position"), frames("position")
assert 0 in frames("position"), "frame-0 setup keys never move"
sky.ops.ed.undo()
sky.ops.ed.undo()
assert 10 in frames("position")
# Box select a range, then delete with X over the timeline.
assert sky.ops.anim.keys_select(frame_start="11", frame_end="13") == {'FINISHED'}
assert sky.wm.event_simulate("X", area="Timeline")
assert 12 not in layer.key_frames and 10 in layer.key_frames
assert sky.wm.event_simulate("A", area="Timeline")
assert sky.ops.anim.keys_delete() == {'FINISHED'} and frames("position") == [0], "all were selected"
sky.ops.ed.undo()
assert sky.wm.event_simulate("A", alt=True, area="Timeline")
assert sky.ops.anim.keys_delete() == {'CANCELLED'}, "nothing selected"
sky.ops.ed.undo()
sky.ops.ed.undo()
sky.ops.ed.undo()
sky.ops.ed.undo()
assert 12 in layer.key_frames, layer.key_frames

# --- Duplicating keys (Shift D) and hold bars ------------------------------------------------
doc.frame_current = 0
sky.ops.anim.keys_select(frame_start="10")
assert sky.ops.anim.keys_duplicate(frames="30") == {'FINISHED'}
assert 10 in frames("position") and 40 in frames("position")
k10 = next(k for k in layer.keyframes("position") if k["frame"] == 10)
k40 = next(k for k in layer.keyframes("position") if k["frame"] == 40)
assert k10["value"] == k40["value"] and k10["interp"] == k40["interp"]
# Frame 40 copies frame 10 on every channel keyed there; any neighbours that are equal show a hold.
sky.ops.anim.keys_select(frame_start="40")
sky.ops.anim.keys_duplicate(frames="5")
import _sky
spans = [tuple(s) for s in _sky.timeline_hold_spans()]
assert (40, 45) in spans, spans
assert (10, 12) not in spans, "frames where something changes have no bar"
sky.ops.ed.undo()
sky.ops.ed.undo()
sky.ops.ed.undo()
sky.ops.ed.undo()
assert 40 not in frames("position")

# --- Channel rows: keys of one selected layer at a time ----------------------------------------
doc.frame_current = 0
sky.ops.layer.new()
other = sky.data.layers.active
doc.frame_current = 10
other.location_x = 2.0
doc.frame_current = 0
layer.select = True  # both layers are animated: the timeline shows a row for each
assert 10 in other.key_frames and 10 in layer.key_frames
assert sky.ops.anim.keys_select(frame_start="10", layers=str(other.id)) == {'FINISHED'}
assert sky.ops.anim.keys_offset(frames="3") == {'FINISHED'}
assert 13 in other.key_frames and 10 not in other.key_frames
assert 10 in layer.key_frames and 13 not in layer.key_frames, "the other row's keys stay"
assert sky.ops.anim.keys_offset(frames="-3") == {'FINISHED'}
assert 10 in other.key_frames
sky.data.layers.active = layer
layer.select = False

# --- Saved with the document ------------------------------------------------------------------
layer.keyframe_set("position", 0, interp="EASE_OUT", hold=2, interval=3)
saved = {p: layer.keyframes(p) for p in ("position", "points[1].pos")}
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_anim_")) / "anim.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = sky.data.layers.active
for p, keys in saved.items():
    assert layer.keyframes(p) == keys, (p, layer.keyframes(p), keys)
doc = sky.data.document
doc.frame_current = 6
assert abs(layer.points[1][1] - 0.5) < 1e-5, "opened files show their animation"

# --- Rotation is a number, as in Moho: one key can turn a layer round twice -----------------------
sky.ops.layer.new()
spin = sky.data.layers.active
doc.frame_current = 24
spin.rotation = 720.0
doc.frame_current = 0
assert spin.rotation == 0.0
doc.frame_current = 12
assert abs(spin.rotation - 360.0) < 1e-3, spin.rotation
doc.frame_current = 6
assert abs(spin.rotation - 180.0) < 1e-3, "the turn goes the long way round, not back to 0"
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
spin = next(l for l in sky.data.layers if l.name == spin.name)
assert [k["value"] for k in spin.keyframes("rotation")] == [0.0, 720.0]

print("Animation tests passed")
