"""X mirror editing between sided layers: with X on, a point edit in Ear_L moves its image in Ear_R
(paired in the setup pose, across the document's vertical axis), handles too, instead of mirroring
inside Ear_L. A layer without a partner still mirrors inside itself. Away from frame 0 the partner's
pose is keyed. Off, nothing follows.

Run with:  OpenAnimate --background --python tests/test_layer_mirror.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm
doc.frame_current = 0
doc.auto_key = True


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def xy(l, i):
    return tuple(l.points[i][:2])


# Ear_R is placed by its layer (moved 0.2 left), so the pair only mirrors across the document's axis.
LEFT = [(0.5, 0.0), (0.8, 0.4), (1.0, 0.1)]
ops.layer.new()
ear_r = layers.active
ear_r.name = "Ear_R"
ear_r.location_x = -0.2
ear_r.add_curve([(-x + 0.2, y) for x, y in LEFT])
ops.layer.new()
ear_l = layers.active
ear_l.name = "Ear_L"
ear_l.add_curve(LEFT)
r_before = [xy(ear_r, i) for i in range(3)]
# An undo step holding both drawings, for the undos below to land on.
ear_l.select_points(range(3))
assert ops.mesh.translate(move_x="0") == {'FINISHED'}
wm.set_mirror_editing(x=True)

# Moving Ear_L's middle point moves Ear_R's, reflected (layer space: Ear_R's points sit 0.2 right).
ear_l.select_points(range(3), False)
ear_l.select_points([1])
assert ops.mesh.translate(move_x="0.1", move_y="0.05") == {'FINISHED'}
assert close(xy(ear_l, 1), (0.9, 0.45))
assert close(xy(ear_r, 1), (-0.9 + 0.2, 0.45)), xy(ear_r, 1)
assert close(xy(ear_r, 0), r_before[0]) and close(xy(ear_r, 2), r_before[2]), "only the image moves"
assert close(xy(ear_l, 0), LEFT[0]) and close(xy(ear_l, 2), LEFT[2]), "no mirroring inside Ear_L"
# The handles mirror too: x flipped, the same way along the curve.
hl, hr = ear_l.curve_handles(0)[1], ear_r.curve_handles(0)[1]
for side in (1, 2):
    l_off = (hl[side][0] - xy(ear_l, 1)[0], hl[side][1] - xy(ear_l, 1)[1])
    r_off = (hr[side][0] - xy(ear_r, 1)[0], hr[side][1] - xy(ear_r, 1)[1])
    assert close(r_off, (-l_off[0], l_off[1])), (side, l_off, r_off)
ops.ed.undo()
assert close(xy(ear_r, 1), r_before[1])

# The other way round: Ear_R's edits move Ear_L.
layers.active = ear_r
ear_r.select_points(range(3), False)
ear_r.select_points([2])
assert ops.mesh.translate(move_x="-0.1") == {'FINISHED'}
assert close(xy(ear_l, 2), (1.1, 0.1)), xy(ear_l, 2)
ops.ed.undo()

# Slide and Smooth Points follow too.
layers.active = ear_l
ear_l.select_points(range(3), False)
ear_l.select_points([1])
assert ops.mesh.smooth_points(factor="1") == {'FINISHED'}
assert close(xy(ear_r, 1), (-xy(ear_l, 1)[0] + 0.2, xy(ear_l, 1)[1])), (xy(ear_l, 1), xy(ear_r, 1))
ops.ed.undo()
assert ops.mesh.slide(factor="0.5") == {'FINISHED'}
assert close(xy(ear_r, 1), (-xy(ear_l, 1)[0] + 0.2, xy(ear_l, 1)[1])), (xy(ear_l, 1), xy(ear_r, 1))
ops.ed.undo()

# Away from frame 0: the partner's pose is keyed there, frame 0 left as it was.
doc.frame_current = 10
assert ops.mesh.translate(move_y="0.2") == {'FINISHED'}
assert close(xy(ear_r, 1), (r_before[1][0], r_before[1][1] + 0.2))
doc.frame_current = 0
assert close(xy(ear_r, 1), r_before[1])
doc.frame_current = 10
assert close(xy(ear_r, 1), (r_before[1][0], r_before[1][1] + 0.2)), "keyed"
doc.frame_current = 0

# Off: the partner stays.
wm.set_mirror_editing(x=False)
assert ops.mesh.translate(move_x="0.1") == {'FINISHED'}
assert close(xy(ear_r, 1), r_before[1])
ops.ed.undo()
wm.set_mirror_editing(x=True)

# A layer with no side still mirrors inside itself.
ops.layer.new()
head = layers.active
head.name = "Head"
head.add_curve([(-0.5, 0.0), (0.0, 0.6), (0.5, 0.0)])
head.select_points(range(3), False)
head.select_points([2])
assert ops.mesh.translate(move_x="0.1") == {'FINISHED'}
assert close(xy(head, 0), (-0.6, 0.0)), xy(head, 0)

# Symmetrize across the pair: a lopsided Ear_R is made Ear_L's mirror image (Right to Left copies the
# +x side, Ear_L), whichever way its points run; Left to Right copies Ear_R back onto Ear_L.
ops.layer.new()
eye_r = layers.active
eye_r.name = "Eye_R"
eye_r.add_curve([(-1.1, 0.05), (-0.75, 0.5), (-0.45, 0.0)])  # runs the other way, a little off
ops.layer.new()
eye_l = layers.active
eye_l.name = "Eye_L"
eye_l.add_curve([(0.5, 0.0), (0.8, 0.4), (1.0, 0.1)])
eye_l.set_shape_style(0, line=(0.0, 0.5, 1.0, 1.0), line_width=0.025)
eye_l.select_points(range(3))
assert ops.mesh.symmetrize(direction="RIGHT_TO_LEFT") == {'FINISHED'}
assert close(eye_r.shape_style(0)["line"], (0.0, 0.5, 1.0, 1.0)), eye_r.shape_style(0)
assert abs(eye_r.shape_style(0)["line_width"] - 0.025) < 1e-6, "the line's look comes across too"
for i, j in ((0, 2), (1, 1), (2, 0)):
    assert close(xy(eye_r, j), (-xy(eye_l, i)[0], xy(eye_l, i)[1])), (i, j, xy(eye_l, i), xy(eye_r, j))
assert close(xy(eye_l, 1), (0.8, 0.4)), "the source side stays"
hl, hr = eye_l.curve_handles(0)[1], eye_r.curve_handles(0)[1]
l_out = (hl[2][0] - xy(eye_l, 1)[0], hl[2][1] - xy(eye_l, 1)[1])
r_in = (hr[1][0] - xy(eye_r, 1)[0], hr[1][1] - xy(eye_r, 1)[1])
assert close(r_in, (-l_out[0], l_out[1]), 1e-3), (l_out, r_in)

# Nudge Eye_R off (mirror editing off, so Eye_L stays), then copy it back.
wm.set_mirror_editing(x=False)
layers.active = eye_r
eye_r.select_points(range(3))
assert ops.mesh.translate(move_y="0.2") == {'FINISHED'}
layers.active = eye_l
eye_l.select_points(range(3))
rt = [xy(eye_r, j) for j in range(3)]
assert ops.mesh.symmetrize(direction="LEFT_TO_RIGHT") == {'FINISHED'}
for i, j in ((0, 2), (1, 1), (2, 0)):
    assert close(xy(eye_l, i), (-rt[j][0], rt[j][1])), (i, j)

# Uneven: more selected than the partner has points: nothing changes, the status says so.
ops.layer.new()
lip_r = layers.active
lip_r.name = "Lip_R"
lip_r.add_curve([(-0.5, -1.0), (-1.0, -1.0)])
ops.layer.new()
lip_l = layers.active
lip_l.name = "Lip_L"
lip_l.add_curve([(0.5, -1.0), (0.75, -1.1), (1.0, -1.0)])
lip_l.select_points(range(3))
before = [xy(lip_r, j) for j in range(2)]
assert ops.mesh.symmetrize() == {'CANCELLED'}
assert [xy(lip_r, j) for j in range(2)] == before

print("layer_mirror: ok")
