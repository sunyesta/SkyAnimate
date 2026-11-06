"""The Motion Path tool's timing brushes (the shelf's Timing): Pinch crowds the dots under the circle together (slower
there), Ctrl (invert=1) spreads them; Slide drags the dots in the circle along the path, those nearer its edge going
less. Neither moves a key or changes the path's shape; Shift (shift=1) gives Blend Speed in both. Each keeps its own
radius and strength.

Run with:  OpenAnimate --background --python tests/test_motion_path_timing_brushes.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def frames(layer):
    return [k["frame"] for k in layer.keyframes("position") if k["frame"] > 0]


def where(layer, frame):
    doc.frame_current = frame
    return (layer.location_x, layer.location_y)


def fresh_line():
    """A layer alone in the paths, going straight (Linear) from (0, 0) on frame 10 to (1, 0) on frame 20."""
    ops.layer.new()
    l = layers.active
    l.add_curve([(0.0, 0.0), (0.1, 0.0)])
    doc.frame_current = 0
    l.location_x, l.location_y = -5.0, -5.0
    for other in layers:
        other.selected = other.id == l.id
    layers.active = l
    for f, x in ((10, 0.0), (20, 1.0)):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y="0", layer=str(l.id)) == {'FINISHED'}
        l.keyframe_set("position", f, interp="LINEAR")
    doc.frame_current = 15
    return l


def gaps(layer):
    xs = [where(layer, f)[0] for f in range(10, 21)]
    return [b - a for a, b in zip(xs, xs[1:])]


def kept(layer, why):
    assert frames(layer) == [10, 20], (why, "no key moved or added", frames(layer))
    assert close(where(layer, 10), (0.0, 0.0)) and close(where(layer, 20), (1.0, 0.0)), (why, "the keys stay")
    assert all(abs(where(layer, f)[1]) < 1e-5 for f in range(10, 21)), (why, "the same straight path")


doc.motion_paths = True
doc.path_before, doc.path_after = 3, 3
doc.auto_key = False
assert ops.wm.tool_motion_path() == {'FINISHED'}

# --- Picking them: each keeps its own size and strength ---------------------------------------------------------------
assert ops.anim.motion_path_brush_set(brush="PINCH", radius="50", strength="0.4") == {'FINISHED'}
assert ops.anim.motion_path_brush_set(brush="SLIDE", radius="70", strength="0.8") == {'FINISHED'}
s = oanim.wm.path_brush_settings()
assert s["brush"] == "SLIDE", s
assert close(s["brushes"]["PINCH"], (50.0, 0.4)) and close(s["brushes"]["SLIDE"], (70.0, 0.8)), s
assert ops.anim.motion_path_brush_set(brush="PINCH") == {'FINISHED'}
assert close(oanim.wm.path_brush_settings()["radius"], 50.0), "Pinch's own again"

# --- Pinch: slower in the middle; Ctrl: faster ------------------------------------------------------------------------
line = fresh_line()
even = gaps(line)
assert ops.anim.motion_path_stroke(brush="PINCH", points="0.5,0", radius="0.3", strength="1") == {'FINISHED'}
g = gaps(line)
assert g[4] < even[4] - 1e-3 and g[4] < g[0], ("the dots crowd together under the brush", g)
kept(line, "Pinch")
ops.ed.undo()
assert next(k for k in line.keyframes("position") if k["frame"] == 20)["interp"] != "CUSTOM", "one undo step"

line = fresh_line()
assert ops.anim.motion_path_stroke(brush="PINCH", points="0.5,0", radius="0.3", strength="1", invert="1") == {
    'FINISHED'}
g = gaps(line)
assert g[4] > even[4] + 1e-3 and g[4] > g[0], ("Ctrl spreads them", g)
kept(line, "Spread")

# Shift with Pinch is Blend Speed (a Linear line has nothing to even out: it stays even).
line = fresh_line()
assert ops.anim.motion_path_stroke(brush="PINCH", shift="1", points="0.5,0", radius="0.3", strength="1") == {
    'FINISHED'}
assert close(gaps(line), even, 1e-4), ("Shift: Blend Speed, not Pinch", gaps(line))

# --- Slide: the dot under the press goes where the mouse goes, along the path, its neighbours less ------------------
line = fresh_line()
assert ops.anim.motion_path_stroke(brush="SLIDE", points="0.5,0;0.7,0", radius="0.35") == {'FINISHED'}
assert close(where(line, 15)[0], 0.7, 0.02), ("the dot slid", where(line, 15))
x = [where(line, f)[0] for f in range(10, 21)]
assert all(a < b for a, b in zip(x, x[1:])), ("still in order", x)
assert 0.45 < x[4] < 0.65 and close(x[1], 0.1), ("its neighbours go less, the far ones hardly", x)
kept(line, "Slide")
ops.ed.undo()
assert next(k for k in line.keyframes("position") if k["frame"] == 20)["interp"] != "CUSTOM", "one undo step"
line = fresh_line()
assert ops.anim.motion_path_stroke(brush="SLIDE", points="0.5,3;0.7,3", radius="0.35") == {'CANCELLED'}, \
    "no dot under it"
# Shift with Slide is Blend Speed too.
assert ops.anim.motion_path_stroke(brush="SLIDE", shift="1", points="0.5,0", radius="0.3", strength="1") == {
    'FINISHED'}
assert close(gaps(line), even, 1e-4), gaps(line)
print("Motion path timing brushes: ok")
