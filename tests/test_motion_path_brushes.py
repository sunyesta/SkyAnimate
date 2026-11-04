"""Motion paths, stages 4-6: the Motion Path tool's brushes. Shape brushes (Grab, Smooth) move keys; Blend Speed
evens out how fast a path is gone along, its stretches becoming Custom, each still arriving on its key's frame and
keeping its shape; Ctrl (invert=1) sharpens. Each brush has its own radius and strength; Shift (shift=1) swaps in
Grab's Smooth and Edit's Blend Speed.

Run with:  OpenAnimate --background --python tests/test_motion_path_brushes.py
"""

import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def key(layer, channel, frame):
    return next(k for k in layer.keyframes(channel) if k["frame"] == frame)


def frames(layer, channel="position"):
    return [k["frame"] for k in layer.keyframes(channel) if k["frame"] > 0]


def where(layer, frame):
    doc.frame_current = frame
    return (layer.location_x, layer.location_y)


def fresh_layer(keys, interp=None):
    """A layer alone in the paths, keyed (through an operator: undo steps) at `keys` {frame: (x, y)}."""
    ops.layer.new()
    l = layers.active
    l.add_curve([(0.0, 0.0), (0.1, 0.0)])
    # Its rest away from every key, so each key differs from it and is made.
    doc.frame_current = 0
    l.location_x, l.location_y = -5.0, -5.0
    for other in layers:
        other.selected = other.id == l.id
    layers.active = l
    for f, (x, y) in keys.items():
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(l.id)) == {'FINISHED'}
        if interp:
            l.keyframe_set("position", f, interp=interp)
    return l


doc.motion_paths = True
doc.path_before, doc.path_after = 3, 3
doc.auto_key = False

# --- Picking a brush (stage 4) ---------------------------------------------------------------------------------------
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert oanim.wm.path_brush_settings()["brush"] == "EDIT"
assert ops.anim.motion_path_brush_set(brush="GRAB", radius="80", strength="0.7") == {'FINISHED'}
s = oanim.wm.path_brush_settings()
assert s["brush"] == "GRAB" and close(s["radius"], 80.0) and close(s["strength"], 0.7), s
assert ops.anim.motion_path_brush_set(brush="NOPE") == {'CANCELLED'}
for gone in ("PAINT_SPEED", "FASTER", "SLOWER"):
    assert ops.anim.motion_path_brush_set(brush=gone) == {'CANCELLED'}, gone + " is gone"
# Each brush keeps its own; Edit's are its Shift brush's, Blend Speed's.
assert ops.anim.motion_path_brush_set(brush="SMOOTH", radius="30", strength="0.2") == {'FINISHED'}
assert ops.anim.motion_path_brush_set(brush="EDIT", radius="40", strength="0.9") == {'FINISHED'}
s = oanim.wm.path_brush_settings()
assert close(s["brushes"]["GRAB"], (80.0, 0.7)) and close(s["brushes"]["SMOOTH"], (30.0, 0.2)), s
assert close(s["brushes"]["BLEND_SPEED"], (40.0, 0.9)) and close(s["radius"], 40.0), s
assert ops.anim.motion_path_brush_set(brush="GRAB") == {'FINISHED'}
assert close(oanim.wm.path_brush_settings()["radius"], 80.0), "Grab's own again"
assert ops.anim.motion_path_stroke(brush="EDIT", points="0,0") == {'CANCELLED'}, "Edit isn't a brush"

# --- Grab (stage 5): the keys under the brush follow the mouse by the falloff, none added ---------------------------
box = fresh_layer({10: (0.0, 0.0), 20: (1.0, 0.0), 30: (2.0, 0.0)})
doc.frame_current = 20
assert ops.anim.motion_path_stroke(brush="GRAB", points="1,0;1,1", radius="0.6", strength="1") == {'FINISHED'}
assert frames(box) == [10, 20, 30], "no keys added"
assert close(where(box, 20), (1.0, 1.0)) and close(where(box, 10), (0.0, 0.0)) and close(where(box, 30), (2.0, 0.0))
ops.ed.undo()
assert close(where(box, 20), (1.0, 0.0)), "one undo step"
# A wider brush: the keys either side, 1 away of 1.5, follow by the falloff (1 - (3x^2 - 2x^3), x = 2/3).
x = 1.0 / 1.5
w = 1.0 - x * x * (3.0 - 2.0 * x)
assert ops.anim.motion_path_stroke(brush="GRAB", points="1,0;1,1", radius="1.5", strength="0.5") == {'FINISHED'}
assert close(where(box, 20), (1.0, 0.5)) and close(where(box, 10), (0.0, 0.5 * w)), (where(box, 10), w)

# --- Smooth (stage 5): a lump toward the curve through the keys either side ------------------------------------------
def arc_y(f):
    return 2.0 - (f - 20) ** 2 / 50.0


ball = fresh_layer({f: (f / 10.0, arc_y(f) + (1.0 if f == 20 else 0.0)) for f in (10, 15, 20, 25, 30)})
doc.frame_current = 20
bump = where(ball, 20)
# Shift with Grab is Smooth.
assert ops.anim.motion_path_stroke(brush="GRAB", shift="1", points=f"{bump[0]},{bump[1]}", radius="0.2",
                                   strength="0.5") == {'FINISHED'}
assert close(where(ball, 20), (2.0, arc_y(20) + 0.5)), ("half way, at the middle of the brush", where(ball, 20))
assert close(where(ball, 15), (1.5, arc_y(15))), "outside the brush, as it was"
here = where(ball, 20)
assert ops.anim.motion_path_stroke(brush="SMOOTH", points=f"{here[0]},{here[1]}", radius="0.2", strength="1") == {
    'FINISHED'}
assert close(where(ball, 20), (2.0, arc_y(20))), where(ball, 20)
# Dragged handles ease back to Auto.
assert ops.anim.motion_path_handle(frame="20", side="OUT", x="3", y="3", layer=str(ball.id)) == {'FINISHED'}
assert key(ball, "position", 20)["path_handles"] == "ALIGNED"
here = where(ball, 20)
assert ops.anim.motion_path_stroke(brush="SMOOTH", points=f"{here[0]},{here[1]}", radius="0.2", strength="1") == {
    'FINISHED'}
assert key(ball, "position", 20)["path_handles"] == "AUTO"

# --- Speed (stage 6): Blend Speed evens out the spacing; Ctrl sharpens ----------------------------------------------
def spread(speeds):
    return max(speeds) - min(speeds)


def blend(strength, invert="0", brush="BLEND_SPEED", shift="0"):
    """Blend Speed over the middle of a fresh Ease In/Out stretch (strength 0: just made Custom, as it was; None:
    the brush's own)."""
    l = fresh_layer({10: (0.0, 0.0), 20: (1.0, 0.0)}, interp="EASE_IN_OUT")
    doc.frame_current = 15
    more = {} if strength is None else {"strength": strength}
    assert ops.anim.motion_path_stroke(brush=brush, shift=shift, points="0.5,0", radius="0.3", invert=invert,
                                       **more) == {'FINISHED'}
    return l, key(l, "position", 20)["speeds"]


assert ops.anim.motion_path_stroke(brush="BLEND_SPEED", points="5,5", radius="0.3", strength="0.3") == {'CANCELLED'}
(_, was), (line, blended), (_, sharper) = blend("0"), blend("0.3"), blend("0.3", invert="1")
mid = slice(3, 7)
assert spread(blended[mid]) < spread(was[mid]) < spread(sharper[mid]), (blended, was, sharper)
k = key(line, "position", 20)
assert k["interp"] == "CUSTOM" and len(k["speeds"]) == 10 and close(sum(k["speeds"]), 10.0), k
assert close(where(line, 10), (0.0, 0.0)) and close(where(line, 20), (1.0, 0.0)), "still arrives on its frames"
assert all(abs(where(line, f)[1]) < 1e-5 for f in range(10, 21)), "the same straight path"
# Shift with Edit is Blend Speed.
assert close(blend("0.3", brush="EDIT", shift="1")[1], blended, 1e-5)
# Left out, a stroke's strength is its brush's own.
held = oanim.wm.path_brush_settings()["brush"]
assert ops.anim.motion_path_brush_set(brush="BLEND_SPEED", strength="0.3") == {'FINISHED'}
assert ops.anim.motion_path_brush_set(brush=held) == {'FINISHED'}
assert close(blend(None)[1], blended, 1e-5), "Blend Speed's own strength"

# A straight stretch into a corner stays straight as Custom (its key's handles don't round the corner).
corner = fresh_layer({10: (0.0, 0.0), 20: (1.0, 1.0), 30: (2.0, 0.0)}, interp="LINEAR")
assert ops.anim.motion_path_stroke(brush="BLEND_SPEED", points="0.5,0.5", radius="0.25", strength="1") == {
    'FINISHED'}
assert key(corner, "position", 20)["interp"] == "CUSTOM"
diagonal = [where(corner, f) for f in range(10, 21)]
assert all(abs(p[0] - p[1]) < 1e-4 for p in diagonal), diagonal

# A curved (Smooth) stretch keeps its curve as Custom.
curve = fresh_layer({10: (0.0, 0.0), 20: (1.0, 1.0), 30: (2.0, 0.0)})
shape = [where(curve, f) for f in range(10, 31)]
assert ops.anim.motion_path_stroke(brush="BLEND_SPEED", points="1,1", radius="0.05", strength="0") == {'FINISHED'}
assert key(curve, "position", 20)["interp"] == "CUSTOM"
assert all(close(where(curve, f), shape[f - 10], 2e-3) for f in range(10, 31)), "unchanged until painted"

# --- A bone's tip: its angle's stretches -----------------------------------------------------------------------------
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
for other in layers:
    other.selected = other.id == rig.id
layers.active = rig
assert ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="") == {'FINISHED'}
rig.select_bone("arm")
for f, a in ((10, 0.2), (20, 1.0)):
    assert ops.anim.motion_path_move(frame=str(f), x=str(math.cos(a)), y=str(math.sin(a)), layer=str(rig.id),
                                     bone="arm") == {'FINISHED'}
tip = (math.cos(0.6), math.sin(0.6))
assert ops.anim.motion_path_stroke(brush="BLEND_SPEED", points=f"{tip[0]},{tip[1]}", radius="0.2",
                                   strength="1", layer=str(rig.id), bone="arm") == {'FINISHED'}
k = key(rig, 'bones["arm"].angle', 20)
assert k["interp"] == "CUSTOM" and len(k["speeds"]) == 10, k
doc.frame_current = 20
assert close(next(b for b in rig.bones if b["name"] == "arm")["angle"], 1.0), "arrives on its frame"

# --- Saved with the file ---------------------------------------------------------------------------------------------
painted = key(line, "position", 20)["speeds"]
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_path_brushes_")) / "brushes.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
line = next(l for l in oanim.data.layers if l.id == line.id)
k = key(line, "position", 20)
assert k["interp"] == "CUSTOM" and close(k["speeds"], painted, 1e-4), k

print("Motion path brush tests passed")
