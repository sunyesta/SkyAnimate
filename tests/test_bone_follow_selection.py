"""Bone Follows Selection (the Layer tab's Bones panel): on every frame the selection is keyed in the
timeline being edited, the layer's parent bone is keyed where the selection shows, the drawings kept where
they are (Move Only Bones). Keep Offset, and Fit: Move, Move & Turn, Move, Turn & Scale.

Run with:  OpenAnimate --background --python tests/test_bone_follow_selection.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="head", head_x="0", head_y="0", tail_x="0", tail_y="2", parent="")
ops.armature.bone_add(name="eye", head_x="0.4", head_y="1.2", tail_x="0.7", tail_y="1.2", parent="head")
ops.armature.bone_add(name="dial", head_x="2", head_y="0", tail_x="3", tail_y="0", parent="")
ops.layer.new()
eye = layers.active
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
eye = next(l for l in layers.all() if l.id == eye.id)
ring = [(0.5 + 0.1 * math.cos(a * math.pi / 3), 1.0 + 0.1 * math.sin(a * math.pi / 3)) for a in range(6)]
eye.add_curve(ring)
layers.active = eye
assert ops.layer.parent_bone_set(bone="eye") == {'FINISHED'}
eye = next(l for l in layers.all() if l.id == eye.id)
middle0 = (0.5, 1.0)

# An action bone action: as the dial turns, the eye slides and turns (its points keyed, Linear).
layers.active = rig
rig.select_bone("dial")
assert ops.anim.action_new(name="dial", action_bone="1") == {'FINISHED'}
doc.auto_key = True
moves = {10: ((0.3, 0.1), 0.4), 20: ((0.6, -0.2), 0.8)}
for frame, ((dx, dy), turn) in moves.items():
    doc.frame_current = frame
    rig.bone_set("dial", angle=frame / 20.0)
    for i, (x, y) in enumerate(ring):
        ox, oy = x - middle0[0], y - middle0[1]
        c, s = math.cos(turn), math.sin(turn)
        eye.place_point(i, (middle0[0] + dx + c * ox - s * oy, middle0[1] + dy + s * ox + c * oy))
    doc.frame_current = frame + 1
for frame in moves:
    for i in range(len(ring)):
        eye.keyframe_set("points[%d].pos" % i, frame, interp="LINEAR")


def shown(frame):
    doc.frame_current = frame
    return eye.shown()[0]


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


def worst(a, b):
    return max(math.dist(p, q) for p, q in zip(a, b))


frames = (0, 5, 10, 15, 20)
before = {f: shown(f) for f in frames}
eye_rest = bone("eye")
doc.frame_current = 0
rest_head = bone("eye")["head"]

# Nothing on the setup frame of the mainline, nor without a parent bone.
assert ops.anim.action_set(name="") == {'FINISHED'}
layers.active = eye
assert ops.layer.bone_follow_selection() == {'CANCELLED'}
assert ops.anim.action_set(name="dial") == {'FINISHED'}

# Move, Keep Offset (the defaults), with nothing selected: the whole eye.
doc.frame_current = 10
layers.active = eye
eye.select_points(range(len(ring)), False)
assert ops.layer.bone_follow_selection() == {'FINISHED'}
assert doc.frame_current == 10
for frame, ((dx, dy), turn) in moves.items():
    doc.frame_current = frame
    head = bone("eye")["head"]
    assert math.dist(head, (rest_head[0] + dx, rest_head[1] + dy)) < 1e-3, ("moved with the eye", frame, head)
for f in frames:
    assert worst(shown(f), before[f]) < 1e-4, ("the drawing stays", f, worst(shown(f), before[f]))
keys = rig.keyframes('bones["eye"].position')
assert [k["frame"] for k in keys if k["frame"]] == [10, 20] and all(k["interp"] == "LINEAR" for k in keys if k["frame"]), keys
ops.ed.undo()
for f in frames:
    assert worst(shown(f), before[f]) < 1e-6, ("undone", f)

# Move & Turn: the bone turns with the eye too.
doc.frame_current = 10
layers.active = eye
assert ops.layer.bone_follow_selection(fit="TURN") == {'FINISHED'}
doc.frame_current = 0
rest_dir = math.atan2(bone("eye")["tail"][1] - bone("eye")["head"][1], bone("eye")["tail"][0] - bone("eye")["head"][0])
for frame, ((dx, dy), turn) in moves.items():
    doc.frame_current = frame
    b = bone("eye")
    direction = math.atan2(b["tail"][1] - b["head"][1], b["tail"][0] - b["head"][0])
    assert abs(direction - rest_dir - turn) < 1e-3, ("turned with the eye", frame, direction - rest_dir)
    ox, oy = rest_head[0] - middle0[0], rest_head[1] - middle0[1]
    c, s = math.cos(turn), math.sin(turn)
    want = (middle0[0] + dx + c * ox - s * oy, middle0[1] + dy + s * ox + c * oy)
    assert math.dist(b["head"], want) < 1e-3, ("its offset turns too", frame, b["head"], want)
# Exactly on the keyed frames; between them a turning bone carries the points round an arc while their
# keys go straight, so they drift a little there.
for f in frames:
    assert worst(shown(f), before[f]) < (1e-4 if f % 10 == 0 else 0.01), ("the drawing stays when turned", f,
                                                                          worst(shown(f), before[f]))
ops.ed.undo()

# Keep Offset off, one point selected: the head goes onto it.
doc.frame_current = 10
layers.active = eye
eye.select_points([0])
assert ops.layer.bone_follow_selection(keep_offset="0") == {'FINISHED'}
doc.frame_current = 20
assert math.dist(bone("eye")["head"], shown(20)[0]) < 1e-3, "onto the selected point"
for f in frames:
    assert worst(shown(f), before[f]) < 1e-4, ("the drawing stays", f)

# A group following a bone as a whole (a group's Parent Bone always does): the button in its Bones panel
# moves its bone with the points of the drawings inside it; the bone takes their motion, so their points
# are keyed back where they showed and the group's own transform is left alone.
ops.ed.undo()
doc.frame_current = 0
assert ops.anim.action_set(name="") == {'FINISHED'}
layers.active = rig
ops.armature.bone_add(name="brow", head_x="-0.4", head_y="1.5", tail_x="-0.1", tail_y="1.5", parent="head")
ops.layer.new()
brow = layers.active
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
brow = next(l for l in layers.all() if l.id == brow.id)
line = [(-0.5, 1.4), (-0.3, 1.45), (-0.1, 1.4)]
brow.add_curve(line)
layers.active = brow
assert ops.layer.group() == {'FINISHED'}
group = layers.active
assert group.type == "GROUP"
assert ops.layer.parent_bone_set(bone="brow") == {'FINISHED'}
brow = next(l for l in layers.all() if l.id == brow.id)
assert ops.anim.action_set(name="dial") == {'FINISHED'}
for frame, (dx, dy) in {10: (0.0, 0.2), 20: (0.1, 0.35)}.items():
    doc.frame_current = frame
    for i, (x, y) in enumerate(line):
        brow.place_point(i, brow.to_document((x + dx, y + dy)))
    doc.frame_current = frame + 1
for frame in (10, 20):
    for i in range(len(line)):
        brow.keyframe_set("points[%d].pos" % i, frame, interp="LINEAR")


def brow_shown(frame):
    doc.frame_current = frame
    return brow.shown()[0]  # document space already


brow_before = {f: brow_shown(f) for f in frames}
doc.frame_current = 0
brow_rest = bone("brow")["head"]
doc.frame_current = 10
group = next(l for l in layers.all() if l.id == group.id)
layers.active = group
assert ops.layer.bone_follow_selection() == {'FINISHED'}
for frame, (dx, dy) in {10: (0.0, 0.2), 20: (0.1, 0.35)}.items():
    doc.frame_current = frame
    assert math.dist(bone("brow")["head"], (brow_rest[0] + dx, brow_rest[1] + dy)) < 1e-3, ("the brow bone follows", frame)
for f in frames:
    w = worst(brow_shown(f), brow_before[f])
    assert w < 1e-4, ("the brow stays", f, w)
assert not group.key_frames, "the group's transform isn't keyed"
doc.frame_current = 20
assert all(math.dist(p[:2], q) < 1e-4 for p, q in zip(brow.points, line)), "the brow's points hold no move now"

# A key the bone already had on a frame the selection has none (an Auto-Freeze key from before the
# bone moved, back at rest): it is fitted too, so the bone doesn't dart back there for a frame.
ops.ed.undo()
doc.frame_current = 15
layers.active = rig
rig.bone_set("eye", position=next(b for b in rig.bones if b["name"] == "eye")["setup_position"])
doc.frame_current = 16
eye = next(l for l in layers.all() if l.id == eye.id)
before = {f: shown(f) for f in frames}
doc.frame_current = 10
layers.active = eye
eye.select_points(range(len(ring)), False)
oanim.data.document.auto_freeze_keys = True
assert ops.layer.bone_follow_selection() == {'FINISHED'}
doc.frame_current = 15
head = bone("eye")["head"]
assert math.dist(head, (rest_head[0] + 0.45, rest_head[1] - 0.05)) < 1e-3, ("the stray key follows too", head)
for f in frames:
    assert worst(shown(f), before[f]) < 1e-4, ("the drawing stays", f, worst(shown(f), before[f]))
assert not rig.keyframes('bones["head"].position'), "no other bone keyed (Auto-Freeze is off for it)"

print("bone follows selection: ok")
