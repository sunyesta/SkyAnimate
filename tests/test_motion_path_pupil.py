"""Motion paths: what a sweep of a pupil's path (a controller's knob, Custom timing through control points, an
overshoot) found, each kept fixed. Dots through control points go as the stretch's speeds say (no bunching where the
path without them would turn sharply, so Blend Speed evens what is seen); undoing a stroke keeps the paths the Motion
Path tool turned on; Slide on a stretch with control points lands where it is dragged, keeps an overshoot elsewhere
in the stretch, and a dot in the overshoot follows the mouse there; a key added on a stretch with handles, eases,
Custom timing or control points, or past its key in an overshoot, leaves every dot where it was, and so does a control
point added; G past a controller's box goes as far as the edge.

Run with:  OpenAnimate --background --python tests/test_motion_path_pupil.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def start():
    assert ops.wm.read_homefile() == {'FINISHED'}
    doc.auto_key = False
    doc.motion_paths = True
    doc.path_before, doc.path_after = 3, 3
    assert ops.wm.tool_motion_path() == {'FINISHED'}


def mover(keys):
    """A new layer, its origin moved to keys {frame: (x, y)} through the path."""
    ops.layer.new()
    layer = layers.active
    layer.add_curve([(0.0, 0.0), (0.1, 0.0)])
    doc.frame_current = 0
    layer.location_x, layer.location_y = -5.0, -5.0
    for other in layers:
        other.selected = other.id == layer.id
    layers.active = layer
    for f, (x, y) in keys.items():
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(layer.id)) == {'FINISHED'}
    return layer


def path_of(layer):
    return next(p for p in oanim.wm.motion_paths() if p["layer"] == layer.id)


def dots(layer):
    """{frame: where its dot is drawn}."""
    p = path_of(layer)
    return {p["first"] + i: tuple(q) for i, q in enumerate(p["points"])}


def progress(layer, first):
    p = path_of(layer)
    row = next(t for t in oanim.wm.motion_path_timing() if t["label"] == p["label"])
    return next(s for s in row["stretches"] if s["start"] == first)["progress"]


def key(layer, frame):
    return next(k for k in layer.keyframes("position") if k["frame"] == frame)


def curvy(layer):
    """Long handles on 10 and 30 (the path without control points would loop), a control point, Custom timing."""
    args = dict(layer=str(layer.id))
    doc.frame_current = 20
    assert ops.anim.motion_path_handle(frame="10", side="OUT", x="0.8", y="-0.6", **args) == {'FINISHED'}
    assert ops.anim.motion_path_handle(frame="30", side="IN", x="0.2", y="0.6", **args) == {'FINISHED'}
    assert ops.anim.motion_path_control_add(frame="20", **args) == {'FINISHED'}
    assert ops.anim.motion_path_control_move(frame="30", index="0", x="0.5", y="0.3", **args) == {'FINISHED'}
    assert ops.anim.motion_path_dot_speed(frame="15", speed="1.5", **args) == {'FINISHED'}
    assert key(layer, 30)["interp"] == "CUSTOM" and len(key(layer, 30)["path_controls"]) == 1
    doc.frame_current = 20


def spread(layer, first, last):
    """How far apart the dots of first..last are by length as a share of their progress: (least, most)."""
    at, u = dots(layer), progress(layer, first)
    run, ratios = 0.0, []
    for f in range(first + 1, last + 1):
        run += math.dist(at[f - 1], at[f])
        if u[f - first] > 0.02:
            ratios.append(run / u[f - first])
    return min(ratios), max(ratios)


# --- Through control points, the dots go as the speeds say -------------------------------------------------------------
start()
ball = mover({10: (0.0, 0.0), 30: (1.0, 0.0)})
curvy(ball)
least, most = spread(ball, 10, 30)
assert most / least < 1.02, ("each dot as far along the path as its progress", least, most)

# --- Undo keeps the paths the Motion Path tool showed ----------------------------------------------------------------
assert ops.wm.read_homefile() == {'FINISHED'}
doc.auto_key = False
assert not doc.motion_paths
ball = mover({10: (0.0, 0.0), 30: (1.0, 0.0)})
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert doc.motion_paths, "the tool turns them on"
doc.frame_current = 20
at = dots(ball)
assert ops.anim.motion_path_stroke(brush="BLEND_SPEED", points=f"{at[12][0]},{at[12][1]};{at[18][0]},{at[18][1]}",
                                   radius="0.2", strength="1") == {'FINISHED'}
assert ops.ed.undo() == {'FINISHED'}
assert doc.motion_paths, "undoing the stroke leaves them shown"
assert ops.ed.redo() == {'FINISHED'}
assert doc.motion_paths

# --- Slide on a stretch with control points: where it is dragged -----------------------------------------------------
start()
ball = mover({10: (0.0, 0.0), 30: (1.0, 0.0)})
curvy(ball)
at = dots(ball)
for frame in (13, 22):
    to = tuple((at[frame][i] + at[frame + 1][i]) / 2 for i in (0, 1))
    assert ops.anim.motion_path_slide(frame=str(frame), x=str(to[0]), y=str(to[1]), proportional="0",
                                      layer=str(ball.id)) == {'FINISHED'}
    now = dots(ball)[frame]
    step = math.dist(at[frame], at[frame + 1])
    assert math.dist(now, to) < 0.1 * step, ("slid onto the place dragged to", frame, now, to, step)
    assert ops.ed.undo() == {'FINISHED'}

# --- Slide keeps an overshoot elsewhere in the stretch, and a dot in it follows the mouse ----------------------------
start()
ball = mover({10: (0.0, 0.0), 30: (1.0, 0.0)})
ball.keyframe_set("position", 30, speeds=[1.6] * 14 + [-0.9] * 3 + [0.3] * 3)
doc.frame_current = 20
before = progress(ball, 10)
assert max(before) > 1.05, ("past the key and back", before)
at = dots(ball)
to = tuple((at[13][i] + at[14][i]) / 2 for i in (0, 1))
assert ops.anim.motion_path_slide(frame="13", x=str(to[0]), y=str(to[1]), proportional="0",
                                  layer=str(ball.id)) == {'FINISHED'}
after = progress(ball, 10)
assert close(after[20 - 10:], before[20 - 10:], 1e-4), ("the overshoot stays", before, after)
ball.keyframe_set("position", 30, speeds=[1.6] * 14 + [-0.9] * 3 + [0.3] * 3)
assert close(progress(ball, 10), before, 1e-5)
# Dot 26, coming back from past the key: slid to where 25 is, it goes there.
to = at[25]
assert ops.anim.motion_path_slide(frame="26", x=str(to[0]), y=str(to[1]), proportional="0",
                                  layer=str(ball.id)) == {'FINISHED'}
assert math.dist(dots(ball)[26], to) < 1e-3, ("in the overshoot, the dot follows", dots(ball)[26], to)
assert progress(ball, 10)[26 - 10] > 1.0, progress(ball, 10)

# Through a control point, an overshoot goes on past the key the way the line arrives there.
start()
ball = mover({10: (0.0, 0.0), 30: (1.0, 0.0)})
doc.frame_current = 20
assert ops.anim.motion_path_control_add(frame="20", layer=str(ball.id)) == {'FINISHED'}
ball.keyframe_set("position", 30, speeds=[1.6] * 14 + [-0.9] * 3 + [0.3] * 3)
assert len(key(ball, 30)["path_controls"]) == 1
doc.frame_current = 20
u = progress(ball, 10)
far = max(range(len(u)), key=lambda j: u[j])
assert u[far] > 1.05, u
x, y = dots(ball)[10 + far]
assert x > 1.0 + 0.5 * (u[far] - 1.0) and abs(y) < 1e-3, ("past the key", (x, y), u[far])


# --- A key added (Alt click) leaves every dot where it was ------------------------------------------------------------
def setup_for_key(interp, handles, custom, control):
    start()
    layer = mover({1: (0.0, 0.0), 21: (1.0, 0.3)})
    args = dict(layer=str(layer.id))
    layer.keyframe_set("position", 21, interp=interp)
    doc.frame_current = 11
    if handles:
        assert ops.anim.motion_path_handle(frame="1", side="OUT", x="0.2", y="0.6", **args) == {'FINISHED'}
        assert ops.anim.motion_path_handle(frame="21", side="IN", x="1.1", y="-0.3", **args) == {'FINISHED'}
    if control:
        assert ops.anim.motion_path_control_add(frame="15", **args) == {'FINISHED'}
    if custom:
        assert ops.anim.motion_path_dot_speed(frame="5", speed="2.5", **args) == {'FINISHED'}
    doc.frame_current = 11
    return layer


for interp, handles, custom, control in (("EASE_IN_OUT", False, False, False), ("SMOOTH", True, False, False),
                                         ("LINEAR", True, False, False), ("SMOOTH", False, True, False),
                                         ("LINEAR", True, True, True), ("BACK", True, False, False)):
    case = (interp, handles, custom, control)
    for kind in ("KEYFRAME", "BREAKDOWN"):
        layer = setup_for_key(*case)
        was = dots(layer)
        assert ops.anim.motion_path_key_add(frame="8", type=kind, layer=str(layer.id)) == {'FINISHED'}, case
        assert 8 in [k["frame"] for k in layer.keyframes("position")]
        now = dots(layer)
        moved = max(math.dist(was[f], now[f]) for f in range(1, 22))
        assert moved < 2e-3, ("no dot moves", case, kind, moved)

# --- A control point added leaves every dot where it was; where a dot is past its key it isn't added -----------------
for frame in (13, 17, 24, 28):
    start()
    ball = mover({10: (0.0, 0.0), 30: (1.0, 0.0)})
    curvy(ball)
    was = dots(ball)
    assert ops.anim.motion_path_control_add(frame=str(frame), layer=str(ball.id)) == {'FINISHED'}
    assert len(key(ball, 30)["path_controls"]) == 2
    moved = max(math.dist(was[f], dots(ball)[f]) for f in range(10, 31))
    assert moved < 1e-3, ("no dot moves", frame, moved)
start()
ball = mover({10: (0.0, 0.0), 30: (1.0, 0.0)})
ball.keyframe_set("position", 30, speeds=[1.6] * 14 + [-0.9] * 3 + [0.3] * 3)
doc.frame_current = 20
assert progress(ball, 10)[25 - 10] > 1.0
assert ops.anim.motion_path_control_add(frame="25", layer=str(ball.id)) == {'CANCELLED'}, "past the key: not on the path"
assert key(ball, 30)["path_controls"] == []

# --- A key added past the key (an overshoot) or back before the start leaves every dot where it was -----------------
for speeds, frame in (([1.6] * 14 + [-0.9] * 3 + [0.3] * 3, 25), ([-0.5] * 4 + [1.0] * 16, 12)):
    for controls in (False, True):
        start()
        ball = mover({10: (0.0, 0.0), 30: (1.0, 0.0)})
        doc.frame_current = 20
        if controls:
            assert ops.anim.motion_path_handle(frame="10", side="OUT", x="0.3", y="0.4", layer=str(ball.id)) == {'FINISHED'}
            assert ops.anim.motion_path_control_add(frame="20", layer=str(ball.id)) == {'FINISHED'}
        ball.keyframe_set("position", 30, speeds=speeds)
        doc.frame_current = 20
        u = progress(ball, 10)[frame - 10]
        assert u > 1.0 or u < 0.0, ("off the curve", u)
        was = dots(ball)
        assert ops.anim.motion_path_key_add(frame=str(frame), type="KEYFRAME", layer=str(ball.id)) == {'FINISHED'}
        assert frame in [k["frame"] for k in ball.keyframes("position")]
        moved = max(math.dist(was[f], dots(ball)[f]) for f in range(10, 31))
        assert moved < 2e-3, ("no dot moves", frame, controls, moved)

# --- G on a controller's knob past its box: as far as the edge -------------------------------------------------------
assert ops.wm.read_homefile() == {'FINISHED'}
doc.auto_key = True
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
assert ops.armature.bone_add(name="eye", head_x="0", head_y="0", tail_x="0.1", tail_y="0") == {'FINISHED'}
ops.layer.new()
pupil = layers.active
ops.layer.move_to_group(group=str(rig.id))
pupil.add_curve([(0.0, 0.0), (0.2, 0.0)])
assert ops.layer.parent_bone_set(bone="eye") == {'FINISHED'}
assert ops.anim.action_new(name="eye_r") == {'FINISHED'}
doc.frame_current = 40
pupil.set_point(1, (0.3, 0.0))
doc.frame_current = 0
doc.action = ""
layers.active = rig
ops.armature.select_none()
rig.select_bone("eye")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="eye_r") == {'FINISHED'}
eye = next(b for b in rig.bones if b["name"] == "eye")
assert eye["contain_knob"]
size = eye["controller_frame_size"]
for frame, x in ((10, 10.0), (30, 30.0)):
    doc.frame_current = frame
    rig.bone_set("eye", knob=(x, 0.0))
doc.frame_current = 31
doc.auto_key = False
doc.motion_paths = True
assert ops.wm.tool_motion_path() == {'FINISHED'}
doc.frame_current = 20
ops.armature.select_none()
rig.select_bone("eye")
assert ops.anim.motion_path_select(frame="30") == {'FINISHED'}
assert ops.anim.motion_path_keys_transform(kind="G", x=str(25 * size), y="0") == {'FINISHED'}, (
    "past the box's edge (40): as far as it goes")
knob = next(k for k in rig.keyframes('bones["eye"].position') if k["frame"] == 30)["value"]
assert close(knob, (40.0, 0.0), 0.05), ("the key at the edge, not past it", knob)


# --- A Grab on a target its position keys don't move: refused, its path handles left alone ---------------------------
doc.auto_key = True
doc.frame_current = 45
rig.bone_set("eye", knob=(5.0, 0.0))
doc.auto_key = False
CH = 'bones["eye"].position'
for l in layers.all():
    l.selected = l.id == rig.id
layers.active = rig
ops.armature.select_none()
rig.select_bone("eye")
doc.frame_current = 22
was = [(k["frame"], k["value"], k["path_in"], k["path_out"]) for k in rig.keyframes(CH)]
assert any(math.hypot(*k[3]) > 1e-3 for k in was), was
for extra in ({}, {"head": "1"}):
    assert ops.anim.motion_path_handle(frame="30", side="IN", x="0", y="0", layer=str(rig.id), bone="eye",
                                       **extra) == {'CANCELLED'}, "its handles can't be dragged"
    assert ops.anim.motion_path_stroke(brush="GRAB", points="0,0;0.01,0.01", radius="0.2", strength="1",
                                       layer=str(rig.id), bone="eye", **extra) == {'CANCELLED'}
    now = [(k["frame"], k["value"], k["path_in"], k["path_out"]) for k in rig.keyframes(CH)]
    assert now == was, ("the Grab changed keys it can't move", was, now)

print("test_motion_path_pupil: all passed")
