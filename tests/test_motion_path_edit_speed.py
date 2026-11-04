"""Motion paths: the Edit Speed brush. A dot between keys slides along its path, the keys staying on their frames (its
stretch made Custom); with proportional editing the dots around it slide too, by the falloff, never past the keys or
each other. Ctrl click evens a stretch out.

Run with:  OpenAnimate --background --python tests/test_motion_path_edit_speed.py
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


def key(layer, channel, frame):
    return next(k for k in layer.keyframes(channel) if k["frame"] == frame)


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
    if interp:
        # An undo step with the interpolation set (set from Python, it makes none).
        f, (x, y) = list(keys.items())[-1]
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(l.id)) == {'FINISHED'}
    return l


doc.motion_paths = True
doc.path_before, doc.path_after = 3, 3
doc.auto_key = False

# --- Picking it, and its settings --------------------------------------------------------------------------------
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert ops.anim.motion_path_brush_set(brush="EDIT_SPEED", proportional="0", proportional_size="3",
                                      falloff="LINEAR") == {'FINISHED'}
s = oanim.wm.path_brush_settings()
assert s["brush"] == "EDIT_SPEED" and not s["proportional"] and close(s["proportional_size"], 3.0), s
assert s["falloff"] == "LINEAR", s
assert ops.anim.motion_path_stroke(points="0,0") == {'CANCELLED'}, "not a circle brush"

# --- A straight, even stretch: 0.1 a frame from (0, 0) on 10 to (1, 0) on 20 ----------------------------------------
# O turns proportional editing on with Edit Speed, though the layer has no points to move.
ops.layer.new()
empty = layers.active
for other in layers:
    other.selected = other.id == empty.id
assert oanim.wm.event_simulate("O", area="Viewport")
assert oanim.wm.path_brush_settings()["proportional"]
assert oanim.wm.event_simulate("O", area="Viewport")
assert not oanim.wm.path_brush_settings()["proportional"]

line = fresh_layer({10: (0.0, 0.0), 20: (1.0, 0.0)}, interp="LINEAR")

doc.frame_current = 12
args = dict(layer=str(line.id))
# Off the line: it goes to the nearest place on it.
assert ops.anim.motion_path_slide(frame="15", x="0.55", y="0.3", **args) == {'FINISHED'}
assert doc.frame_current == 12, "the frame shown stays"
k = key(line, "position", 20)
assert k["interp"] == "CUSTOM" and len(k["speeds"]) == 10, k
assert close(where(line, 15), (0.55, 0.0)), where(line, 15)
assert close(where(line, 14), (0.4, 0.0)) and close(where(line, 16), (0.6, 0.0)), "only that dot, without proportional"
assert close(where(line, 10), (0.0, 0.0)) and close(where(line, 20), (1.0, 0.0)), "the keys stay"
ops.ed.undo()
assert close(where(line, 15), (0.5, 0.0)), "one undo step"

# Proportional, Linear falloff over 3 frames: frame 14 goes 2/3 as far, 13 a third, 12 not at all.
assert ops.anim.motion_path_slide(frame="15", x="0.6", y="0", proportional="1", **args) == {'FINISHED'}
assert close(where(line, 15)[0], 0.6) and close(where(line, 16)[0], 0.6 + 0.1 * 2 / 3), where(line, 16)
assert close(where(line, 14)[0], 0.4 + 0.1 * 2 / 3) and close(where(line, 13)[0], 0.3 + 0.1 / 3), where(line, 13)
assert close(where(line, 12)[0], 0.2) and close(where(line, 18)[0], 0.8), "outside the reach, as it was"
ops.ed.undo()
# A bigger reach takes in more.
assert ops.anim.motion_path_slide(frame="15", x="0.6", y="0", proportional="1", size="5", **args) == {'FINISHED'}
assert where(line, 12)[0] > 0.2 + 1e-3, where(line, 12)
ops.ed.undo()

# Never past the next dot or the key: the dots ahead are pushed along, in order, all arriving.
assert ops.anim.motion_path_slide(frame="15", x="2", y="0", **args) == {'FINISHED'}
xs = [where(line, f)[0] for f in range(10, 21)]
assert all(b > a for a, b in zip(xs, xs[1:])), xs
assert xs[5] > 0.9 and close(xs[10], 1.0), xs
# Ctrl click: even again.
assert ops.anim.motion_path_even_spacing(frame="15", **args) == {'FINISHED'}
assert close([where(line, f)[0] for f in range(10, 21)], [i / 10 for i in range(11)]), "even"
assert ops.anim.motion_path_slide(frame="20", x="1", y="0", **args) == {'CANCELLED'}, "a key doesn't slide"

# --- S: the spacing around a dot scaled --------------------------------------------------------------------------
even = fresh_layer({10: (0.0, 0.0), 20: (1.0, 0.0)}, interp="LINEAR")
sargs = dict(layer=str(even.id))
# Half as far from frame 15, the dots within 3 frames alike (proportional off); 12, 3 away, stays.
assert ops.anim.motion_path_scale_spacing(frame="15", factor="0.5", proportional="0", size="3", **sargs) == {
    'FINISHED'}
xs = [where(even, f)[0] for f in range(10, 21)]
assert close(xs, [0.0, 0.1, 0.2, 0.4, 0.45, 0.5, 0.55, 0.6, 0.8, 0.9, 1.0]), xs
ops.ed.undo()
# Proportional, Linear: 14 (2/3 in) goes 2/3 of the way to 15, 13 (1/3) a third.
assert ops.anim.motion_path_scale_spacing(frame="15", factor="0", proportional="1", size="3", falloff="LINEAR",
                                          **sargs) == {'FINISHED'}
assert close(where(even, 14)[0], 0.5 - 0.1 / 3) and close(where(even, 13)[0], 0.5 - 0.2 * 2 / 3), where(even, 13)
assert close(where(even, 12)[0], 0.2) and close(where(even, 20)[0], 1.0)
ops.ed.undo()
assert ops.anim.motion_path_scale_spacing(frame="40", factor="0.5", **sargs) == {'CANCELLED'}, "no stretch there"

# On a key: the dots either side come in toward it, easing into and out of the pose.
eased = fresh_layer({10: (0.0, 0.0), 20: (1.0, 0.0), 30: (2.0, 0.0)}, interp="LINEAR")
assert ops.anim.motion_path_scale_spacing(frame="20", factor="0.5", proportional="0", size="3",
                                          layer=str(eased.id)) == {'FINISHED'}
xs = [round(where(eased, f)[0], 4) for f in range(16, 25)]
assert close(xs, [0.6, 0.7, 0.9, 0.95, 1.0, 1.05, 1.1, 1.3, 1.4]), xs
assert key(eased, "position", 20)["interp"] == "CUSTOM" and key(eased, "position", 30)["interp"] == "CUSTOM"

# S starts it in the viewport with Edit Speed (the keymap's), not with Edit.
assert oanim.wm.event_simulate("S", area="Viewport")

# --- A curve: the dot stays on it ---------------------------------------------------------------------------------
curve = fresh_layer({10: (0.0, 0.0), 20: (1.0, 1.0), 30: (2.0, 0.0)})
shape = {f: where(curve, f) for f in range(10, 21)}
target = shape[17]
assert ops.anim.motion_path_slide(frame="15", x=str(target[0]), y=str(target[1]), layer=str(curve.id)) == {
    'FINISHED'}
assert close(where(curve, 15), target, 5e-3), (where(curve, 15), target)
assert close(where(curve, 20), shape[20]) and close(where(curve, 10), shape[10])


# Every dot still on the curve it was on: near the polyline through the old dots.
def off_path(p):
    best = 1e9
    for f in range(10, 20):
        a, b = shape[f], shape[f + 1]
        ab = (b[0] - a[0], b[1] - a[1])
        t = max(0.0, min(1.0, ((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1]) / max(ab[0] ** 2 + ab[1] ** 2, 1e-12)))
        q = (a[0] + ab[0] * t, a[1] + ab[1] * t)
        best = min(best, math.hypot(p[0] - q[0], p[1] - q[1]))
    return best


assert all(off_path(where(curve, f)) < 0.01 for f in range(10, 21)), [off_path(where(curve, f)) for f in range(10, 21)]

# --- A bone's tip: its angle --------------------------------------------------------------------------------------
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
assert ops.anim.motion_path_slide(frame="15", x=str(math.cos(0.8)), y=str(math.sin(0.8)), layer=str(rig.id),
                                  bone="arm") == {'FINISHED'}
assert key(rig, 'bones["arm"].angle', 20)["interp"] == "CUSTOM"
doc.frame_current = 15
assert close(next(b for b in rig.bones if b["name"] == "arm")["angle"], 0.8), rig.bones
doc.frame_current = 20
assert close(next(b for b in rig.bones if b["name"] == "arm")["angle"], 1.0), "arrives on its frame"

print("Motion path Edit Speed tests passed")
