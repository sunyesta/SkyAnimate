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

print("test_path_key_add_dots: all passed")
