"""Smooth Points (right-click on points): the selected points go part of the way to their neighbours'
middle along their curves, their handles toward smooth ones; open ends and unselected points stay;
Keep Shape stops a ring shrinking; Position, Handles, Radius, Repeat and Smooth Endpoints; away from
frame 0 it keys the smoothed pose.

Run with:  OpenAnimate --background --python tests/test_smooth_points.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm
doc.frame_current = 0
doc.auto_key = True


def close(a, b, eps=1e-5):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def pts(l):
    return [p[:2] for p in l.points]


def make_zigzag():
    ops.layer.new()
    l = layers.active
    l.add_curve([(i * 0.2, 0.2 if i % 2 else -0.2) for i in range(7)])
    l.select_points(range(l.point_count))
    # An undo step, so the panel's redo (which undoes the smoothing first) lands here.
    assert ops.mesh.translate(move_x="0", move_y="0") == {'FINISHED'}
    return l


def jag(l):
    """How far the inner points stand off the line through their neighbours."""
    p = pts(l)
    return sum(abs(p[i][1] - (p[i - 1][1] + p[i + 1][1]) / 2) for i in range(1, len(p) - 1))


# A zigzag smooths flatter; its ends stay.
zig = make_zigzag()
before = pts(zig)
assert ops.mesh.smooth_points() == {'FINISHED'}
after = pts(zig)
assert jag(zig) < 0.6 * sum(abs(before[i][1] - (before[i - 1][1] + before[i + 1][1]) / 2) for i in range(1, 6))
assert close(after[0], before[0]) and close(after[6], before[6]), "an open curve's ends stay"
# Point 1 went a quarter of the way (half its Smoothing, 0.5) to its neighbours' middle: 0.2 toward -0.2.
assert close(after[1], (0.2, 0.1)), after[1]
# Its handles lie along the way from point 0 to point 2, a third of each side long.
h = zig.curve_handles(0)[1]
d = (after[2][0] - after[0][0], after[2][1] - after[0][1])
out = (h[2][0] - after[1][0], h[2][1] - after[1][1])
inn = (h[1][0] - after[1][0], h[1][1] - after[1][1])
# Smoothing 0.5 turns them half way: the out handle leans toward that way, and they're longer than none.
cos_out = (out[0] * d[0] + out[1] * d[1]) / (math.hypot(*out) * math.hypot(*d))
assert cos_out > 0.7 and math.hypot(*out) > 0.01 and math.hypot(*inn) > 0.01, (h, cos_out)
last = wm.last_operation()
assert last and last[0] == "mesh.smooth_points", last

# The panel: more smoothing, more times, flatter still; Position off moves no point.
was = jag(zig)
assert wm.redo_last(factor="1", repeat="5") == "FINISHED"
assert jag(zig) < was
assert wm.redo_last(position="0") == "FINISHED"
assert close(pts(zig)[1], before[1]) and close(pts(zig)[3], before[3]), "Position off: the points stay"
# Smooth Endpoints: the ends move too.
assert wm.redo_last(position="1", ends="1") == "FINISHED"
assert not close(pts(zig)[0], before[0])

# Only the selected points move: an unselected one holds.
zig2 = make_zigzag()
before = pts(zig2)
zig2.select_points(range(zig2.point_count), False)
zig2.select_points([2, 3])
assert ops.mesh.smooth_points(factor="1") == {'FINISHED'}
now = pts(zig2)
assert all(close(now[i], before[i]) for i in (0, 1, 4, 5, 6))
assert not close(now[2], before[2]) and not close(now[3], before[3])

# A ring of 8: plain smoothing shrinks it; Keep Shape holds its size.
def ring_size(keep):
    ops.layer.new()
    l = layers.active
    l.add_curve([(math.cos(i * math.pi / 4) + (0.08 if i % 2 else 0.0) * math.cos(i * math.pi / 4),
                  math.sin(i * math.pi / 4)) for i in range(8)], closed=True)
    l.select_points(range(l.point_count))
    assert ops.mesh.smooth_points(factor="0.5", repeat="10", keep_shape="1" if keep else "0") == {'FINISHED'}
    return sum(math.hypot(x, y) for x, y in pts(l)) / l.point_count


plain, kept = ring_size(False), ring_size(True)
assert plain < 0.95, plain
assert kept > plain + 0.03 and kept > 0.97, (plain, kept)

# Radius: off by default; on, a thick point between thin ones thins.
ops.layer.new()
line = layers.active
line.add_curve([(0, 0), (0.5, 0), (1, 0)])
line.select_points([1])
assert ops.mesh.radius_scale(factor="3") == {'FINISHED'}
r = line.radii[1]
line.select_points(range(3))
assert ops.mesh.smooth_points() == {'FINISHED'}
assert close([line.radii[1]], [r])
assert ops.mesh.smooth_points(radius="1") == {'FINISHED'}
assert line.radii[1] < r, line.radii

# Away from frame 0 it poses: frame 0 keeps its zigzag.
zig3 = make_zigzag()
setup = pts(zig3)
doc.frame_current = 10
assert ops.mesh.smooth_points() == {'FINISHED'}
posed = pts(zig3)
assert not close(posed[1], setup[1])
doc.frame_current = 0
assert close(pts(zig3)[1], setup[1])
doc.frame_current = 10
assert close(pts(zig3)[1], posed[1]), "the smoothed pose is keyed"

# Posed by a bone: it smooths what shows (the turned drawing), point 1 a quarter of the way to its
# shown neighbours' middle.
doc.frame_current = 0
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="arm", head_x="-1", head_y="0", tail_x="0", tail_y="0", parent="")
bent = make_zigzag()
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
bent = next(l for l in layers.all() if l.id == bent.id)
doc.frame_current = 10
rig.bone_set("arm", angle=0.6)
layers.active = rig
ops.armature.select_all()
ops.anim.keyframe_insert()
layers.active = bent
bent.select_points(range(bent.point_count))
shown = bent.evaluated()[0]
assert ops.mesh.smooth_points() == {'FINISHED'}
now = bent.evaluated()[0]
mid = ((shown[0][0] + shown[2][0]) / 2, (shown[0][1] + shown[2][1]) / 2)
want = (shown[1][0] + (mid[0] - shown[1][0]) * 0.25, shown[1][1] + (mid[1] - shown[1][1]) * 0.25)
assert close(now[1], want, 1e-4), (now[1], want)
assert close(now[0], shown[0], 1e-4), "its end stays where it shows"

# Nothing to smooth: just an open curve's end.
doc.frame_current = 0
layers.active = zig3
bent.select_points(range(bent.point_count), False)
zig3.select_points(range(zig3.point_count), False)
zig3.select_points([0])
assert ops.mesh.smooth_points() == {'CANCELLED'}

print("smooth_points: ok")
