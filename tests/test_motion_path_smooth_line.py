"""A motion path's line is a smooth curve through its dots (one a frame), not straight pieces from dot to dot, so a fast
curve reads as a curve: it passes through every dot, bends between them, never overshoots a dot, keeps a key's corner
(Linear into Linear) a corner, and stays still on held frames. Overshoot is still dashed; nothing else is.

Run with:  OpenAnimate --background --python tests/test_motion_path_smooth_line.py
"""

import math

import oanim

ops, doc, layers, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm

doc.auto_key = False
doc.motion_paths = True
doc.path_all = True
doc.frame_start, doc.frame_end = 10, 30


def mover(keys, interp):
    ops.layer.new()
    lay = layers.active
    lay.key_whole_drawing = False
    lay.add_curve([(0.0, 0.0), (0.1, 0.0)])
    doc.frame_current = 0
    lay.location_x, lay.location_y = -3.0, -3.0
    for other in layers.all():
        other.selected = other.id == lay.id
    layers.active = lay
    for f, (x, y) in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(lay.id)) == {'FINISHED'}
        lay.keyframe_set("position", f, interp=interp)
    return lay


def where(layer, f):
    doc.frame_current = f
    return (layer.location_x, layer.location_y)


def line(layer):
    """The path's line as the viewport draws it: its pieces' ends, in order."""
    path = wm.overlay_colors(layer, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 0.0)["paths"][0]
    pts = [tuple(path["pieces"][0]["a"])] + [tuple(k["b"]) for k in path["pieces"]]
    return pts, path["pieces"]


def off_chord(p, a, b):
    """How far p is from the straight line a-b."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    return abs((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / math.hypot(dx, dy)


def between(pts, a, b):
    """The line's points from dot a to dot b."""
    i = min(range(len(pts)), key=lambda k: math.dist(pts[k], a))
    j = min(range(len(pts)), key=lambda k: math.dist(pts[k], b))
    return pts[min(i, j):max(i, j) + 1]


# --- A fast curve: many points between dots, through every dot, bent off the chord ------------------------------------
arc = mover({10: (0.0, 0.0), 14: (2.0, 2.0), 18: (4.0, 0.0)}, "SMOOTH")
ops.wm.tool_select()
pts, pieces = line(arc)
dots = [where(arc, f) for f in range(10, 19)]
assert len(pts) > 3 * len(dots), ("finer than a piece a frame", len(pts), len(dots))
for d in dots:
    assert min(math.dist(d, p) for p in pts) < 1e-4, ("through every dot", d)
a, b = where(arc, 13), where(arc, 14)
seg = between(pts, a, b)
assert len(seg) > 3, seg
assert max(off_chord(p, a, b) for p in seg) > 2e-3, ("bent between the dots, not straight", seg)
# It never goes past a dot: between two dots the line stays between them along the way.
for f in range(10, 18):
    a, b = where(arc, f), where(arc, f + 1)
    span = math.dist(a, b)
    for p in between(pts, a, b):
        assert math.dist(p, a) <= span + 1e-3 and math.dist(p, b) <= span + 1e-3, ("no overshoot", f, p)
assert not any(k["dashed"] for k in pieces), "solid: no dashes but overshoot"


def no_overshoot(layer, first, last, why):
    pts, _ = line(layer)
    for f in range(first, last):
        a, b = where(layer, f), where(layer, f + 1)
        span = math.dist(a, b)
        for p in between(pts, a, b):
            assert math.dist(p, a) <= span * 1.001 + 1e-4 and math.dist(p, b) <= span * 1.001 + 1e-4, (why, f, p)


# Dots spaced very unevenly round a bend (a strong ease): still never past a dot.
for ease in ("QUART", "EXPO"):
    eased = mover({10: (0.0, 0.0), 30: (8.0, 0.0)}, "EASE_IN_OUT")
    eased.keyframe_set("position", 30, ease=ease)
    assert ops.anim.motion_path_control_add(frame="20", layer=str(eased.id)) == {'FINISHED'}
    assert ops.anim.motion_path_control_move(frame="30", index="0", x="4", y="5", layer=str(eased.id)) == {'FINISHED'}
    no_overshoot(eased, 10, 30, ("eased", ease))

# Two dots close together at the bend, their neighbours far off: the line between them stays a short bend, never a
# swing out past them (as an even-knotted curve would make).
bend = mover({10: (0.0, 0.0), 20: (8.0, 0.0)}, "LINEAR")
assert ops.anim.motion_path_control_add(frame="15", layer=str(bend.id)) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="20", index="0", x="4", y="4", layer=str(bend.id)) == {'FINISHED'}
assert ops.wm.tool_motion_path() == {'FINISHED'}
for f, u in ((14, 0.2), (15, 0.49), (16, 0.51), (17, 0.8)):
    assert ops.anim.motion_path_slide(frame=str(f), progress=str(u), layer=str(bend.id)) == {'FINISHED'}
ops.wm.tool_select()
no_overshoot(bend, 13, 18, "close dots at a bend")

# --- A Linear key's corner stays a corner; the stretches either side are straight -------------------------------------
corner = mover({10: (0.0, 0.0), 14: (2.0, 2.0), 18: (4.0, 0.0)}, "LINEAR")
pts, _ = line(corner)
for f in (13, 14):
    a, b = where(corner, f), where(corner, f + 1)
    assert max(off_chord(p, a, b) for p in between(pts, a, b)) < 1e-4, ("straight into and out of the key", f)

# --- A control point: the line goes through it; a crease there (handles not in line) is a sharp corner ---------------
def turn_at(pts, at):
    """How sharply the line turns at its point nearest `at` (degrees), from the points either side of it."""
    k = min(range(len(pts)), key=lambda j: math.dist(pts[j], at))
    assert math.dist(pts[k], at) < 1e-4, ("the line goes through the control point", at, pts[k])
    b = next(p for p in reversed(pts[:k]) if math.dist(p, at) > 1e-4)
    c = next(p for p in pts[k + 1:] if math.dist(p, at) > 1e-4)
    u, w = (at[0] - b[0], at[1] - b[1]), (c[0] - at[0], c[1] - at[1])
    cos = (u[0] * w[0] + u[1] * w[1]) / (math.hypot(*u) * math.hypot(*w))
    return math.degrees(math.acos(max(-1.0, min(1.0, cos))))


for crease in (False, True):
    # An odd number of frames: the control point, halfway along, falls between two dots.
    vee = mover({10: (0.0, 0.0), 29: (6.0, 0.0)}, "LINEAR")
    va = dict(layer=str(vee.id))
    assert ops.anim.motion_path_control_add(frame="20", **va) == {'FINISHED'}
    assert ops.anim.motion_path_control_move(frame="29", index="0", x="3", y="-2", **va) == {'FINISHED'}
    if crease:
        # Both handles up: the path comes down to the point and goes straight back up (a V).
        assert ops.anim.motion_path_control_handle_move(frame="29", index="0", out="0", x="2.6", y="0", alone="1",
                                                        **va) == {'FINISHED'}
        assert ops.anim.motion_path_control_handle_move(frame="29", index="0", out="1", x="3.4", y="0", alone="1",
                                                        **va) == {'FINISHED'}
    pts, _ = line(vee)
    assert min(math.dist(where(vee, f), (3.0, -2.0)) for f in range(10, 30)) > 0.05, "no dot on it"
    angle = turn_at(pts, (3.0, -2.0))
    tip = (3.0, -2.0)
    bow = []
    for side in (where(vee, 19), where(vee, 20)):
        seg = between(pts, side, tip)
        bow.append(max(off_chord(q, side, tip) for q in seg) / math.dist(side, tip))
    if crease:
        assert angle > 60, ("a crease: a sharp corner at the control point", angle)
        assert max(bow) < 0.02, ("straight into the corner and out, not rounded", bow)
    else:
        assert angle < 5, ("in line: smooth through it", angle)


# --- Sliding dots (a timing change) leaves the line where it is: it is the path's own shape, not a curve through them --
def off_line(q, pts):
    best = 1e9
    for a, b in zip(pts, pts[1:]):
        dx, dy = b[0] - a[0], b[1] - a[1]
        dd = dx * dx + dy * dy
        t = max(0.0, min(1.0, ((q[0] - a[0]) * dx + (q[1] - a[1]) * dy) / dd)) if dd > 1e-14 else 0.0
        best = min(best, math.dist(q, (a[0] + dx * t, a[1] + dy * t)))
    return best


for interp, control in (("SMOOTH", False), ("LINEAR", True)):
    slid = mover({10: (0.0, 0.0), 20: (2.0, 2.0), 30: (4.0, 0.0)}, interp)
    sa = dict(layer=str(slid.id))
    if control:
        assert ops.anim.motion_path_control_add(frame="15", **sa) == {'FINISHED'}
        assert ops.anim.motion_path_control_move(frame="20", index="0", x="0.4", y="1.6", **sa) == {'FINISHED'}
    before, _ = line(slid)
    dots_before = [where(slid, f) for f in range(10, 31)]
    assert ops.wm.tool_motion_path() == {'FINISHED'}
    for f, u in ((12, 0.5), (13, 0.62), (17, 0.95)):
        assert ops.anim.motion_path_slide(frame=str(f), progress=str(u), **sa) == {'FINISHED'}
    ops.wm.tool_select()
    after, _ = line(slid)
    assert max(math.dist(a, b) for a, b in zip(dots_before, [where(slid, f) for f in range(10, 31)])) > 0.1,         "the dots slid"
    worst = max(max(off_line(q, before) for q in after), max(off_line(q, after) for q in before))
    assert worst < 2e-3, ("the line stays put while its dots slide", interp, control, worst)

# A dot backing up along the path: the line goes back along it, from the dot before straight to it.
back = mover({10: (0.0, 0.0), 20: (2.0, 2.0), 30: (4.0, 0.0)}, "SMOOTH")
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert ops.anim.motion_path_slide(frame="12", progress="0.5", layer=str(back.id)) == {'FINISHED'}
assert ops.anim.motion_path_slide(frame="13", progress="0.3", layer=str(back.id)) == {'FINISHED'}
ops.wm.tool_select()
d12, d13 = where(back, 12), where(back, 13)
pts, _ = line(back)
j = next(k for k, q in enumerate(pts) if math.dist(q, d12) < 1e-6)
k = next(k for k in range(j + 1, len(pts)) if math.dist(pts[k], d13) < 1e-6)
drawn = sum(math.dist(a, b) for a, b in zip(pts[j:k], pts[j + 1:k + 1]))
assert math.dist(d12, d13) > 0.1 and drawn < 1.2 * math.dist(d12, d13), ("straight back along it", drawn,
                                                                         math.dist(d12, d13))

# --- Under a flipped parent the shape is flipped too (it bends the way the dots do) --------------------------------------
plain = mover({10: (0.0, 0.0), 20: (2.0, 2.0), 30: (4.0, 0.0)}, "SMOOTH")
assert ops.anim.motion_path_control_add(frame="15", layer=str(plain.id)) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="20", index="0", x="0.4", y="1.6", layer=str(plain.id)) == {'FINISHED'}
reference, _ = line(plain)
flipped = mover({10: (0.0, 0.0), 20: (2.0, 2.0), 30: (4.0, 0.0)}, "SMOOTH")
assert ops.anim.motion_path_control_add(frame="15", layer=str(flipped.id)) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="20", index="0", x="0.4", y="1.6", layer=str(flipped.id)) == {'FINISHED'}
assert ops.layer.group() == {'FINISHED'}
parent = layers.active
doc.frame_current = 0
parent.scale_x = -1.0
for other in layers.all():
    other.selected = other.id == flipped.id
layers.active = flipped
pts, _ = line(flipped)
mirror = [(-x, y) for x, y in reference]
worst = max(max(off_line(q, mirror) for q in pts), max(off_line(q, pts) for q in mirror))
assert worst < 2e-3, ("the flipped path is the plain one mirrored", worst)

# --- Overshoot is dashed, along the smooth line; the rest stays solid -------------------------------------------------
over = mover({10: (0.0, 0.0), 20: (2.0, 1.0)}, "LINEAR")
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert ops.anim.motion_path_slide(frame="18", progress="1.15", layer=str(over.id)) == {'FINISHED'}
ops.wm.tool_select()
_, pieces = line(over)
dashed = [k for k in pieces if k["dashed"]]
assert dashed and len(dashed) < len(pieces), ("dashed past the key and back, solid elsewhere", len(dashed), len(pieces))

# --- Where the path has no shape of its own (a turning bone's tip): a curve through the dots --------------------------
layers.active = plain  # out of the flipped group
ops.layer.new_bone()
rig = layers.active


def tip_mover(name, angles, positions, speeds=None):
    """A bone whose tip's path is drawn through its dots (it turns, or has angle keys): keys Linear."""
    doc.frame_current = 0
    doc.auto_key = True
    ops.armature.select_none()
    ops.armature.bone_add(name=name, head_x="0", head_y="0", tail_x="2", tail_y="0")
    rig.select_bone(name)
    chans = ((f'bones["{name}"].angle', angles), (f'bones["{name}"].position', positions))
    for f in sorted(set(angles) | set(positions)):
        doc.frame_current = f
        if f in angles:
            rig.bone_set(name, angle=angles[f])
        if f in positions:
            rig.bone_set(name, position=positions[f])
        doc.frame_current = f + 1
        doc.frame_current = f
        for ch, keyed in chans:
            if f in keyed and not any(k["frame"] == f for k in rig.keyframes(ch)):
                assert ops.anim.keyframe_toggle(layer=str(rig.id), paths=ch) == {'FINISHED'}
    doc.frame_current = 0
    doc.auto_key = False
    for ch, _ in chans:
        for k in rig.keyframes(ch):
            if k["frame"] > 0:
                rig.keyframe_set(ch, k["frame"], interp="LINEAR")
    if speeds:
        rig.keyframe_set(chans[1][0], max(positions), speeds=speeds)


def tip_at(name, f):
    doc.frame_current = f
    return tuple(next(b for b in rig.bones if b["name"] == name)["tail"])


def tip_line(name):
    paths = wm.overlay_colors(rig, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 0.0)["paths"]
    path = next(p for p in paths if p["label"] == name + " tip")
    return [tuple(path["pieces"][0]["a"])] + [tuple(k["b"]) for k in path["pieces"]]


# A quarter turn (Linear) into key 14, then a straight move (Linear): the tip turns a corner there, and the line out of
# the key is straight, its way taken from its own side only.
ops.wm.tool_select()
tip_mover("elbow", {10: 0.0, 14: math.pi / 2, 18: math.pi / 2}, {10: (0.0, 0.0), 14: (0.0, 0.0), 18: (0.0, 2.0)})
pts = tip_line("elbow")
for f in range(10, 19):
    assert min(math.dist(tip_at("elbow", f), p) for p in pts) < 1e-4, ("through every dot", f)
a, b = tip_at("elbow", 13), tip_at("elbow", 14)
assert max(off_chord(p, a, b) for p in between(pts, a, b)) > 5e-3, "bent along the turn into the key"
for f in (14, 15):
    a, b = tip_at("elbow", f), tip_at("elbow", f + 1)
    assert max(off_chord(p, a, b) for p in between(pts, a, b)) < 1e-4, ("a key's corner stays a corner", f)

# A dash with a near stop in it (long, short, long steps in a line): centripetal, the short step stays between its
# dots, where an even-knotted curve swings past both ends of it.
rig.select_bone("")
tip_mover("dash", {10: 0.0, 16: 0.0}, {10: (0.0, 0.0), 16: (30.0, 0.0)},
          speeds=[0.02 if 10 <= j < 20 else 1.0 for j in range(60)])
steps = [math.dist(tip_at("dash", f), tip_at("dash", f + 1)) for f in range(10, 16)]
assert steps[1] < steps[0] / 20 and steps[1] < steps[2] / 20, ("long, short, long", steps)
pts = tip_line("dash")
for f in range(10, 16):
    a, b = tip_at("dash", f), tip_at("dash", f + 1)
    span = math.dist(a, b)
    for p in between(pts, a, b):
        assert math.dist(p, a) <= span * 1.001 + 1e-4 and math.dist(p, b) <= span * 1.001 + 1e-4, ("no overshoot", f, p)

print("Motion path smooth line: ok")
