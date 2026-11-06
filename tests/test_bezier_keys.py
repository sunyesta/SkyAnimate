"""Bezier keys: an ease shaped by two handles (x1, y1, x2, y2, as a CSS cubic-bezier), so one curve fits stretches of
any length and many keys at once. Set from the timeline's key settings (dragging a handle, or the Even / In / Out /
In-Out shapes) through anim.keys_interpolation_set(bezier=...). Position keys go along the path Smooth makes, at that
timing; across breakdowns the whole span is timed by it. Saved in files and in the new-key settings. The settings
graph shows handles for the modes two handles can shape. The header's key group turns red while it edits selected
keys.

Run with:  OpenAnimate --background --python tests/test_bezier_keys.py
"""

import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def bezier_ease(h, t):
    """The reference: progress at time t along the ease the handles make."""
    x1, y1, x2, y2 = h

    def at(c1, c2, s):
        u = 1 - s
        return 3 * u * u * s * c1 + 3 * u * s * s * c2 + s ** 3

    lo, hi = 0.0, 1.0
    for _ in range(60):
        mid = (lo + hi) / 2
        if at(x1, x2, mid) < t:
            lo = mid
        else:
            hi = mid
    return at(y1, y2, (lo + hi) / 2)


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def interp(**settings):
    return ops.anim.keys_interpolation_set(**{k: str(v) for k, v in settings.items()})


def select(start, end=None):
    ops.anim.keys_deselect_all()
    args = {"frame_start": str(start)}
    if end is not None:
        args["frame_end"] = str(end)
    assert ops.anim.keys_select(**args) == {'FINISHED'}


doc.frame_current = 0
ops.layer.new()
layer = layers.active
layer.add_curve([(0.0, 0.0), (0.1, 0.0)])


def key(frame, path="opacity"):
    return next(k for k in layer.keyframes(path) if k["frame"] == frame)


def opacity_at(frame):
    doc.frame_current = frame
    return layer.opacity


# --- The header: grey while it sets new keys, red while it edits the selected ones -----------------------------------
ops.anim.keys_deselect_all()
header = wm.timeline_key_header()
assert header["parts"] == ["Animate On", "Interpolation", "Key Type", "Settings"], header
assert not header["editing"], "no keys selected: it sets new keys"
grey = header["color"]
assert abs(grey[0] - grey[1]) < 16, ("grey", grey)

# --- A float channel: progress by the handles ---------------------------------------------------------------------
doc.frame_current = 0
layer.opacity = 0.0
doc.frame_current = 10
layer.opacity = 1.0
doc.frame_current = 30
layer.opacity = 0.0
select(10)
header = wm.timeline_key_header()
assert header["editing"], "a key selected: it edits it"
red = header["color"]
assert red[0] > red[1] + 40 and red[0] > red[2] + 40, ("red", red)

h = (0.42, 0.0, 1.0, 1.0)
assert interp(bezier="0.42,0,1,1") == {'FINISHED'}
assert key(10)["interp"] == "BEZIER" and close(key(10)["bezier"], h), key(10)
for f in (2, 5, 8):
    assert close(opacity_at(f), bezier_ease(h, f / 10), 2e-3), (f, opacity_at(f), bezier_ease(h, f / 10))
assert close(opacity_at(10), 1.0) and close(opacity_at(0), 0.0), "it arrives on its keys"
ops.ed.undo()
assert key(10)["interp"] != "BEZIER", "one undo step"

# One curve on stretches of different lengths: 0-10 and 10-30 by the same handles, each over its own frames.
select(1, 30)
assert interp(bezier="0,0,0.58,1") == {'FINISHED'}
h = (0.0, 0.0, 0.58, 1.0)
assert close(opacity_at(5), bezier_ease(h, 0.5), 2e-3)
assert close(opacity_at(20), 1.0 - bezier_ease(h, 0.5), 2e-3), "the 20-frame stretch by the same curve"

# Handles past the top overshoot; times are kept 0..1, heights -2..3.
select(10)
assert interp(bezier="0.3,1.8,0.7,1") == {'FINISHED'}
assert max(opacity_at(f) for f in range(1, 10)) > 1.0 + 1e-3 or key(10)["bezier"][1] > 1.5, "overshoots"
assert interp(bezier="-1,-5,2,9") == {'FINISHED'}
assert close(key(10)["bezier"], (0.0, -2.0, 1.0, 3.0)), key(10)["bezier"]
assert interp(bezier="1,2") == {'CANCELLED'}, "four numbers"

# Saved and opened again.
assert interp(bezier="0.25,0.1,0.75,0.9") == {'FINISHED'}
path = str(pathlib.Path(tempfile.mkdtemp()) / "bezier.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = layers.active
assert key(10)["interp"] == "BEZIER" and close(key(10)["bezier"], (0.25, 0.1, 0.75, 0.9)), key(10)

# --- Position: along the path Smooth makes, at the handles' timing ---------------------------------------------------
ops.layer.new()
ball = layers.active
ball.add_curve([(0.0, 0.0), (0.1, 0.0)])
for f, (x, y) in {0: (0.0, 0.0), 10: (1.0, 1.0), 20: (2.0, 0.0)}.items():
    doc.frame_current = f
    ball.location_x, ball.location_y = x, y


def where(f):
    doc.frame_current = f
    return (ball.location_x, ball.location_y)


smooth = [where(f) for f in range(0, 21)]


def off_path(p):
    best = 1e9
    for a, b in zip(smooth, smooth[1:]):
        ab = (b[0] - a[0], b[1] - a[1])
        t = max(0.0, min(1.0, ((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1]) / max(ab[0] ** 2 + ab[1] ** 2, 1e-12)))
        best = min(best, math.hypot(p[0] - a[0] - ab[0] * t, p[1] - a[1] - ab[1] * t))
    return best


before = where(15)
ops.anim.keys_deselect_all()
assert ops.anim.keys_select(frame_start="20", layers=str(ball.id)) == {'FINISHED'}
assert interp(bezier="0.42,0,1,1") == {'FINISHED'}
assert next(k for k in ball.keyframes("position") if k["frame"] == 20)["interp"] == "BEZIER"
assert all(off_path(where(f)) < 0.02 for f in range(10, 21)), [off_path(where(f)) for f in range(10, 21)]
assert where(15) != before and where(15)[0] < before[0], ("slower out of key 10: behind Smooth", where(15), before)
assert close(where(20), (2.0, 0.0)) and close(where(10), (1.0, 1.0))

# --- After a breakdown: its own stretch, timed by the key's handles as after any key ---------------------------------
ops.layer.new()
span = layers.active
span.add_curve([(0.0, 0.0), (0.1, 0.0)])
for f, v in {0: 0.0, 10: 0.5, 20: 1.0}.items():
    doc.frame_current = f
    span.opacity = v
span.keyframe_set("opacity", 10, type="BREAKDOWN")
ops.anim.keys_deselect_all()
assert ops.anim.keys_select(frame_start="20", layers=str(span.id)) == {'FINISHED'}
assert interp(bezier="0.42,0,0.58,1") == {'FINISHED'}
h = (0.42, 0.0, 0.58, 1.0)
e = lambda x: bezier_ease(h, x)


def span_at(f):
    doc.frame_current = f
    return span.opacity


assert close(span_at(15), 0.5 + 0.5 * e(0.5), 2e-3), (span_at(15), 0.5 + 0.5 * e(0.5))
assert close(span_at(10), 0.5), "the breakdown's pose on its frame"

# --- New keys: with none selected the handles go to what new keys get -------------------------------------------------
ops.anim.keys_deselect_all()
assert interp(bezier="0.1,0.2,0.3,0.9") == {'FINISHED'}
doc.frame_current = 40
span.opacity = 0.2
doc.frame_current = 0  # an edit becomes a key before the frame changes
made = next(k for k in span.keyframes("opacity") if k["frame"] == 40)
assert made["interp"] == "BEZIER" and close(made["bezier"], (0.1, 0.2, 0.3, 0.9)), made
assert interp(interp="SMOOTH") == {'FINISHED'}

# --- The handles the settings graph shows for each mode --------------------------------------------------------------
assert close(wm.interp_bezier_fit("SMOOTH"), (1 / 3, 0.0, 2 / 3, 1.0)), "Smooth is exactly 3t^2 - 2t^3"
assert close(wm.interp_bezier_fit("LINEAR"), (1 / 3, 1 / 3, 2 / 3, 2 / 3))
assert wm.interp_bezier_fit("BOUNCE") is None and wm.interp_bezier_fit("STEP") is None, "two handles can't"
for mode, ease, shape in (("EASE_IN", "QUAD", lambda t: 1 - (1 - t) ** 2), ("EASE_OUT", "QUAD", lambda t: t * t)):
    fit = wm.interp_bezier_fit(mode, ease)
    worst = max(abs(bezier_ease(fit, t / 20) - shape(t / 20)) for t in range(21))
    assert worst < 0.02, (mode, fit, worst)

print("test_bezier_keys: OK")
