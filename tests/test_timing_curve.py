"""A Custom stretch's timing is a curve (Key::timing, anim/timing_curve.hh): how far along against the share of the
stretch's time, kept with as few points as put every frame's dot within a hair of where its timing put it (painted
speeds, a slid dot, a shape all become such a curve), so it stretches with the stretch when its keys move. The Path
Timing graph edits it: Edit mode moves its points and handles (anim.timing_point_move, anim.timing_handle_move: an
aligned point's other handle turns with it unless free=1), adds (anim.timing_point_add keeps the curve's shape) and
deletes them (anim.timing_point_delete, never an end); Sculpt mode's Grab and Smooth (anim.timing_sculpt) brush it, Lock
X keeping it from moving in time, and leave as few points as hold every frame. Files store the curve (format 7); an
older file's per-frame speeds open as the curve they make.

Run with:  OpenAnimate --background --python tests/test_timing_curve.py
"""

import json
import math
import os
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def start():
    assert ops.wm.read_homefile() == {'FINISHED'}
    doc.motion_paths = True
    doc.auto_key = False
    assert ops.wm.tool_motion_path() == {'FINISHED'}
    assert ops.anim.motion_path_brush_set(brush="EDIT", proportional="0") == {'FINISHED'}


def key(layer, frame):
    return next(k for k in layer.keyframes("position") if k["frame"] == frame)


def line_layer(keys):
    """A new layer, its origin keyed along the x axis, {frame: x}, every key Linear."""
    ops.layer.new()
    layer = layers.active
    layer.add_curve([(0.0, 0.0), (0.1, 0.0)])
    doc.frame_current = 0
    # (the setup frame on the same line, so the path is straight whatever its handles)
    layer.location_x, layer.location_y = -1.0, 0.0
    for other in layers:
        other.selected = other.id == layer.id
    layers.active = layer
    for f, x in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y="0", layer=str(layer.id)) == {'FINISHED'}
        if f not in [k["frame"] for k in layer.keyframes("position")]:
            doc.frame_current = f
            layer.keyframe_insert("position", f)
        layer.keyframe_set("position", f, interp="LINEAR")
    return layer


def played(layer, first, last):
    """How far along x (0 to 1) the layer is on each frame first..last."""
    out = []
    for f in range(first, last + 1):
        doc.frame_current = f
        out.append(layer.location_x)
    return out


def progress_of(speeds):
    total = sum(speeds)
    u, out = 0.0, [0.0]
    for s in speeds:
        u += s / total
        out.append(u)
    return out


def args(layer, **kw):
    a = dict(layer=str(layer.id), frame="15")
    a.update({k: str(v) for k, v in kw.items()})
    return a


# --- Painted speeds become a curve of few points, every frame where they put it ---------------------------------------
start()
box = line_layer({10: 0.0, 30: 1.0})
speeds = [1.0 + 0.6 * math.sin(i / 3.0) for i in range(20)]
box.keyframe_set("position", 30, speeds=speeds)
k = key(box, 30)
assert k["interp"] == "CUSTOM"
print("20 painted speeds:", len(k["timing"]), "curve points")
assert 2 < len(k["timing"]) < 10, ("a smooth timing needs few points", len(k["timing"]))
assert close(played(box, 10, 30), progress_of(speeds), 5e-4), "every frame where its speeds put it"
assert k["timing"][0][:2] == (0.0, 0.0) and k["timing"][-1][:2] == (1.0, 1.0), "from the stretch's start to its key"
# Noise has nothing to leave out: every frame a point.
noisy = [1.0, 2.0, 0.5, 1.7, 0.6, 1.9, 0.4, 1.5, 0.8, 1.2, 0.3, 2.0, 0.9, 1.1, 0.5, 1.8, 0.7, 1.4, 0.6, 1.0]
box.keyframe_set("position", 30, speeds=noisy)
assert close(played(box, 10, 30), progress_of(noisy), 5e-4)
assert len(key(box, 30)["timing"]) == 21, len(key(box, 30)["timing"])
# Even speeds: no curve at all (it goes evenly).
box.keyframe_set("position", 30, speeds=[1.0] * 20)
assert "timing" not in key(box, 30), key(box, 30)
assert close(played(box, 10, 30), [f / 20 for f in range(21)])

# The shapes are a curve of their ends alone.
for shape, f in (("EASE_IN", lambda t: t * t), ("EASE_OUT", lambda t: 1 - (1 - t) ** 2),
                 ("EASE_IN_OUT", lambda t: t * t * (3 - 2 * t))):
    assert ops.anim.motion_path_stretch_shape(shape=shape, **args(box)) == {'FINISHED'}
    assert len(key(box, 30)["timing"]) == 2, (shape, key(box, 30)["timing"])
    assert close(played(box, 10, 30), [f(i / 20) for i in range(21)], 3e-3), shape

# --- The curve stretches with the stretch -------------------------------------------------------------------------------
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN", **args(box)) == {'FINISHED'}
ops.anim.keys_deselect_all()
assert ops.anim.keys_select(frame_start="30", layers=str(box.id)) == {'FINISHED'}
assert ops.anim.keys_offset(frames="-10") == {'FINISHED'}
assert len(key(box, 20)["timing"]) == 2 and len(key(box, 20)["speeds"]) == 10
assert close(played(box, 10, 20), [(i / 10) ** 2 for i in range(11)], 3e-3), "Ease In over its 10 frames now"

# --- Edit mode: points and handles ---------------------------------------------------------------------------------------
start()
box = line_layer({10: 0.0, 20: 1.0})
assert ops.anim.motion_path_stretch_shape(shape="EASE_IN_OUT", **args(box)) == {'FINISHED'}
before = played(box, 10, 20)
# A point added keeps the curve's shape.
assert ops.anim.timing_point_add(**args(box, x=0.5)) == {'FINISHED'}
t = key(box, 20)["timing"]
assert len(t) == 3 and close(t[1][0], 0.5), t
assert close(played(box, 10, 20), before), "adding a point leaves every frame where it was"
# Moved: the frames follow, the ends stay the keys.
assert ops.anim.timing_point_move(**args(box, index=1, x=0.5, y=0.8)) == {'FINISHED'}
t = key(box, 20)["timing"]
assert close(t[1][:2], (0.5, 0.8)), t
assert close(played(box, 15, 15)[0], 0.8), "half way through, 80% along"
assert played(box, 20, 20)[0] == 1.0 and close(played(box, 10, 10)[0], 0.0)
# Kept between its neighbours in time.
assert ops.anim.timing_point_move(**args(box, index=1, x=1.5, y=0.8)) == {'FINISHED'}
assert key(box, 20)["timing"][1][0] < 1.0, key(box, 20)["timing"]
assert ops.anim.timing_point_move(**args(box, index=1, x=0.5, y=0.8)) == {'FINISHED'}
assert ops.anim.timing_point_move(**args(box, index=0, x=0.2, y=0.3)) == {'CANCELLED'}, "an end is a key"
assert ops.anim.timing_point_move(**args(box, index=2, x=0.2, y=0.3)) == {'CANCELLED'}, "an end is a key"

# A handle moved turns its aligned partner, keeping its length; Alt (free=1) moves it alone.
t = key(box, 20)["timing"]
(_, _, h_in, h_out, aligned) = t[1]
assert aligned
assert ops.anim.timing_handle_move(**args(box, index=1, side="OUT", x=0.15, y=0.15)) == {'FINISHED'}
(_, _, now_in, now_out, _) = key(box, 20)["timing"][1]
assert close(now_out, (0.15, 0.15)), now_out
assert close(math.atan2(-now_in[1], -now_in[0]), math.atan2(0.15, 0.15)), ("the in handle turned into line", now_in)
assert close(math.hypot(*now_in), math.hypot(*h_in)), "keeping its length"
assert ops.anim.timing_handle_move(**args(box, index=1, side="OUT", x=0.1, y=0.0, free=1)) == {'FINISHED'}
(_, _, free_in, free_out, _) = key(box, 20)["timing"][1]
assert close(free_out, (0.1, 0.0)) and close(free_in, now_in), "Alt: the other handle stays"
# A handle points its own way and reaches no further than its segment (the curve stays a function of time).
assert ops.anim.timing_handle_move(**args(box, index=1, side="OUT", x=-0.2, y=0.1)) == {'FINISHED'}
assert key(box, 20)["timing"][1][3][0] >= 0.0, key(box, 20)["timing"][1]
pointing_back = key(box, 20)["timing"]
assert ops.anim.timing_handle_move(**args(box, index=1, side="OUT", x=3.0, y=0.0)) == {'FINISHED'}
assert key(box, 20)["timing"][1][3][0] <= 0.5 + 1e-6, key(box, 20)["timing"][1]
assert ops.anim.timing_handle_move(**args(box, index=0, side="IN", x=0.1, y=0.1)) == {'CANCELLED'}, "the start has no in"
# One undo step each.
ops.ed.undo()
assert key(box, 20)["timing"] == pointing_back, "undone"
# Deleted: never an end.
assert ops.anim.timing_point_delete(**args(box, index=0)) == {'CANCELLED'}
assert ops.anim.timing_point_delete(**args(box, index=1)) == {'FINISHED'}
assert len(key(box, 20)["timing"]) == 2
# On an even stretch, a point makes it Custom from a straight curve.
plain = line_layer({10: 0.0, 20: 1.0})
assert key(plain, 20)["interp"] == "LINEAR"
assert ops.anim.timing_point_add(**args(plain, x=0.3, y=0.6)) == {'FINISHED'}
assert key(plain, 20)["interp"] == "CUSTOM" and len(key(plain, 20)["timing"]) == 3
assert close(played(plain, 13, 13)[0], 0.6)
# Off a stretch: nothing.
assert ops.anim.timing_point_add(layer=str(plain.id), frame="25", x="0.5") == {'CANCELLED'}

# --- Sculpt mode: Grab and Smooth --------------------------------------------------------------------------------------
start()
box = line_layer({10: 0.0, 30: 1.0})
# (an undo step to go back to)
assert ops.anim.motion_path_stretch_shape(shape="EVEN", **args(box)) == {'FINISHED'}
even = key(box, 30)
# Grab across from frame 20 to 24, 1 frame a whole way along so the brush reaches 3 frames in time and a third up:
# the curve's middle moves later, so frame 20 is less far along.
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="20,0.5;24,0.5", radius=3, strength=1, aspect=6)) == \
    {'FINISHED'}
k = key(box, 30)
assert k["interp"] == "CUSTOM", k
grabbed = played(box, 10, 30)
assert grabbed[10] < 0.5 - 0.05, ("the lump moved later in time", grabbed[10])
print("Grab stroke:", len(k["timing"]), "curve points")
assert len(k["timing"]) < 21, "decimated after the stroke"
ops.ed.undo()
assert key(box, 30) == even, "one undo step"
# Lock X: a stroke across moves nothing.
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="20,0.5;24,0.5", radius=3, strength=1, aspect=6,
                                     lock_x=1)) == {'FINISHED'}
assert close(played(box, 10, 30), [f / 20 for f in range(21)]), "Lock X: never in time"
# Up and down it still moves.
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="20,0.5;20,0.7", radius=3, strength=1, aspect=6,
                                     lock_x=1)) == {'FINISHED'}
assert close(played(box, 20, 20)[0], 0.7, 2e-2), played(box, 20, 20)
# Smooth evens a bumpy timing out.
box.keyframe_set("position", 30, speeds=noisy)


def roughness(u):
    return sum(abs(u[i - 1] - 2 * u[i] + u[i + 1]) for i in range(1, len(u) - 1))


rough = roughness(played(box, 10, 30))
assert ops.anim.timing_sculpt(**args(box, brush="SMOOTH", path="12,0.2;20,0.5;28,0.9", radius=6, strength=1,
                                     aspect=20)) == {'FINISHED'}
smooth = roughness(played(box, 10, 30))
print("Smooth: roughness %.3f -> %.3f, %d points" % (rough, smooth, len(key(box, 30)["timing"])))
assert smooth < rough * 0.7, (rough, smooth)
assert len(key(box, 30)["timing"]) < 21
assert ops.anim.timing_sculpt(**args(box, brush="PINCH", path="20,0.5")) == {'CANCELLED'}
assert ops.anim.timing_sculpt(**args(box, brush="GRAB", path="")) == {'CANCELLED'}

# The graph's settings.
assert ops.anim.timing_graph_set(mode="SCULPT", brush="SMOOTH", radius="4", strength="0.3", lock_x="1") == \
    {'FINISHED'}
s = wm.path_brush_settings()
assert (s["graph_mode"], s["timing_brush"], s["lock_x"]) == ("SCULPT", "SMOOTH", True), s
assert close(s["timing_brushes"]["SMOOTH"], (4.0, 0.3)) and close(s["timing_brushes"]["GRAB"], (2.0, 1.0)), s
assert ops.anim.timing_graph_set(mode="EDIT", brush="GRAB", lock_x="0") == {'FINISHED'}

# --- Python: the curve set as it is --------------------------------------------------------------------------------------
box.keyframe_set("position", 30, timing=[(0.0, 0.0, (0, 0), (0.2, 0.0)), (1.0, 1.0, (-0.2, 0.0), (0, 0))])
assert len(key(box, 30)["timing"]) == 2
assert close(played(box, 20, 20)[0], 0.5), "an S curve: half way at half time"
try:
    box.keyframe_set("position", 30, timing=[(0.0, 0.0, (0, 0), (0, 0)), (0.5, 1.0, (0, 0), (0, 0))])
    assert False, "a curve not ending on the key is refused"
except ValueError:
    pass

# --- Files: the curve saved; an older file's speeds open as a curve -----------------------------------------------------
folder = tempfile.mkdtemp(prefix="oanim_test_")
box.keyframe_set("position", 30, speeds=speeds)
kept = played(box, 10, 30)
saved = os.path.join(folder, "timing.oanim")
assert ops.wm.save_mainfile(filepath=saved) == {'FINISHED'}
with zipfile.ZipFile(saved) as archive:
    stored = json.loads(archive.read("document.json"))
text = json.dumps(stored)
assert '"timing"' in text and '"speeds"' not in text, "the curve is stored, not a speed a frame"
assert ops.wm.open_mainfile(filepath=saved) == {'FINISHED'}
box = next(l for l in layers if l.name == box.name)
assert close(played(box, 10, 30), kept, 1e-5), "opened as saved"


def make_old(node):
    """Format 6: a Custom key's speeds, one a frame."""
    if isinstance(node, dict):
        if "timing" in node and isinstance(node["timing"], list) and node.get("interp") == "CUSTOM":
            del node["timing"]
            node["speeds"] = speeds
        for v in node.values():
            make_old(v)
    elif isinstance(node, list):
        for v in node:
            make_old(v)


make_old(stored)
stored["version"] = 6
old = os.path.join(folder, "old.oanim")
with zipfile.ZipFile(saved) as src, zipfile.ZipFile(old, "w") as dst:
    for item in src.infolist():
        data = json.dumps(stored).encode() if item.filename == "document.json" else src.read(item.filename)
        dst.writestr(item, data)
assert ops.wm.open_mainfile(filepath=old) == {'FINISHED'}
box = next(l for l in layers if l.name == box.name)
assert 2 < len(key(box, 30)["timing"]) < 10, key(box, 30)
assert close(played(box, 10, 30), progress_of(speeds), 5e-4), "the old speeds' timing kept"

print("Timing curve: ok")
