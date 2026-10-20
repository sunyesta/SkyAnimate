"""Interpolation modes from the timeline: the Interpolation menu (header dropdown, T, right-click) sets
the selected keys' mode, or with no keys selected the mode new keys get; ease strengths; each mode's
settings; Copy Previous Key; Copy and Paste Interpolation; Use for New Keys; Stepped Preview; and
saving the new settings.

Run with:  SkyAnimate --background --python tests/test_interpolation.py
"""

import math
import pathlib
import tempfile

import sky

doc = sky.data.document
layers = sky.data.layers
doc.frame_current = 0
sky.ops.layer.new()
layer = layers.active
layer.add_curve([(0.0, 0.0), (1.0, 0.0)])


def x_at(frame):
    doc.frame_current = frame
    return layer.location_x


def key(frame, path="position"):
    return next(k for k in layer.keyframes(path) if k["frame"] == frame)


def select(frame):
    assert sky.ops.anim.keys_select(frame_start=str(frame)) == {'FINISHED'}


def interp(**settings):
    return sky.ops.anim.keys_interpolation_set(**{k: str(v) for k, v in settings.items()})


# --- New keys: Smooth unless the menu says otherwise --------------------------------------------------
doc.frame_current = 10
layer.location_x = 1.0
doc.frame_current = 20
assert [k["frame"] for k in layer.keyframes("position")] == [0, 10]
assert key(0)["interp"] == "SMOOTH" and key(10)["interp"] == "SMOOTH"
assert key(0)["ease"] == "QUAD"

# --- The selected keys' mode --------------------------------------------------------------------------
select(0)
assert interp(interp="LINEAR") == {'FINISHED'}
assert key(0)["interp"] == "LINEAR" and key(10)["interp"] == "SMOOTH", "only the selected key"
assert abs(x_at(5) - 0.5) < 1e-5

# Ease strengths: Ease In slows into the next key, Ease Out away from this one.
assert interp(interp="EASE_IN", ease="SINE") == {'FINISHED'}
assert key(0)["interp"] == "EASE_IN" and key(0)["ease"] == "SINE"
assert abs(x_at(5) - math.sin(math.pi / 4)) < 1e-4, "Sine ease in"
interp(interp="EASE_OUT", ease="QUAD")
assert abs(x_at(5) - 0.25) < 1e-4, "Quad ease out"
interp(ease="CUBIC")
assert key(0)["interp"] == "EASE_OUT", "only the strength changed"
assert abs(x_at(5) - 0.125) < 1e-4, "Cubic ease out"
interp(interp="EASE_IN_OUT", ease="EXPO")
assert abs(x_at(5) - 0.5) < 1e-4 and x_at(2) < 0.05, "Strong (Expo) barely moves at first"
assert interp(ease="NOT_A_STRENGTH") == {'CANCELLED'}
assert interp(interp="NOT_A_MODE") == {'CANCELLED'}

# Each mode's settings.
interp(interp="BACK", overshoot=3.0)
assert abs(key(0)["overshoot"] - 3.0) < 1e-6
back_peak = max(x_at(f) for f in range(1, 10))
interp(overshoot=0.5)
assert max(x_at(f) for f in range(1, 10)) < back_peak, "less overshoot"
interp(interp="ELASTIC", elastic_amplitude=2.0, elastic_period=0.5)
assert abs(key(0)["elastic_amplitude"] - 2.0) < 1e-6 and abs(key(0)["elastic_period"] - 0.5) < 1e-6
assert max(x_at(f) for f in range(1, 10)) > 1.0
interp(hold=4, interval=2)
assert key(0)["hold"] == 4 and key(0)["interval"] == 2 and key(0)["interp"] == "ELASTIC"
assert x_at(4) == 0.0, "held"
interp(interval=9)
assert key(0)["interval"] == 3, "on 1s, 2s or 3s"
interp(interp="LINEAR", hold=0, interval=1)

# --- Nothing selected: the mode new keys get --------------------------------------------------------
sky.ops.anim.keys_deselect_all()
assert interp(interp="STEP") == {'FINISHED'}
assert key(0)["interp"] == "LINEAR", "no key changed"
doc.frame_current = 30
layer.location_x = 3.0
doc.frame_current = 31
assert key(30)["interp"] == "STEP", "the new key gets the new-key mode"

# Copy Previous Key: a new key copies the key before it instead.
interp(interp="SMOOTH")
assert sky.ops.anim.interpolation_copy_previous(enable="1") == {'FINISHED'}
doc.frame_current = 40
layer.location_x = 4.0
doc.frame_current = 41
assert key(40)["interp"] == "STEP", "copied from frame 30's key"
sky.ops.anim.interpolation_copy_previous(enable="0")
doc.frame_current = 50
layer.location_x = 5.0
doc.frame_current = 51
assert key(50)["interp"] == "SMOOTH"

# --- Copy and Paste Interpolation, Use for New Keys -----------------------------------------------
select(0)
interp(interp="EASE_OUT", ease="QUINT", hold=2)
assert sky.ops.anim.interpolation_copy() == {'FINISHED'}
select(10)
assert sky.ops.anim.interpolation_paste() == {'FINISHED'}
k10 = key(10)
assert k10["interp"] == "EASE_OUT" and k10["ease"] == "QUINT" and k10["hold"] == 2
assert k10["value"] == (1.0, 0.0), "the value stays"
assert sky.ops.anim.interpolation_use_for_new_keys() == {'FINISHED'}
sky.ops.anim.keys_deselect_all()
assert sky.ops.anim.interpolation_copy() == {'CANCELLED'}, "nothing selected to copy"
doc.frame_current = 60
layer.location_x = 6.0
doc.frame_current = 61
assert key(60)["interp"] == "EASE_OUT" and key(60)["ease"] == "QUINT" and key(60)["hold"] == 2
interp(interp="SMOOTH", ease="QUAD", hold=0)

# Undo takes a mode change back.
select(10)
interp(interp="BOUNCE")
assert key(10)["interp"] == "BOUNCE"
sky.ops.ed.undo()
assert key(10)["interp"] == "EASE_OUT"

# --- A yes/no property's keys stay Step ---------------------------------------------------------------
doc.frame_current = 0
layer.keyframe_insert("visible", 0)
layer.keyframe_insert("visible", 10)
sky.ops.anim.keys_select_all()
interp(interp="LINEAR")
assert all(k["interp"] == "STEP" for k in layer.keyframes("visible"))
assert key(0)["interp"] == "LINEAR"

# --- Stepped Preview: everything holds its key, no key changes ------------------------------------
smooth_5 = x_at(5)
assert 0.0 < smooth_5 < 1.0
assert sky.ops.screen.stepped_preview(enable="1") == {'FINISHED'}
assert x_at(5) == 0.0 and x_at(10) == 1.0, "held from key to key"
assert key(0)["interp"] == "LINEAR", "the keys are untouched"
sky.ops.screen.stepped_preview()
assert abs(x_at(5) - smooth_5) < 1e-6, "toggled back off"

# --- Saved and opened again -----------------------------------------------------------------------------
select(0)
interp(interp="ELASTIC", elastic_amplitude=1.5, elastic_period=0.4)
select(10)
interp(interp="EASE_IN", ease="CIRC")
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_interp_")) / "interp.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = sky.data.layers.active
assert key(0)["interp"] == "ELASTIC" and abs(key(0)["elastic_amplitude"] - 1.5) < 1e-6
assert abs(key(0)["elastic_period"] - 0.4) < 1e-6
assert key(10)["interp"] == "EASE_IN" and key(10)["ease"] == "CIRC"

# The T menu is asked of the timeline.
assert sky.ops.anim.interpolation_menu() == {'FINISHED'}
assert sky.ops.anim.interpolation_settings() == {'FINISHED'}

print("test_interpolation: OK")
