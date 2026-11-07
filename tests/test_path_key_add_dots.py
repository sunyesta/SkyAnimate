"""Adding a key or a control point on a motion path leaves every dot where it was: near either end of a stretch timed
along its curve, on a Linear stretch, beside the keys either side (their Auto handles kept), a control point on a plain
Smooth stretch that curves, and a key put in a stretch shaped on the path (I on a frame inside it). A dot backed up
before its start goes back the way the path leaves, even when the start key's handle is next to nothing.

Run with:  OpenAnimate --background --python tests/test_path_key_add_dots.py
"""

import math

import oanim

ops, doc, L, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm


def dots(layer):
    p = next(p for p in wm.motion_paths() if p["label"] == layer.name)
    return {p["first"] + i: q for i, q in enumerate(p["points"])}


def worst(a, b, frames=None):
    return max(math.dist(a[f], b[f]) for f in (frames or a) if f in b)


def path(keys):
    """A fresh layer on the Motion Path tool, its position keyed at `keys` ({frame: (x, y)})."""
    ops.wm.read_homefile()
    doc.auto_key = False
    doc.path_before = doc.path_after = 5
    assert ops.wm.tool_motion_path() == {'FINISHED'}
    ops.layer.new()
    v = L.active
    v.add_curve([(0.0, 0.0), (0.1, 0.0)])
    for layer in L:
        layer.selected = layer.id == v.id
    for f, (x, y) in keys.items():
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(v.id)) == {'FINISHED'}
    return v, dict(layer=str(v.id))


# --- Near either end of an eased stretch (timed along its length) ------------------------------------------------------
v, A = path({10: (0.2, 0.1), 30: (0.6, -0.2)})
assert ops.anim.motion_path_stretch_shape(frame="20", shape="EASE_OUT", **A) == {'FINISHED'}
doc.frame_current = 20
for f in (28, 29, 11):
    p0 = dots(v)
    assert ops.anim.motion_path_key_add(frame=str(f), type="KEYFRAME", **A) == {'FINISHED'}
    assert worst(p0, dots(v)) < 1e-3, (f"a key added on {f}, near an end, moved the dots", worst(p0, dots(v)))
    assert ops.ed.undo() == {'FINISHED'}

# --- On a Linear stretch: the new key arrives as the stretch did --------------------------------------------------------
v, A = path({10: (0.2, 0.1), 30: (0.6, -0.2)})
assert ops.anim.motion_path_key_interp(frame="30", interp="LINEAR", **A) == {'FINISHED'}
doc.frame_current = 20
p0 = dots(v)
assert ops.anim.motion_path_key_add(frame="20", type="KEYFRAME", **A) == {'FINISHED'}
assert [k["interp"] for k in v.keyframes("position") if k["frame"] == 20] == ["LINEAR"], v.keyframes("position")
assert worst(p0, dots(v)) < 1e-3, ("a key added on a Linear stretch moved the dots", worst(p0, dots(v)))

# --- Plain Smooth: neither the stretch it is added in nor those beside it change -----------------------------------------
v, A = path({10: (0.2, 0.1), 20: (0.5, -0.2), 30: (0.6, 0.3), 40: (0.2, 0.4)})
doc.frame_current = 25
p0 = dots(v)
assert ops.anim.motion_path_key_add(frame="27", type="KEYFRAME", **A) == {'FINISHED'}
p1 = dots(v)
assert worst(p0, p1, range(11, 20)) < 1e-4 and worst(p0, p1, range(31, 40)) < 1e-4, "the stretches beside it changed"
assert worst(p0, p1, range(20, 31)) < 2e-3, ("the stretch it was added in changed", worst(p0, p1, range(20, 31)))

# --- A control point on a plain Smooth stretch that curves ----------------------------------------------------------------
v, A = path({10: (0.2, 0.1), 20: (0.5, -0.2), 30: (0.6, 0.3)})
doc.frame_current = 25
for f in (23, 25, 27):
    p0 = dots(v)
    assert ops.anim.motion_path_control_add(frame=str(f), **A) == {'FINISHED'}
    assert worst(p0, dots(v)) < 1e-3, (f"a control point added on {f} slid the dots", worst(p0, dots(v)))
    assert ops.ed.undo() == {'FINISHED'}

# --- I inside a stretch shaped on the path: the rest of it stays ------------------------------------------------------------
v, A = path({10: (0.2, 0.1), 30: (0.6, 0.3)})
assert ops.anim.motion_path_control_add(frame="20", **A) == {'FINISHED'}
key = next(k for k in v.keyframes("position") if k["frame"] == 30)
assert key["interp"] == "CUSTOM" and len(key["path_controls"]) == 1, key
p0 = dots(v)
doc.frame_current = 15
assert ops.anim.keyframe_toggle(layer=str(v.id), paths="position") == {'FINISHED'}
assert 15 in [k["frame"] for k in v.keyframes("position")]
assert worst(p0, dots(v)) < 1e-3, ("keying frame 15 in place reshaped the stretch", worst(p0, dots(v)))

# --- Backed up before the start: back along the way the path leaves ---------------------------------------------------------
# It comes in from (0.3, 0.3), turns at (0, 0) on frame 10 (a flat Auto handle there) and runs along +x to (1, 0).
v, A = path({5: (0.3, 0.3), 10: (0.0, 0.0), 30: (1.0, 0.0), 40: (1.0, 0.5)})
assert ops.anim.motion_path_slide(frame="13", progress="-0.4", **A) == {'FINISHED'}
doc.frame_current = 13
assert abs(v.location_y) < 1e-3 and v.location_x < -0.3, ("backed up sideways", v.location_x, v.location_y)
assert ops.ed.undo() == {'FINISHED'}
assert ops.anim.motion_path_key_interp(frame="30", bezier="0.8,-0.5,0.2,1.5", **A) == {'FINISHED'}
doc.frame_current = 14
assert abs(v.location_y) < 1e-3 and v.location_x < 0.0, ("a timing dipping under 0 went sideways", v.location_x,
                                                         v.location_y)


# --- Holds and 2s / 3s: the new key keeps the hold and the frames played --------------------------------------------------
def poses(layer, a=0, b=45):
    out = []
    for f in range(a, b + 1):
        doc.frame_current = f
        out.append((layer.location_x, layer.location_y))
    return out


def timed(interp, **settings):
    ops.wm.read_homefile()
    doc.auto_key = False
    doc.path_before = doc.path_after = 60
    assert ops.wm.tool_motion_path() == {'FINISHED'}
    ops.layer.new()
    v = L.active
    v.add_curve([(0.0, 0.0), (0.1, 0.0)])
    v.key_whole_drawing = False
    doc.frame_current = 0
    v.location_x, v.location_y = -1.0, -1.0
    for layer in L:
        layer.selected = layer.id == v.id
    A = dict(layer=str(v.id))
    for f, (x, y) in ((10, (0.0, 0.0)), (20, (1.0, 0.5)), (30, (2.0, 0.0)), (40, (3.0, 1.0))):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), **A) == {'FINISHED'}
    for f in (10, 20, 30, 40):
        v.keyframe_set("position", f, interp=interp, **settings)
    return v, A


for interp, settings, frame, keyed in (("LINEAR", {"hold": 3}, 11, 11), ("SMOOTH", {"hold": 3}, 14, 14),
                                       ("SMOOTH", {"interval": 3}, 13, 13), ("LINEAR", {"interval": 2}, 12, 12),
                                       ("SMOOTH", {"interval": 3}, 14, 13), ("SMOOTH", {"hold": 2, "interval": 2}, 15, 14),
                                       ("SMOOTH", {}, 25, 25), ("SMOOTH", {"hold": 3}, 11, 11),
                                       ("EASE_IN_OUT", {"hold": 3}, 12, 12)):
    v, A = timed(interp, **settings)
    p0 = poses(v)
    assert ops.anim.motion_path_key_add(frame=str(frame), type="KEYFRAME", **A) == {'FINISHED'}
    frames = [k["frame"] for k in v.keyframes("position")]
    assert keyed in frames and (frame == keyed or frame not in frames), (interp, settings, frame, frames)
    w = max(math.dist(a, b) for a, b in zip(p0, poses(v)))
    assert w < 1e-4, (f"{interp} {settings}: a key added on {frame} moved a dot by {w:.5f}", v.keyframes("position"))
    k = next(k for k in v.keyframes("position") if k["frame"] == keyed)
    assert k["interval"] == settings.get("interval", 1), k

# A held frame of the start key's own pose: that pose is a key already.
v, A = timed("LINEAR", interval=2)
p0 = poses(v)
assert ops.anim.motion_path_key_add(frame="11", type="KEYFRAME", **A) == {'CANCELLED'}
assert 11 not in [k["frame"] for k in v.keyframes("position")]
assert max(math.dist(a, b) for a, b in zip(p0, poses(v))) < 1e-6

# --- Key Whole Drawing: a key on one point's path keeps every other point's own motion ---------------------------------
for interp in ("EASE_IN_OUT", "SMOOTH"):
    ops.wm.read_homefile()
    doc.path_before = doc.path_after = 60
    assert ops.wm.tool_motion_path() == {'FINISHED'}
    ops.layer.new()
    v = L.active
    assert v.key_whole_drawing
    v.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.5)])
    for layer in L:
        layer.selected = layer.id == v.id
    doc.auto_key = True
    moves = {10: [(0.0, 0.3), (1.2, 0.0), (2.0, 0.9)], 20: [(0.5, 1.0), (1.0, -0.6), (2.6, 0.5)],
             30: [(1.5, 0.2), (1.9, 0.3), (2.0, -0.4)], 40: [(0.4, -0.5), (1.0, 1.0), (3.0, 0.0)]}
    for f, pts in moves.items():
        doc.frame_current = f
        for i, p in enumerate(pts):
            v.set_point(i, p)
        ops.anim.keyframe_insert()
    doc.auto_key = False
    for i in range(3):
        for f in moves:
            v.keyframe_set("points[%d].pos" % i, f, interp=interp)
    v.select_points([0])

    def shape():
        out = []
        for f in range(0, 46):
            doc.frame_current = f
            out.append([tuple(p[:2]) for p in v.points])
        return out

    s0 = shape()
    doc.frame_current = 25
    assert ops.anim.motion_path_key_add(frame="25", layer=str(v.id), point=str(v.point_ids[0])) == {'FINISHED'}
    s1 = shape()
    w = max(math.dist(a[i], b[i]) for a, b in zip(s0, s1) for i in range(3))
    assert w < 1e-3, (f"{interp}: a key on point 0's path moved the drawing by {w:.4f}",
                      v.keyframes("points[1].pos"))
    for i in range(3):
        k = next(k for k in v.keyframes("points[%d].pos" % i) if k["frame"] == 25)
        assert k["type"] == "BREAKDOWN", (i, k)

print("test_path_key_add_dots: all passed")
