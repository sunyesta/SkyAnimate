"""Ignore Bones and Keep Place on Duplicate Keys and Paste Keys. Ignore Bones: copied point keys are worked
out again so each point shows where it did on the frame it was copied from, whatever the bones do on the
new frame (off, the copies hold the same change as the originals and the bones move them). Keep Place:
only the shape is copied, its centre staying where the points' centre was on the new frame. Both are kept
for next time.

Run with:  OpenAnimate --background --python tests/test_keys_ignore_bones.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
assert oanim.ops.armature.bone_primitive_add() == {'FINISHED'}
bone = rig.bones[0]["name"]
assert oanim.ops.layer.new() == {'FINISHED'}
art = layers.active
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
art = next(l for l in layers.all() if l.id == art.id)
art.add_curve([(0.2, 0.1), (0.4, 0.3), (0.1, 0.5)])
assert oanim.ops.layer.parent_bone_set(bone=bone, deform="POINTS") == {'FINISHED'}
# The middle point's handles stored (set by hand); the end points' worked out from their neighbours.
art.place_handle(0, 1, "OUT", (0.5, 0.35))


def shown():
    return [tuple(p) for p in art.shown()[0]]


def handles():
    """The middle point's handles (stored: the bones turn them with the point)."""
    cp = art.shown()[1][0][0][1]
    return [(cp[1], cp[2])]


def make_active():
    layers.active = art
    for l in layers.all():
        l.select = l.id == art.id


# Frame 10: the bone at rest, a point pulled out (every point keyed). Frames 20 and 30: the bone turned.
doc.frame_current = 10
rig.bone_set(bone, angle=0.0)
make_active()
doc.auto_freeze_keys = True  # every point keyed, so the whole shape is copied
art.set_point(0, (0.3, 0.0))
doc.frame_current = 20
rig.bone_set(bone, angle=0.8)
doc.frame_current = 30
rig.bone_set(bone, angle=-0.5)
doc.frame_current = 31
doc.frame_current = 10
look = shown()
look_handles = handles()
assert not close(look[0], (0.2, 0.1)), "the point was pulled"


def select_point_keys(frame):
    make_active()
    assert oanim.ops.anim.keys_select(layers=str(art.id), frame_start=str(frame), frame_end=str(frame)) == {'FINISHED'}


# Off: the copy holds the same change, so the turned bone carries it somewhere else.
select_point_keys(10)
assert oanim.ops.anim.keys_duplicate(frames="10", ignore_bones="0") == {'FINISHED'}
doc.frame_current = 20
assert not close(shown()[0], look[0]), "the bone moves a plain copy"
oanim.ops.ed.undo()

# On: every point shows on frame 20 where it did on frame 10, handles too.
doc.frame_current = 10
select_point_keys(10)
assert oanim.ops.anim.keys_duplicate(frames="10", ignore_bones="1") == {'FINISHED'}
doc.frame_current = 20
for a, b in zip(shown(), look):
    assert close(a, b), ("the shape stays where it was", a, b)
for (ai, ao), (bi, bo) in zip(handles(), look_handles):
    assert close(ai, bi) and close(ao, bo), ("handles too", ai, bi)
assert abs(rig.bones[0]["angle"] - 0.8) < 1e-5, "the bones keep their own keys"
oanim.ops.ed.undo()

# Kept for next time: no ignore_bones given, it is still on.
doc.frame_current = 10
select_point_keys(10)
assert oanim.ops.anim.keys_duplicate(frames="10") == {'FINISHED'}
doc.frame_current = 20
assert close(shown()[0], look[0]), "Ignore Bones was kept"
oanim.ops.ed.undo()

# Paste Keys: copied on frame 10, pasted on frame 30 where the bone turns the other way.
doc.frame_current = 10
select_point_keys(10)
assert oanim.ops.anim.keys_copy() == {'FINISHED'}
doc.frame_current = 30
assert oanim.ops.anim.keys_paste(ignore_bones="1") == {'FINISHED'}
doc.frame_current = 31
doc.frame_current = 30
for a, b in zip(shown(), look):
    assert close(a, b), ("pasted, the shape stays where it was", a, b)
oanim.ops.ed.undo()
doc.frame_current = 30
assert oanim.ops.anim.keys_paste(ignore_bones="0") == {'FINISHED'}
assert not close(shown()[0], look[0]), "a plain paste is carried by the bone"

# Keep Place: the shape from frame 20, put where the shape is on frame 30.
oanim.ops.anim.keys_duplicate(frames="0", ignore_bones="0")  # Ignore Bones off for what follows
oanim.ops.ed.undo()
layers.active = rig  # a new layer above it: outside the rig, no bones moving it
oanim.ops.layer.new()
plain = layers.active
plain.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True)


def place(frame, points):
    doc.frame_current = frame
    for i, p in enumerate(points):
        plain.set_point(i, p)
    doc.frame_current = frame + 1


def centre(pts):
    return (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))


squashed = [(2.0, 0.0), (4.0, 0.0), (4.0, 0.5), (2.0, 0.5)]
place(20, squashed)
place(30, [(5.0, 0.0), (6.0, 0.0), (6.0, 1.0), (5.0, 1.0)])
doc.frame_current = 30
was_centre = centre(plain.shown()[0])
layers.active = plain
for l in layers.all():
    l.select = l.id == plain.id
assert oanim.ops.anim.keys_select(layers=str(plain.id), frame_start="20", frame_end="20") == {'FINISHED'}
assert oanim.ops.anim.keys_duplicate(frames="10", keep_place="1") == {'FINISHED'}
doc.frame_current = 31
doc.frame_current = 30
now = plain.shown()[0]
assert close(centre(now), was_centre), ("the centre stays", centre(now), was_centre)
c_sq = centre(squashed)
for p, q in zip(now, squashed):
    assert close((p[0] - centre(now)[0], p[1] - centre(now)[1]), (q[0] - c_sq[0], q[1] - c_sq[1])), ("the shape is copied", now, squashed)
oanim.ops.ed.undo()

# Paste with Keep Place, and it is kept for next time.
assert oanim.ops.anim.keys_select(layers=str(plain.id), frame_start="20", frame_end="20") == {'FINISHED'}
assert oanim.ops.anim.keys_copy() == {'FINISHED'}
doc.frame_current = 30
assert oanim.ops.anim.keys_paste() == {'FINISHED'}
doc.frame_current = 31
doc.frame_current = 30
assert close(centre(plain.shown()[0]), was_centre), "pasted: the centre stays (Keep Place kept)"

print("keys ignore bones: ok")
