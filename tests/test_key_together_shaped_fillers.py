"""Key Together's fillers inside a stretch shaped on its motion path (Custom timing, control points): a folded layer's
other channel keyed inside the stretch gives it a filler there, which must move nothing, folded or opened again (the
filler let go). It once split the stretch and was then left out as it played, the stretch's second half playing over
all of it.

Run with:  OpenAnimate --background --python tests/test_key_together_shaped_fillers.py
"""

import oanim

ops, L = oanim.ops, oanim.data.layers


def fresh():
    assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
    d = oanim.data.document
    d.auto_key = True
    d.auto_freeze_keys = False
    ops.layer.new()
    a = L.active
    a.add_curve([(0.0, 0.0), (1.0, 0.0)])
    a.key_together = True
    return d, a


def places(d, a, last):
    out = []
    for f in range(last + 1):
        d.frame_current = f
        out.append((a.location_x, a.location_y, a.points[1][0], a.points[1][1]))
    return out


def worst(p, q):
    return max(max(abs(u - v) for u, v in zip(x, y)) for x, y in zip(p, q))


def unfold(d, a):
    assert ops.anim.key_together_set(layer=str(a.id), on="0") == {'FINISHED'}
    d.frame_current = 1
    d.frame_current = 0


# --- A Custom stretch (Ease In painted on the path): an opacity key inside it ----------------------------------------
d, a = fresh()
d.frame_current = 20
a.location_x = 4.0
d.frame_current = 10
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert ops.anim.motion_path_select(frame="10", layer=str(a.id)) == {'FINISHED'}
assert ops.anim.motion_path_stretch_shape(frame="10", layer=str(a.id), shape="EASE_IN") == {'FINISHED'}
before = places(d, a, 20)
d.frame_current = 10
a.opacity = 0.5
d.frame_current = 0
assert 10 in [k["frame"] for k in a.keyframes("position") if k.get("filler")], a.keyframes("position")
w = worst(before, places(d, a, 20))
assert w < 2e-4, ("a filler inside the Custom stretch moved the layer", w)
unfold(d, a)
w = worst(before, places(d, a, 20))
assert w < 2e-4, ("opened again (the filler let go), the stretch plays as it was", w)

# --- A stretch with a control point on its path: an opacity key inside it ---------------------------------------------
d, a = fresh()
d.frame_current = 20
a.location_x, a.location_y = 4.0, 1.0
d.frame_current = 30
a.location_x = 5.0
d.frame_current = 15
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert ops.anim.motion_path_select(frame="15", layer=str(a.id)) == {'FINISHED'}
assert ops.anim.motion_path_control_add(frame="10", layer=str(a.id)) == {'FINISHED'}
before = places(d, a, 30)
d.frame_current = 13
a.opacity = 0.5
w = worst(before, places(d, a, 30))
assert w < 2e-4, ("a filler inside the stretch with a control point moved the layer", w)
unfold(d, a)
w = worst(before, places(d, a, 30))
assert w < 2e-4, ("opened again, the stretch plays as it was", w)

print("key together shaped fillers: ok")
