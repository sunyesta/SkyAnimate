"""Parent Bone's Keep Animation: changing the bone a layer follows (or freeing it) keeps the layer where it
showed on every keyed frame, its position, rotation and scale keyed again there against the new bone (the
reverse of Keep Poses: the setup is left alone, the poses kept). Off, the layer follows the new bone's
motion from its setup pose.

Run with:  OpenAnimate --background --python tests/test_parent_keep_animation.py
"""

import oanim

layers = oanim.data.layers
doc = oanim.data.document
ops = oanim.ops
doc.frame_current = 0


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="leg", head_x="0", head_y="-1", tail_x="1", tail_y="-1", parent="")
ops.layer.new()
prop = layers.active
prop.name = "prop"
prop.add_curve([(0.5, 0.2), (0.9, 0.3)])
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
prop = next(l for l in layers.all() if l.name == "prop")
layers.active = prop
assert ops.layer.parent_bone_set(bone="arm", deform="LAYER") == {'FINISHED'}

# Posed: the arm and leg keyed on frame 10, the prop's own position on frame 5.
doc.frame_current = 10
rig.bone_set("arm", angle=0.8)
rig.bone_set("leg", angle=-0.5, position=(0.3, -1.2))
layers.active = rig
ops.armature.select_all()
ops.anim.keyframe_insert()
doc.frame_current = 5
layers.active = prop
prop.location_x = 0.4
ops.anim.keyframe_insert()
doc.frame_current = 0


def placed():
    """Where the prop's two points show (document space) on the frames that matter."""
    out = {}
    for f in (5, 10, 15):
        doc.frame_current = f
        out[f] = [prop.to_document(p[:2]) for p in prop.points]
    doc.frame_current = 0
    return out


was = placed()
setup = [prop.to_document(p[:2]) for p in prop.points]

# Keep Animation: to the leg, then free: the prop shows where it did on every frame, frame 0 too.
layers.active = prop
assert ops.layer.parent_bone_set(bone="leg", keep_animation="1") == {'FINISHED'}
assert prop.parent_bone == "leg"
assert oanim.wm.undo_history()[0][-1] == "Parent to Bone, Keeping Animation"
now = placed()
for f in was:
    assert all(close(a, b) for a, b in zip(now[f], was[f])), ("frame %d stays" % f, now[f], was[f])
assert all(close(a, b) for a, b in zip([prop.to_document(p[:2]) for p in prop.points], setup)), "the setup too"
layers.active = prop
assert ops.layer.parent_bone_set(bone="", keep_animation="1") == {'FINISHED'}
now = placed()
for f in was:
    assert all(close(a, b) for a, b in zip(now[f], was[f])), ("freed, frame %d stays" % f, now[f], was[f])

# Off: the prop follows the new bone's motion instead.
ops.ed.undo()
ops.ed.undo()
assert prop.parent_bone == "arm"
layers.active = prop
assert ops.layer.parent_bone_set(bone="leg") == {'FINISHED'}
now = placed()
assert not all(close(a, b) for a, b in zip(now[10], was[10])), "off: it moves with the leg"
assert all(close(a, b) for a, b in zip([prop.to_document(p[:2]) for p in prop.points], setup))

# Points (the default for drawings): the bone moves the points; Keep Animation keys the layer's own
# transform so they still show where they did.
doc.frame_current = 0
layers.active = rig
ops.layer.new()
dots = layers.active
dots.name = "dots"
dots.add_curve([(0.5, 0.4), (0.8, 0.6)])
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
dots = next(l for l in layers.all() if l.name == "dots")
layers.active = dots
assert ops.layer.parent_bone_set(bone="arm") == {'FINISHED'}
assert dots.parent_bone_deform == "POINTS"


def shown_dots():
    """On the setup frame, the bones' key frame (10) and after it; in between it eases its own way."""
    out = {}
    for f in (0, 10, 15):
        doc.frame_current = f
        out[f] = dots.shown()[0]
    doc.frame_current = 0
    return out


was = shown_dots()
assert not all(close(a, b) for a, b in zip(was[10], [(0.5, 0.4), (0.8, 0.6)])), "the arm moves them"
layers.active = dots
assert ops.layer.parent_bone_set(bone="leg", keep_animation="1") == {'FINISHED'}
now = shown_dots()
for f in was:
    assert all(close(a, b) for a, b in zip(now[f], was[f])), ("points: frame %d stays" % f, now[f], was[f])

# Every action, not just the one being edited: keys in the mainline and in an action "wave", reparented
# from the mainline, both still show the layer where it was.
doc.frame_current = 0
if doc.action != "":
    ops.anim.action_set(name="")
layers.active = rig
ops.layer.new()
flag = layers.active
flag.name = "flag"
flag.add_curve([(0.3, 0.3), (0.6, 0.4)])
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
flag = next(l for l in layers.all() if l.name == "flag")
layers.active = flag
assert ops.layer.parent_bone_set(bone="arm", deform="LAYER") == {'FINISHED'}
layers.active = rig
assert ops.anim.action_new(name="wave") == {'FINISHED'} and doc.action == "wave"
doc.frame_current = 12
rig.bone_set("arm", angle=-0.6)
doc.frame_current = 18
rig.bone_set("leg", angle=0.7, position=(0.2, -1.3))
doc.frame_current = 19
doc.frame_current = 0


def flag_at(action, frame):
    if doc.action != action:
        assert ops.anim.action_set(name=action) == {'FINISHED'}
    doc.frame_current = frame
    return flag.shown()[0]


frames = (("", 10), ("", 15), ("wave", 12), ("wave", 18))
before = {k: flag_at(*k) for k in frames}
flag_at("", 0)
layers.active = flag
assert ops.layer.parent_bone_set(bone="leg", keep_animation="1") == {'FINISHED'}
assert doc.action == "", "back in the action it was done in"
for k in frames:
    now = flag_at(*k)
    assert all(close(a, b) for a, b in zip(now, before[k])), ("kept in %s frame %d" % (k[0] or "the mainline", k[1]), now, before[k])
ops.anim.action_set(name="")

print("parent_keep_animation: ok")
