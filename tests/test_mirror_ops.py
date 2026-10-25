"""X mirror editing for operators on the selection (OPTYPE_MIRROR): with X on, Separate, Delete,
Duplicate and the others act on the selected points' mirror images too: in the same layer across its
axis, or in a sided layer's partner (Ear_R for Ear_L), which is edited along for the run and then
left as it was picked. Both a point and its image selected: the -x one follows, reflected. Off,
only the selection.

Run with:  OpenAnimate --background --python tests/test_mirror_ops.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm
doc.frame_current = 0


def close(a, b, eps=1e-5):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def names():
    return [l.name for l in layers.all()]


def box(l, x0, x1, y0=0.0, y1=1.0):
    return l.add_curve([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], closed=True, fill=True)


# One layer with two eyes either side of its axis.
face = layers.active
face.name = "Face"
box(face, 0.5, 1.0)     # points 0-3, the right eye
box(face, -1.0, -0.5)   # points 4-7, the left eye
face.select_points(range(8))
assert ops.mesh.translate(move_x="0") == {'FINISHED'}  # an undo step to come back to
wm.set_mirror_editing(x=True)

# Separate the right eye: the left one comes along, into the same new layer.
ops.mesh.select_none()
face.select_points([0, 1, 2, 3])
assert ops.mesh.separate() == {'FINISHED'}
new = next(l for l in layers.all() if l.name == "Face 1")
assert new.point_count == 8 and new.shape_count == 2, (new.point_count, new.shape_count)
assert face.point_count == 0
ops.ed.undo()
assert face.point_count == 8 and "Face 1" not in names()

# The selection comes back as it was after a run (Bring Forward keeps it).
ops.mesh.select_none()
face.select_points([0, 1, 2, 3])
ops.mesh.shape_raise()
assert sorted(i for i, p in enumerate(face.points) if p[2]) == [0, 1, 2, 3], "the selection as it was"

# Delete one corner: its image goes too.
ops.mesh.select_none()
face.select_points([1])
assert ops.mesh.delete_points() == {'FINISHED'}
assert face.point_count == 6, face.point_count
ops.ed.undo()

# Duplicate, then move right: the right copy goes right, the left copy left (it follows, reflected).
ops.mesh.select_none()
face.select_points([0, 1, 2, 3])
assert ops.mesh.duplicate_move() == {'FINISHED'}
assert face.point_count == 16
copies = [i for i, p in enumerate(face.points) if p[2]]
assert len(copies) == 8, copies
assert ops.mesh.translate(move_x="0.25") == {'FINISHED'}
xs = sorted(face.points[i][0] for i in range(8, 16))
assert close(xs, [-1.25, -1.25, -0.75, -0.75, 0.75, 0.75, 1.25, 1.25]), xs
ops.ed.undo()
ops.ed.undo()

# Off: only the selection.
wm.set_mirror_editing(x=False)
ops.mesh.select_none()
face.select_points([1])
assert ops.mesh.delete_points() == {'FINISHED'}
assert face.point_count == 7
ops.ed.undo()
wm.set_mirror_editing(x=True)

# Sided layers: separating from Ear_L separates the matching piece of Ear_R too, into Ear_R 1. (Made
# with X off and settled by an undo step, so X doesn't take them for new drawing.)
wm.set_mirror_editing(x=False)
ops.layer.new()
ear_r = layers.active
ear_r.name = "Ear_R"
box(ear_r, -2.0, -1.5)
box(ear_r, -3.0, -2.5)
ops.layer.new()
ear_l = layers.active
ear_l.name = "Ear_L"
box(ear_l, 1.5, 2.0)
box(ear_l, 2.5, 3.0)
ear_l.select_points(range(8))
assert ops.mesh.translate(move_x="0") == {'FINISHED'}
wm.set_mirror_editing(x=True)
ops.mesh.select_none()
ear_l.select_points([0, 1, 2, 3])
assert ops.mesh.separate() == {'FINISHED'}
assert "Ear_L 1" in names() and "Ear_R 1" in names(), names()
ear_r1 = next(l for l in layers.all() if l.name == "Ear_R 1")
assert sorted(p[0] for p in ear_r1.points) == [-2.0, -2.0, -1.5, -1.5]
assert ear_r.point_count == 4 and ear_l.point_count == 4
assert not ear_r.selected, "Ear_R is left unpicked, as it was"
assert layers.active.name == "Ear_L"

# Delete in Ear_L takes Ear_R's image too.
ops.mesh.select_none()
ear_l.select_points([0])
assert ops.mesh.delete_points() == {'FINISHED'}
assert ear_r.point_count == 3, ear_r.point_count

# Looks: a shape's colour changed (then an undo step, as the Properties panel makes) goes to its
# mirror shape too: the other eye in the same layer, Ear_R's matching shape for Ear_L's.
def push():
    """An undo step, as the Properties panel or a tool makes after an edit."""
    assert ops.mesh.select_none() == {'FINISHED'}


def fill(l, i):
    return l.shape_style(i)["fill"]


layers.active = face
ops.mesh.select_none()
face.select_points([0])
face.set_shape_style(0, fill=(1.0, 0.0, 0.0, 1.0), line_width=0.02)
push()
assert close(fill(face, 1), (1.0, 0.0, 0.0, 1.0)), fill(face, 1)
assert abs(face.shape_style(1)["line_width"] - 0.02) < 1e-6
layers.active = ear_l
ops.mesh.select_none()
ear_l.select_points([0])
ear_l.set_shape_style(0, fill=(0.0, 0.0, 1.0, 1.0))
push()
assert close(fill(ear_r, 0), (0.0, 0.0, 1.0, 1.0)), fill(ear_r, 0)
# Off: only the shape changed.
wm.set_mirror_editing(x=False)
ear_l.set_shape_style(0, fill=(0.0, 1.0, 0.0, 1.0))
push()
assert close(fill(ear_r, 0), (0.0, 0.0, 1.0, 1.0)), fill(ear_r, 0)
wm.set_mirror_editing(x=True)

# New geometry, in a layer of its own (settled with X off first).
wm.set_mirror_editing(x=False)
ops.layer.new()
body = layers.active
body.name = "Body"
body.add_curve([(0.5, 0.0), (1.0, 0.0), (1.0, 0.5), (0.5, 0.5)], closed=True, fill=True)   # 0-3
body.add_curve([(-1.0, 0.0), (-0.5, 0.0), (-0.5, 0.5), (-1.0, 0.5)], closed=True, fill=True)  # 4-7
body.select_points(range(8))
assert ops.mesh.translate(move_x="0") == {'FINISHED'}
ops.mesh.select_none()
wm.set_mirror_editing(x=True)


def has_point(l, p):
    return any(close(q[:2], p) for q in l.points)


# A new stroke (drawn, then its undo step): its reflected copy is drawn too.
body.add_curve([(1.5, 0.0), (2.0, 0.3), (2.5, 0.0)])
push()
assert body.point_count == 14 and body.curve_count == 4, (body.point_count, body.curve_count)
assert has_point(body, (-2.0, 0.3)) and has_point(body, (-1.5, 0.0))
push()
assert body.point_count == 14, "never twice"

# The knife: a point cut into the right box's bottom cuts the left box's bottom at the mirror spot.
q = body.split_segment(0, 0, 0.25)
cut = body.points[q][:2]
push()
assert body.point_count == 16, body.point_count
assert has_point(body, (-cut[0], cut[1])), (cut, body.points)

# Closing an open line (F on its two ends) closes its mirror copy too, and both are filled.
ops.mesh.select_none()
body.select_points([8, 10])
assert ops.mesh.make_segment() == {'FINISHED'}
closed = [body.shape_outline(i)[1] for i in range(body.shape_count)]
assert closed.count(True) >= 4, closed

# Sided layers: a stroke drawn in Ear_L is drawn reflected in Ear_R.
layers.active = ear_l
before_r = ear_r.point_count
ear_l.add_curve([(4.0, 0.0), (4.5, 0.5)])
push()
assert ear_r.point_count == before_r + 2 and has_point(ear_r, (-4.5, 0.5)), ear_r.points

# Scale Radius (Alt S) widens the image too, in the same layer and across a pair.
layers.active = body
ops.mesh.select_none()
body.select_points([0])
image = next(i for i, p in enumerate(body.points) if close(p[:2], (-body.points[0][0], body.points[0][1])))
r0, ri = body.radii[0], body.radii[image]
assert ops.mesh.radius_scale(factor="2") == {'FINISHED'}
assert abs(body.radii[0] - r0 * 2) < 1e-5 and abs(body.radii[image] - ri * 2) < 1e-5, body.radii
layers.active = ear_l
ops.mesh.select_none()
ear_l.select_points([0])
rr = list(ear_r.radii)
assert ops.mesh.radius_scale(factor="0.5") == {'FINISHED'}
assert any(abs(a - b * 0.5) < 1e-5 for a, b in zip(ear_r.radii, rr)), (ear_r.radii, rr)

print("mirror_ops: ok")
