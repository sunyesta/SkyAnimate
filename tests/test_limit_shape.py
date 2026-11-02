"""Limit to Shape (a bone constraint): the bone's head, or the whole drawing it carries, stays inside a
closed shape of another layer as that shape is drawn on the frame: a pupil inside its eye white, however
the head turns. Overlap lets it go part way out, Softness eases it to a stop; saved with the file.

Run with:  OpenAnimate --background --python tests/test_limit_shape.py
"""

import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="head", head_x="0", head_y="0", tail_x="0", tail_y="2", parent="")
ops.armature.bone_add(name="pupil", head_x="0.5", head_y="1", tail_x="0.5", tail_y="1.05", parent="head")


def drawing(points, bone):
    ops.layer.new()
    l = layers.active
    assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
    l = next(x for x in layers.all() if x.id == l.id)
    l.add_curve(points, closed=True, fill=True)
    layers.active = l
    assert ops.layer.parent_bone_set(bone=bone) == {'FINISHED'}
    return next(x for x in layers.all() if x.id == l.id)


ring = lambda cx, cy, rx, ry, n=16: [(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n))
                                     for k in range(n)]
white = drawing(ring(0.5, 1.0, 0.3, 0.15), "head")
pupil = drawing(ring(0.5, 1.0, 0.05, 0.05), "pupil")

layers.active = rig
rig.select_bone("pupil")
assert ops.armature.constraint_add(type="LIMIT_SHAPE", bone="pupil") == {'FINISHED'}
c = rig.bone_constraints("pupil")[0]
assert c["type"] == "LIMIT_SHAPE" and c["name"] == "Limit to Shape" and c["fit"] == "WHOLE", c
assert c["affect_transform"], "Affect Transform is on by default"
# Off for these checks: where it shows is held, the bone's own position goes on past.
rig.bone_constraint_set("pupil", 0, shape_layer=white.name, affect_transform=False)


def near_edge(poly, p, tolerance=0.004):
    """Within `tolerance` of the polygon's edges: the shape's smooth curve bulges past these chords."""
    for i in range(len(poly)):
        (ax, ay), (bx, by) = poly[i - 1], poly[i]
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / (dx * dx + dy * dy)))
        if math.dist(p, (ax + dx * t, ay + dy * t)) < tolerance:
            return True
    return False


def inside(poly, p):
    return inside_chords(poly, p) or near_edge(poly, p)


def inside_chords(poly, p):
    hit = False
    for i in range(len(poly)):
        (ax, ay), (bx, by) = poly[i], poly[i - 1]
        if (ay > p[1]) != (by > p[1]) and p[0] < (bx - ax) * (p[1] - ay) / (by - ay) + ax:
            hit = not hit
    return hit


def outline(layer):
    """The shape's points as shown (its curve is smooth through them: inside these is inside it)."""
    return layer.shown()[0]


def head():
    return next(b for b in rig.bones if b["name"] == "pupil")["head"]


# Pushed far out on a posed frame: the whole pupil stays inside the eye white.
doc.frame_current = 10
rig.bone_set("pupil", position=(1.0, -1.0))  # in the head bone's space: up the head and to the right
doc.frame_current = 11
doc.frame_current = 10
assert rig.bone_constraints("pupil")[0]["shape_region"] > 3, "the eye white's outline is read"
eye = outline(white)
assert all(inside(eye, p) for p in outline(pupil)), ("the pupil fits inside", outline(pupil))
right = head()[0]
assert right > 0.65, ("it goes as far as it fits", right)

# On the setup frame, nothing holds it (constraints never act there).
doc.frame_current = 0
assert abs(head()[0] - 0.5) < 1e-5

# Center: only its head has to stay in, so it goes further.
doc.frame_current = 10
rig.bone_constraint_set("pupil", 0, fit="CENTER")
doc.frame_current = 10
assert inside(outline(white), head()) and head()[0] > right + 0.02, ("the head alone fits", head()[0], right)
# Whole with Overlap 100% is the same as Center.
rig.bone_constraint_set("pupil", 0, fit="WHOLE", overlap=1.0)
doc.frame_current = 10
assert abs(head()[0] - right) > 0.02 and inside(outline(white), head())
rig.bone_constraint_set("pupil", 0, overlap=0.0)

# Any angle: the head turns, the eye white with it; the pupil still fits.
rig.bone_set("head", angle=0.8)
doc.frame_current = 11
doc.frame_current = 10
eye = outline(white)
assert all(inside(eye, p) for p in outline(pupil)), "turned, it still fits"

# Inside the shape the pupil moves freely.
rig.bone_set("pupil", position=(1.02, -0.55))  # a little up and right of its rest
doc.frame_current = 11
doc.frame_current = 10
moved = head()
rig.bone_constraint_set("pupil", 0, enabled=False)
doc.frame_current = 10
assert math.dist(moved, head()) < 1e-5, "inside, it isn't touched"
rig.bone_constraint_set("pupil", 0, enabled=True)

# Softness: it eases to a stop short of the hard limit and never quite reaches it.
rig.bone_set("pupil", position=(1.0, -1.0))
doc.frame_current = 11
doc.frame_current = 10
hard = head()
rig.bone_constraint_set("pupil", 0, softness=0.03)
doc.frame_current = 10
soft = head()
eye = outline(white)
assert all(inside(eye, p) for p in outline(pupil)), "soft, it still fits"
assert math.dist(soft, hard) < 0.03 + 1e-3, (soft, hard)

# Affect Transform: the bone's own position stops at the edge too, so its key holds where it shows.
rig.bone_constraint_set("pupil", 0, softness=0.0, affect_transform=True)
doc.auto_key = True
doc.frame_current = 20
rig.bone_set("pupil", position=(1.0, -1.2))  # further out than its key holds: a move
doc.frame_current = 21
doc.frame_current = 20
held = head()
rig.bone_constraint_set("pupil", 0, enabled=False)
doc.frame_current = 20
assert math.dist(head(), held) < 1e-4, ("its data stopped there too", head(), held)
rig.bone_constraint_set("pupil", 0, enabled=True)
# Off, the key holds the move past the edge (only where it shows is held).
rig.bone_constraint_set("pupil", 0, affect_transform=False)
doc.frame_current = 30
rig.bone_set("pupil", position=(1.0, -1.2))
doc.frame_current = 31
rig.bone_constraint_set("pupil", 0, enabled=False)
doc.frame_current = 30
assert head()[0] > 0.95, ("off: the data goes on past", head())
rig.bone_constraint_set("pupil", 0, enabled=True, softness=0.03)

# Saved with the file.
path = str(pathlib.Path(tempfile.mkdtemp()) / "limit_shape.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.type == "BONE")
c = rig.bone_constraints("pupil")[0]
assert c["shape_layer"] == white.id and c["fit"] == "WHOLE" and abs(c["softness"] - 0.03) < 1e-6, c
doc.frame_current = 10
assert math.dist(head(), soft) < 1e-4, "it holds the same after opening"

# A smart action moving the pupil's parent (a head turn moving the eye): Affect Transform reads the parent where it shows,
# with the shape, so a move lands where it's asked for. The eye comes first in the bone layer's list, as on a real rig.
doc.frame_current = 0
assert ops.layer.new_bone() == {'FINISHED'}
rig2 = layers.active
rig2.name = "SmartRig"
ops.armature.bone_add(name="turn", head_x="3", head_y="0", tail_x="3.5", tail_y="0", parent="")
ops.armature.bone_add(name="eye", head_x="0", head_y="0", tail_x="0", tail_y="0.4", parent="")
ops.armature.bone_add(name="iris", head_x="0", head_y="0", tail_x="0", tail_y="0.05", parent="eye")
rig2.select_bone("turn")
ops.layer.new()
eye_white = layers.active
eye_white.name = "SmartWhite"
assert ops.layer.move_to_group(group=str(rig2.id)) == {'FINISHED'}
eye_white = next(x for x in layers.all() if x.name == "SmartWhite")
eye_white.add_curve(ring(0.0, 0.0, 0.5, 0.5, 48), closed=True, fill=True)
layers.active = eye_white
assert ops.layer.parent_bone_set(bone="eye") == {'FINISHED'}
layers.active = rig2
ops.armature.select_none()
rig2.select_bone("iris")
assert ops.armature.constraint_add(type="LIMIT_SHAPE", bone="iris") == {'FINISHED'}
rig2.bone_constraint_set("iris", 0, shape_layer=eye_white.name, fit="CENTER", affect_transform=True)
ops.armature.select_none()
rig2.select_bone("turn")
assert ops.anim.action_new(name="turn", smart="1") == {'FINISHED'}
doc.frame_current = 10
rig2.bone_set("turn", angle=1.0)
rig2.bone_set("eye", position=(0.0, 2.0))  # the eye goes up by 2
doc.frame_current = 11
assert ops.anim.action_set(name="") == {'FINISHED'}
doc.frame_current = 5
rig2.bone_set("turn", angle=1.0)
doc.frame_current = 6
doc.frame_current = 5


def iris_head():
    return rig2.to_document(rig2.bone_pose("iris")[:2])


def nudged(x, y):
    layers.active = rig2
    ops.armature.select_none()
    rig2.select_bone("iris")
    was = iris_head()
    assert ops.armature.translate(move_x=str(x), move_y=str(y), only_bones="0") == {'FINISHED'}
    doc.frame_current = 6
    doc.frame_current = 5
    now = iris_head()
    assert ops.ed.undo() == {'FINISHED'}
    doc.frame_current = 6
    doc.frame_current = 5
    return was, now


was, _ = nudged(0.0, 0.0)
assert abs(was[1] - 2.0) < 1e-3, ("the smart action has the eye up there", was)
for move, wanted in (((0.1, 0.0), (0.1, 2.0)), ((0.0, -0.1), (0.0, 1.9)), ((-0.1, 0.1), (-0.1, 2.1))):
    _, now = nudged(*move)
    assert math.dist(now, wanted) < 1e-3, ("inside the shape it goes where it's put", move, now)
for move, wanted in (((3.0, 0.0), (0.5, 2.0)), ((-3.0, 0.0), (-0.5, 2.0)), ((0.0, 3.0), (0.0, 2.5)), ((0.0, -3.0), (0.0, 1.5))):
    _, now = nudged(*move)
    assert math.dist(now, wanted) < 3e-3, ("past the edge it stops at the side it went to", move, now)

print("limit to shape: ok")
