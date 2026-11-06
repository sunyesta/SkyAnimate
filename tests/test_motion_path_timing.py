"""Motion paths: timing. Dots are coloured by time through their stretch between keys (blue to yellow), only the
stretch being worked on (the frame shown, both sides on a key, and those with dots selected) unless Dot Colour is
Timing (All); Speed and Path Colour as before, and old files' Speed Colours switch read into Dot Colour. The timeline's
timing chart and Path Timing panel read each stretch's progress (motion_path_timing), slide a dot to a share of the way
along (motion_path_slide progress=), set its speed, give a stretch a shape, and step the panel between stretches.

Run with:  OpenAnimate --background --python tests/test_motion_path_timing.py
"""

import json
import os
import shutil
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops

UNLIT = (0.85, 0.85, 0.85, 0.55)
STOPS = [(0.231, 0.322, 0.545), (0.129, 0.569, 0.549), (0.369, 0.788, 0.384), (0.992, 0.906, 0.145)]


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def time_color(t):
    x = min(max(t, 0.0), 1.0) * 3.0
    i = min(int(x), 2)
    f = x - i
    a, b = STOPS[i], STOPS[i + 1]
    return tuple(a[c] + (b[c] - a[c]) * f for c in range(3)) + (1.0,)


def where(layer, frame):
    doc.frame_current = frame
    return (layer.location_x, layer.location_y)


def path_of(layer):
    return next(p for p in oanim.wm.motion_paths() if p["layer"] == layer.id)


def colors(layer):
    """{frame: rgba} of the layer's path dots."""
    p = path_of(layer)
    return {p["first"] + i: tuple(c) for i, c in enumerate(p["dot_colors"])}


def stretches(layer):
    t = next(t for t in oanim.wm.motion_path_timing() if t["label"] == path_of(layer)["label"])
    return t["stretches"]


def progress(layer, start):
    return next(s for s in stretches(layer) if s["start"] == start)["progress"]


doc.motion_paths = True
doc.path_before, doc.path_after = 3, 3
doc.auto_key = False
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert ops.anim.motion_path_brush_set(brush="EDIT", proportional="0") == {'FINISHED'}

# A straight, even line: (0, 0) on 10, (1, 0) on 20, (2, 0) on 30, 0.1 a frame.
ops.layer.new()
line = layers.active
line.add_curve([(0.0, 0.0), (0.1, 0.0)])
doc.frame_current = 0
line.location_x, line.location_y = -5.0, -5.0
for other in layers:
    other.selected = other.id == line.id
layers.active = line
for f, x in ((10, 0.0), (20, 1.0), (30, 2.0)):
    assert ops.anim.motion_path_move(frame=str(f), x=str(x), y="0", layer=str(line.id)) == {'FINISHED'}
    line.keyframe_set("position", f, interp="LINEAR")
assert ops.anim.motion_path_move(frame="30", x="2", y="0", layer=str(line.id)) == {'FINISHED'}
args = dict(layer=str(line.id))

# --- Dot colours ------------------------------------------------------------------------------------------------
assert doc.path_dot_colors == "TIMING_CURRENT"
doc.frame_current = 15
c = colors(line)
for f in range(11, 20):
    assert close(c[f], time_color((f - 10) / 10)), (f, c[f], "the stretch the frame shown is in: time through it")
assert c[11][2] > c[19][2] and c[19][0] > c[11][0], "blue at its start, yellow at its end"
for f in range(21, 30):
    assert close(c[f], UNLIT), (f, c[f], "the next stretch: grey")
assert close(c[10], UNLIT) and close(c[20], UNLIT), "keys aren't coloured dots (drawn white)"

doc.frame_current = 20
c = colors(line)
assert close(c[15], time_color(0.5)) and close(c[25], time_color(0.5)), "on a key: both stretches"

doc.frame_current = 15
assert ops.anim.motion_path_select(frame="25", **args) == {'FINISHED'}
c = colors(line)
assert close(c[15], time_color(0.5)) and close(c[27], time_color(0.7)), "a selected dot's stretch is lit too"
assert ops.anim.motion_path_select(none="1") == {'FINISHED'}
assert close(colors(line)[25], UNLIT)

doc.path_dot_colors = "TIMING_ALL"
c = colors(line)
assert close(c[25], time_color(0.5)) and close(c[12], time_color(0.2)), "Timing (All): every stretch"
doc.path_dot_colors = "PATH"
p = path_of(line)
assert all(close(col[:3], p["color"]) for col in p["dot_colors"]), "Path Colour: the path's own"
doc.path_dot_colors = "SPEED"
p = path_of(line)
still, fast = p["speed_colors"]
for col, sp in zip(p["dot_colors"], p["speed"]):
    sp = min(max(sp, 0.0), 1.0)
    assert close(col[:3], [still[i] + (fast[i] - still[i]) * sp for i in range(3)]), (col, sp, "Speed: by how fast")
doc.path_dot_colors = "TIMING_CURRENT"

# --- An old file's Speed Colours switch becomes Dot Colour -------------------------------------------------------
folder = tempfile.mkdtemp()
try:
    path = os.path.join(folder, "timing.oanim")
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    for old, want in ((False, "PATH"), (True, "TIMING_CURRENT")):
        with zipfile.ZipFile(path) as z:
            items = {n: z.read(n) for n in z.namelist()}
        name = next(n for n in items if n.endswith(".json") and b"path_dot_colors" in items[n])
        data = json.loads(items[name])

        def swap(node):
            if isinstance(node, dict):
                if "path_dot_colors" in node:
                    del node["path_dot_colors"]
                    node["path_speed_colors"] = old
                    return True
                return any(swap(v) for v in node.values())
            if isinstance(node, list):
                return any(swap(v) for v in node)
            return False

        assert swap(data)
        items[name] = json.dumps(data).encode()
        old_path = os.path.join(folder, "old.oanim")
        with zipfile.ZipFile(old_path, "w") as z:
            for n, b in items.items():
                z.writestr(n, b)
        doc.path_dot_colors = "SPEED"
        assert ops.wm.open_mainfile(filepath=old_path) == {'FINISHED'}
        doc = oanim.data.document
        assert doc.path_dot_colors == want, (old, doc.path_dot_colors)
    assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
    doc = oanim.data.document
finally:
    shutil.rmtree(folder, ignore_errors=True)
line = next(l for l in oanim.data.layers if l.id == line.id)
layers = oanim.data.layers
for other in layers:
    other.selected = other.id == line.id
layers.active = line
assert ops.wm.tool_motion_path() == {'FINISHED'}

# --- The timing chart's stretches ---------------------------------------------------------------------------------
st = stretches(line)
assert [(s["start"], s["end"]) for s in st] == [(0, 10), (10, 20), (20, 30)], "from the setup frame on"
assert close(st[1]["progress"], [k / 10 for k in range(11)]), st[0]["progress"]

# Sliding a dot to a share of the way along (the chart's ticks, the panel's dots and its % field).
doc.frame_current = 12
assert ops.anim.motion_path_slide(frame="15", progress="0.7", **args) == {'FINISHED'}
assert doc.frame_current == 12, "the frame shown stays"
assert close(where(line, 15), (0.7, 0.0)), where(line, 15)
assert close(where(line, 14), (0.4, 0.0)) and close(where(line, 20), (1.0, 0.0)), "only it; the key stays"
assert close(progress(line, 10)[5], 0.7)
ops.ed.undo()
assert close(where(line, 15), (0.5, 0.0)), "one undo step"
assert ops.anim.motion_path_slide(frame="15", progress="0.7", proportional="1", size="3", falloff="LINEAR",
                                  **args) == {'FINISHED'}
assert close(where(line, 14), (0.4 + 0.2 * 2 / 3, 0.0)), "proportional: the dots around it follow"
ops.ed.undo()
assert ops.anim.motion_path_slide(frame="20", progress="0.5", **args) == {'CANCELLED'}, "not a key"

# --- A stretch's shape -------------------------------------------------------------------------------------------
shapes = {"EASE_IN": lambda t: t * t, "EASE_OUT": lambda t: 1 - (1 - t) ** 2,
          "EASE_IN_OUT": lambda t: t * t * (3 - 2 * t), "EVEN": lambda t: t}
for shape, fn in shapes.items():
    assert ops.anim.motion_path_stretch_shape(shape=shape, frame="13", **args) == {'FINISHED'}, shape
    assert close(progress(line, 10), [fn(k / 10) for k in range(11)]), (shape, progress(line, 10))
    assert close(progress(line, 20), [k / 10 for k in range(11)]), "the next stretch keeps its timing"
    assert close(where(line, 20), (1.0, 0.0)) and close(where(line, 10), (0.0, 0.0)), "the keys stay"
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", frame="13", **args) == {'FINISHED'}
ops.ed.undo()
assert close(progress(line, 10), [k / 10 for k in range(11)]), "one undo step"
assert ops.anim.motion_path_stretch_shape(shape="BOUNCY", frame="13", **args) == {'CANCELLED'}
assert ops.anim.motion_path_stretch_shape(shape="EVEN", frame="20", **args) == {'CANCELLED'}, "on a key"

# --- A dot's speed -------------------------------------------------------------------------------------------------
# Twice the even speed into and out of 15; the rest of the stretch makes up the time (0.75 each).
assert ops.anim.motion_path_dot_speed(frame="15", speed="2", **args) == {'FINISHED'}
u = progress(line, 10)
assert close(u[4], 0.3) and close(u[5], 0.5) and close(u[6], 0.7), u
assert close(where(line, 20), (1.0, 0.0))
ops.ed.undo()
assert close(progress(line, 10)[4], 0.4)
assert ops.anim.motion_path_dot_speed(frame="10", speed="2", **args) == {'CANCELLED'}, "on a key"

# --- The Path Timing panel: the stretch picked, its arrows picking the one beside -----------------------------------
assert ops.anim.motion_path_select(none="1") == {'FINISHED'}
doc.frame_current = 15
panel = oanim.wm.motion_path_timing_panel()
assert (panel["start"], panel["end"]) == (10, 20) and panel["layer"] == line.id, "the frame shown's stretch"
assert ops.anim.motion_path_select(frame="25", **args) == {'FINISHED'}
panel = oanim.wm.motion_path_timing_panel()
assert (panel["start"], panel["end"]) == (20, 30), "a dot picked: its stretch"
assert close(panel["progress"], [k / 10 for k in range(11)])
assert ops.anim.motion_path_timing_step(step="-1") == {'FINISHED'}
assert oanim.wm.motion_path_timing_panel()["end"] == 20
assert ops.anim.motion_path_timing_step(step="-1") == {'FINISHED'}
assert oanim.wm.motion_path_timing_panel()["end"] == 10, "the stretch from the setup frame"
assert ops.anim.motion_path_timing_step(step="-1") == {'CANCELLED'}, "no stretch before the first"
assert ops.anim.motion_path_timing_step(step="1") == {'FINISHED'}
assert ops.anim.motion_path_timing_step(step="1") == {'FINISHED'}
assert oanim.wm.motion_path_timing_panel()["end"] == 30
assert ops.anim.motion_path_timing_step(step="1") == {'CANCELLED'}, "none after the last"
assert ops.anim.motion_path_select(frame="20", **args) == {'FINISHED'}
assert oanim.wm.motion_path_timing_panel()["end"] == 20, "a key picked: the stretch arriving at it"

# A dot picked: its stretch is the selected one, shown whatever the frame.
assert ops.anim.motion_path_select(frame="25", **args) == {'FINISHED'}
assert oanim.wm.motion_path_timing_panel()["end"] == 30
doc.frame_current = 16
assert oanim.wm.motion_path_timing_panel()["end"] == 30, "the picked stretch, not the playhead's"
# Nothing picked: it follows the frame shown, a stretch stepped to shown while the frame stays.
assert ops.anim.motion_path_select(none="1") == {'FINISHED'}
assert oanim.wm.motion_path_timing_panel()["end"] == 20, "the playhead's stretch"
assert ops.anim.motion_path_timing_step(step="1") == {'FINISHED'}
assert oanim.wm.motion_path_timing_panel()["end"] == 30, "stepped to: shown while the frame stays"
doc.frame_current = 5
assert oanim.wm.motion_path_timing_panel()["end"] == 10
doc.frame_current = 22
assert oanim.wm.motion_path_timing_panel()["end"] == 30
assert ops.anim.motion_path_select(none="1") == {'FINISHED'}

# N opens the timeline's sidebar. O isn't there: proportional editing is always on in the chart and the graph.
assert not oanim.wm.motion_path_timing_panel()["sidebar"]
assert oanim.wm.event_simulate("N", area="Timeline")
assert oanim.wm.motion_path_timing_panel()["sidebar"]
assert oanim.wm.event_simulate("N", area="Timeline")
assert not oanim.wm.motion_path_timing_panel()["sidebar"]
was = oanim.wm.path_brush_settings()["proportional"]
oanim.wm.event_simulate("O", area="Timeline")
assert oanim.wm.path_brush_settings()["proportional"] == was, "O no longer toggles it over the timeline"

# The chart's and the graph's own reach, in frames (0 the dot alone), 3 to start with.
s = oanim.wm.path_brush_settings()
assert close(s["chart_size"], 3.0) and close(s["graph_size"], 3.0), s
assert ops.anim.motion_path_brush_set(chart_size="5", graph_size="0") == {'FINISHED'}
s = oanim.wm.path_brush_settings()
assert close(s["chart_size"], 5.0) and close(s["graph_size"], 0.0), s
assert ops.anim.motion_path_brush_set(chart_size="500") == {'FINISHED'}
assert close(oanim.wm.path_brush_settings()["chart_size"], 100.0), "at most 100 frames"
assert ops.anim.motion_path_brush_set(chart_size="3", graph_size="3") == {'FINISHED'}


# --- Shift click: blend the timing there (Blender's Smooth Vertices, on the way along) ------------------------------
def falloff(d, radius):
    if radius <= 0.0 or d >= radius:
        return 0.0
    x = d / radius
    return 1.0 - x * x * (3.0 - 2.0 * x)


def blended(u, start, frame, smoothing, repeat, radius):
    u = list(u)
    w = [falloff(abs(start + k - frame), radius + 1.0) for k in range(len(u))]
    for _ in range(repeat):
        was = list(u)
        for k in range(1, len(u) - 1):
            u[k] = was[k] + ((was[k - 1] + was[k + 1]) / 2 - was[k]) * smoothing * w[k]
    return u


assert ops.anim.motion_path_stretch_shape(shape="EVEN", frame="15", **args) == {'FINISHED'}
assert ops.anim.motion_path_slide(frame="15", progress="0.8", proportional="0", **args) == {'FINISHED'}
spiked = progress(line, 10)
assert close(spiked[5], 0.8) and close(spiked[4], 0.4), spiked
assert tuple(oanim.wm.path_brush_settings()["blend"]) == (0.5, 1, 3.0), "Smoothing 0.5, Repeat 1, Radius 3"
assert ops.anim.motion_path_timing_blend(frame="15", **args) == {'FINISHED'}
u = progress(line, 10)
assert close(u, blended(spiked, 10, 15, 0.5, 1, 3.0)), (u, blended(spiked, 10, 15, 0.5, 1, 3.0))
assert close(u[5], 0.65) and close(u[4], 0.4 + 0.15 * 0.5 * 0.84375), u
assert close(u[2], 0.2) and close(u[8], 0.8), "nothing past the radius moves"
assert close(u[0], 0.0) and close(u[10], 1.0), "the keys stay"
assert close(progress(line, 20), [i / 10 for i in range(11)])

# The Adjust Last Operation panel shows its settings and runs it again with them changed; they're kept.
last = oanim.wm.last_operation()
assert last and last[0] == "anim.motion_path_timing_blend", last
assert close(float(last[1]["smoothing"]), 0.5) and int(last[1]["repeat"]) == 1, last
assert close(float(last[1]["radius"]), 3.0), last
assert oanim.wm.redo_last(smoothing="1", repeat="3") == "FINISHED"
assert close(progress(line, 10), blended(spiked, 10, 15, 1.0, 3, 3.0)), "redone from before, not on top"
assert tuple(oanim.wm.path_brush_settings()["blend"]) == (1.0, 3, 3.0)

# Each click blends again by the same amount, with the last settings.
once = progress(line, 10)
assert ops.anim.motion_path_timing_blend(frame="15", **args) == {'FINISHED'}
last = oanim.wm.last_operation()
assert close(float(last[1]["smoothing"]), 1.0) and int(last[1]["repeat"]) == 3, ("the panel shows them", last)
assert close(progress(line, 10), blended(once, 10, 15, 1.0, 3, 3.0))
ops.ed.undo()
assert close(progress(line, 10), once), "one undo step"

# A Shift drag is a brush: a blend on each frame the mouse reached, in turn (gone over again, again), one undo step.
stroke = once
for f in (14, 15, 16, 15):
    stroke = blended(stroke, 10, f, 1.0, 3, 3.0)
assert ops.anim.motion_path_timing_blend(frames="14,15,16,15", **args) == {'FINISHED'}
assert close(progress(line, 10), stroke), (progress(line, 10), stroke)
last = oanim.wm.last_operation()
assert last[1]["frames"] == "14,15,16,15", last
assert oanim.wm.redo_last(smoothing="0.25") == "FINISHED"
again = once
for f in (14, 15, 16, 15):
    again = blended(again, 10, f, 0.25, 3, 3.0)
assert close(progress(line, 10), again), "the panel runs the whole stroke again"
ops.ed.undo()
assert close(progress(line, 10), once), "the stroke is one undo step"

# Radius 0: the clicked dot alone. On a key with nothing in reach, nothing to do.
assert ops.anim.motion_path_timing_blend(frame="17", smoothing="0.5", repeat="1", radius="0", **args) == {'FINISHED'}
u = progress(line, 10)
assert close(u, blended(once, 10, 17, 0.5, 1, 0.0)) and not close(u[7], once[7]), u
assert all(close(u[k], once[k]) for k in range(11) if k != 7)
assert ops.anim.motion_path_timing_blend(frame="10", **args) == {'CANCELLED'}
assert ops.anim.motion_path_timing_blend(frame="15", smoothing="5", repeat="0", radius="-2", **args) == {'FINISHED'}
assert tuple(oanim.wm.path_brush_settings()["blend"]) == (1.0, 1, 0.0), "kept in range"
assert ops.anim.motion_path_timing_blend(frame="15", smoothing="0.5", repeat="1", radius="3", **args) == {'FINISHED'}
assert ops.anim.motion_path_stretch_shape(shape="EVEN", frame="15", **args) == {'FINISHED'}

# --- Overshoot: past the key and back, or backing up before the start --------------------------------------------
doc.frame_current = 12
assert ops.anim.motion_path_slide(frame="18", progress="1.15", **args) == {'FINISHED'}
u = progress(line, 10)
assert close(u[8], 1.15), u
assert close(where(line, 18), (1.15, 0.0)), ("past the key, on along the way it arrives", where(line, 18))
assert close(where(line, 20), (1.0, 0.0)), "it still arrives on its key"
assert close(where(line, 17), (0.7, 0.0)) and close(where(line, 19), (0.9, 0.0)), "only that dot"
assert path_of(line)["overshoot"] == [18], path_of(line)["overshoot"]
assert ops.anim.motion_path_slide(frame="12", progress="-0.1", **args) == {'FINISHED'}
assert close(where(line, 12), (-0.1, 0.0)), ("back before its start", where(line, 12))
assert path_of(line)["overshoot"] == [12, 18]
assert ops.anim.motion_path_slide(frame="18", progress="5", **args) == {'FINISHED'}
assert close(progress(line, 10)[8], 2.0), "at most twice the way"

# Alt S keeps an overshoot as it scales.
assert ops.anim.motion_path_slide(frame="18", progress="1.15", **args) == {'FINISHED'}
assert ops.anim.motion_path_scale_spacing(frame="15", factor="0.5", **args) == {'FINISHED'}
assert close(progress(line, 10)[8], 1.15), progress(line, 10)

# Saved with the file.
folder = tempfile.mkdtemp()
try:
    path = os.path.join(folder, "over.oanim")
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
    doc = oanim.data.document
    line = next(l for l in oanim.data.layers if l.id == line.id)
    assert close(where(line, 18), (1.15, 0.0)) and close(where(line, 12), (-0.1, 0.0)), "kept"
finally:
    shutil.rmtree(folder, ignore_errors=True)
for other in oanim.data.layers:
    other.selected = other.id == line.id
oanim.data.layers.active = line
assert ops.wm.tool_motion_path() == {'FINISHED'}

# A dot's speed under 0 goes back: past the key on 18 and 19, coming back to it.
assert ops.anim.motion_path_stretch_shape(shape="EVEN", frame="15", **args) == {'FINISHED'}
assert ops.anim.motion_path_dot_speed(frame="19", speed="-0.5", **args) == {'FINISHED'}
u = progress(line, 10)
assert close(u[8], 1.1) and close(u[9], 1.05) and close(u[10], 1.0), u
assert close(where(line, 19), (1.05, 0.0))

# --- The chart's ticks are picked and slid whichever brush is chosen ---------------------------------------------------
for brush in oanim.wm.path_brush_settings()["brushes"]:
    assert oanim.ops.anim.motion_path_brush_set(brush=brush) == {'FINISHED'}, brush
    assert oanim.wm.timing_chart_ticks_editable(), ("the chart's ticks edit with " + brush)
assert oanim.ops.anim.motion_path_brush_set(brush="EDIT") == {'FINISHED'}

print("Motion path timing tests passed")
