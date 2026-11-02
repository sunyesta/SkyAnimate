"""Copy and paste shapes over the viewport: Ctrl C copies what Shift D would, Ctrl V pastes it into the active drawing
as one undo step, selected and on top. Keep Place (on at first) lands them where they looked when copied, Keyframes (off
at first) brings their keys, on the mainline and the action lines the layer has. Off, the pose they were copied in
is their setup. Weights find bones by name; looks come along. The clipboard is the layers': shapes pasted over the Layers
editor make a new layer, layers pasted over the viewport paste as layers.

Run with:  OpenAnimate --background --python tests/test_shape_clipboard.py
"""
import math

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm
doc.frame_current = 0


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    # (Any undo step, so that undoing a paste keeps the name.)
    assert ops.object.modifier_add(type="RESAMPLE") == {'FINISHED'}
    assert ops.object.modifier_remove(index="0") == {'FINISHED'}
    return layer


def find(name):
    return next(l for l in layers.all() if l.name == name)


def box(layer, lo, hi):
    layer.add_curve([(lo, lo), (hi, lo), (hi, hi), (lo, hi)], closed=True, fill=True)


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def close(a, b, eps=1e-4):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def copy_keys(source, points):
    """Copies `source`'s points through the key, as over the viewport."""
    layers.active = source
    ops.mesh.select_none()
    source.select_points(points)
    assert wm.event_simulate("C", ctrl=True, area="Viewport")


def paste_key(target):
    layers.active = target
    assert wm.event_simulate("V", ctrl=True, area="Viewport")


def picked(layer):
    return [p[2] for p in layer.points]


def shown(layer):
    return [layer.to_document(p) for p in layer.evaluated()[0]]


# --- Between layers, through the keys ----------------------------------------------------------------------------------
source = new_layer("Source")
box(source, 0.1, 0.3)
box(source, -0.5, -0.3)
source.opacity = 0.5
target = new_layer("Target")
box(target, 0.5, 0.7)
copy_keys(source, [0, 1, 2, 3])
assert source.point_count == 8 and source.curve_count == 2, "copying changes nothing"
paste_key(target)
assert target.point_count == 8 and target.curve_count == 2 and target.shape_count == 2, target.shapes
assert picked(target) == [False] * 4 + [True] * 4, "the pasted points are the selection"
assert sorted(tuple(p[:2]) for p in target.points[4:]) == sorted(tuple(p[:2]) for p in source.points[:4])
assert abs(target.opacity - 1.0) < 1e-6, "the layer's own settings stay its own"
assert wm.undo_history()[0][-1] == "Paste Shapes", wm.undo_history()[0][-1]
ids = [s for s in target.shapes]
assert len(set(s[0] for s in ids)) == 2, ("names of its own", ids)
ops.ed.undo()
target = find("Target")
assert target.point_count == 4, "one undo step"
ops.ed.redo()
target = find("Target")
assert target.point_count == 8

# Pasted on top: the new shape is last, drawn over the others.
assert target.shapes[-1][0] != target.shapes[0][0]

# A piece of a line, as Shift D copies it.
ops.ed.undo()
line = new_layer("Line")
line.add_curve([(0, 2), (1, 2), (2, 2), (3, 2)])
copy_keys(line, [1, 2])
target = find("Target")
paste_key(target)
assert target.point_count == 6 and target.curve_count == 2, (target.point_count, target.curve_count)
ops.ed.undo()

# Nothing that makes a line: said, and the clipboard left alone.
layers.active = line
ops.mesh.select_none()
line.select_points([0])
assert ops.mesh.copy() == {'CANCELLED'}
target = find("Target")
copy_keys(source, [0, 1, 2, 3])

# Several layers' shapes, into the one active drawing.
layers.active = line
ops.mesh.select_none()
layers.active = source
ops.mesh.select_none()
source.select_points([4, 5, 6, 7])
line.selected = True
line.select_points([1, 2])
assert ops.mesh.copy() == {'FINISHED'}
line.selected = False
paste_key(target)
assert target.point_count == 10 and target.curve_count == 3, (target.point_count, target.curve_count)
assert picked(target) == [False] * 4 + [True] * 6, ("everything pasted is selected", picked(target))
ops.ed.undo()

# --- Keep Place --------------------------------------------------------------------------------------------------------
doc.frame_current = 0
target = find("Target")
target.location_x, target.location_y = 0.4, -0.2
source = find("Source")
copy_keys(source, [0, 1, 2, 3])
wanted = [source.to_document(p[:2]) for p in source.points[:4]]
paste_key(target)
got = [target.to_document(p[:2]) for p in target.points[4:]]
assert all(close(a, b) for a, b in zip(sorted(got), sorted(wanted))), ("where it looked when copied", got, wanted)
ops.ed.undo()
target = find("Target")
layers.active = target
assert ops.mesh.paste(keep_place="0") == {'FINISHED'}
got = [tuple(p[:2]) for p in target.points[4:]]
assert all(close(a, b[:2]) for a, b in zip(sorted(got), sorted(tuple(p[:2]) for p in source.points[:4]))), got
ops.ed.undo()
target = find("Target")
target.location_x, target.location_y = 0.0, 0.0

# --- Keyframes ---------------------------------------------------------------------------------------------------------
# Off: the pose as copied, on every frame, with no keys. Copied on frame 20, pasted on frame 30.
doc.frame_current = 0
keyed = new_layer("Keyed")
box(keyed, 0.1, 0.3)
rest = [tuple(p[:2]) for p in keyed.points]
doc.frame_current = 20
for i, p in enumerate(rest):
    keyed.set_point(i, (p[0] + 0.3, p[1] + 0.1))
at(20)
copy_keys(keyed, [0, 1, 2, 3])
pose = [tuple(p[:2]) for p in keyed.points]
empty = new_layer("Empty")
doc.frame_current = 30
paste_key(empty)
at(30)
assert empty.point_count == 4 and empty.key_frames == [], ("no keys", empty.key_frames)
for frame in (0, 20, 30, 40):
    doc.frame_current = frame
    assert all(close(a, b) for a, b in zip(sorted(tuple(p[:2]) for p in empty.points), sorted(pose))), (
        "that pose on every frame", frame, empty.points)
doc.frame_current = 30
ops.ed.undo()
empty = find("Empty")

# On: the keys, on their own frames, and it plays as the original does.
doc.frame_current = 0
layers.active = empty
assert ops.mesh.paste(keyframes="1") == {'FINISHED'}
assert empty.key_frames == [20] or 20 in empty.key_frames, empty.key_frames
for frame in (0, 10, 20):
    doc.frame_current = frame
    a, b = sorted(tuple(p[:2]) for p in empty.points), sorted(tuple(p[:2]) for p in find("Keyed").points)
    assert all(close(p, q) for p, q in zip(a, b)), (frame, a, b)
doc.frame_current = 0
ops.ed.undo()

# Across a difference in scale, a key's change scales with the rest.
empty = find("Empty")
empty.scale_x, empty.scale_y = 2.0, 2.0
assert ops.mesh.paste(keyframes="1") == {'FINISHED'}
empty = find("Empty")
for frame in (0, 20):
    doc.frame_current = frame
    a = sorted(empty.to_document(p[:2]) for p in empty.points)
    b = sorted(find("Keyed").to_document(p[:2]) for p in find("Keyed").points)
    assert all(close(p, q, 1e-3) for p, q in zip(a, b)), (frame, a, b)
doc.frame_current = 0
ops.ed.undo()
empty = find("Empty")
empty.scale_x, empty.scale_y = 1.0, 1.0

# --- Action lines ----------------------------------------------------------------------------------------------------
doc.frame_current = 0
new_layer("Outside")
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
rig.name = "Rig"
assert ops.armature.bone_add(name="head", head_x="0", head_y="0", tail_x="0", tail_y="1") == {'FINISHED'}
ops.armature.bone_parent_set(bone="head", parent="")
assert ops.armature.bone_add(name="far", head_x="2", head_y="0", tail_x="2", tail_y="1") == {'FINISHED'}
ops.armature.bone_parent_set(bone="far", parent="")
for name in ("Acting", "Into", "Lost"):
    layer = new_layer(name)
    box(layer, 0.1, 0.3)
    assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
acting, into = find("Acting"), find("Into")
rig = find("Rig")
layers.active = rig
assert ops.anim.action_new(name="Wave") == {'FINISHED'} and doc.action == "Wave"
doc.frame_current = 12
start = [tuple(p[:2]) for p in acting.points]
acting.set_point(0, (start[0][0] - 0.2, start[0][1]))
at(12)
waved = tuple(acting.points[0][:2])
doc.frame_current = 0
assert ops.anim.action_set(name="") == {'FINISHED'}
doc.frame_current = 6
acting.set_point(1, (start[1][0], start[1][1] + 0.25))
at(6)
doc.frame_current = 0

# Copied while on the mainline: both lines come.
copy_keys(acting, [0, 1, 2, 3])
ops.mesh.select_none()
into = find("Into")
paste_key(into)
ops.ed.undo()
into = find("Into")
layers.active = into
assert ops.mesh.paste(keyframes="1") == {'FINISHED'}
into = find("Into")
assert 6 in into.key_frames, ("the mainline's", into.key_frames)
doc.frame_current = 6
assert close(into.points[5][:2], (start[1][0], start[1][1] + 0.25)), into.points[5]
doc.frame_current = 0
layers.active = rig
assert ops.anim.action_set(name="Wave") == {'FINISHED'}
doc.frame_current = 12
into = find("Into")
assert close(into.points[4][:2], waved), ("the action's", into.points[4], waved)
doc.frame_current = 0
assert ops.anim.action_set(name="") == {'FINISHED'}

# Copied and pasted while editing the action: the same, the timeline being edited showing its keys.
assert ops.anim.action_set(name="Wave") == {'FINISHED'}
acting = find("Acting")
copy_keys(acting, [0, 1, 2, 3])
doc.frame_current = 12
into2 = find("Lost")
paste_key(into2)
into2 = find("Lost")
assert ops.mesh.paste(keyframes="1", keep_place="0") == {'FINISHED'}
into2 = find("Lost")
at(12)
assert close(into2.points[8][:2], waved), ("the action's, while in it", into2.points[8], waved)
doc.frame_current = 0
assert ops.anim.action_set(name="") == {'FINISHED'}
into2 = find("Lost")
doc.frame_current = 6
assert close(into2.points[9][:2], (start[1][0], start[1][1] + 0.25)), ("the mainline's", into2.points[9])
doc.frame_current = 0

# A layer outside the rig has no such action: those keys are left out, the mainline's come.
acting = find("Acting")
copy_keys(acting, [0, 1, 2, 3])
outside = find("Outside")
assert outside.parent is None or outside.parent.name != "Rig", "outside the rig"
layers.active = outside
assert ops.mesh.paste(keyframes="1") == {'FINISHED'}
outside = find("Outside")
assert 6 in outside.key_frames, outside.key_frames
assert outside.point_count == 4
ops.ed.undo()
outside = find("Outside")
assert outside.point_count == 0

# --- Weights by name --------------------------------------------------------------------------------------------------
doc.frame_current = 0
acting = find("Acting")
layers.active = acting
ops.mesh.select_none()
acting.select_points([0, 1, 2, 3])
rig = find("Rig")
rig.select_bone("head")
layers.active = acting
assert ops.weight.assign(weight="1") == {'FINISHED'}
copy_keys(acting, [0, 1, 2, 3])
into = find("Into")
paste_key(into)
into = find("Into")
assert all(w.get("head") == 1.0 for w in into.weights[-4:]), into.weights[-4:]
outside = find("Outside")
paste_key(outside)
outside = find("Outside")
assert all(not w for w in outside.weights[-4:]), ("no such bone here", outside.weights[-4:])
ops.ed.undo()

# In another rig the same name is another bone; a bone it hasn't goes.
doc.frame_current = 0
outside = find("Outside")
layers.active = outside
assert ops.layer.new_bone() == {'FINISHED'}
other_rig = layers.active
other_rig.name = "OtherRig"
for bone, x in (("spare", "5"), ("extra", "6"), ("head", "7")):
    assert ops.armature.bone_add(name=bone, head_x=x, head_y="0", tail_x=x, tail_y="1") == {'FINISHED'}
    ops.armature.bone_parent_set(bone=bone, parent="")
new_layer("OtherArt")
assert ops.layer.move_to_group(group=str(other_rig.id)) == {'FINISHED'}
other_art = find("OtherArt")
copy_keys(find("Acting"), [0, 1, 2, 3])
paste_key(other_art)
other_art = find("OtherArt")
assert all(list(w) == ["head"] for w in other_art.weights[-4:]), ("its own bone of that name", other_art.weights[-4:])
doc.frame_current = 15
rig_other = find("OtherRig")
at(15)
before_turn = [tuple(p) for p in other_art.shown()[0]]
rig_other.bone_set("head", position=(rig_other.bones[2]["position"][0] + 0.3, rig_other.bones[2]["position"][1]))
at(15)
assert all(close((a[0] + 0.3, a[1]), b, 1e-3) for a, b in zip(before_turn, find("OtherArt").shown()[0])), "it moves with it"
doc.frame_current = 0

# Its action keys were left out, so nothing waits in a Wave of its own if it joins the rig later.
outside = find("Outside")
layers.active = outside
copy_keys(find("Acting"), [0, 1, 2, 3])
layers.active = outside
assert ops.mesh.paste(keyframes="1", keep_place="0") == {'FINISHED'}
assert ops.layer.move_to_group(group=str(find("Rig").id)) == {'FINISHED'}
layers.active = find("Rig")
assert ops.anim.action_set(name="Wave") == {'FINISHED'}
doc.frame_current = 12
outside = find("Outside")
assert close(outside.points[0][:2], start[0]), ("no Wave keys", outside.points[0], start[0])
doc.frame_current = 0
assert ops.anim.action_set(name="") == {'FINISHED'}

# Keep Place off the setup frame, among bones moved there: where it was copied from, with no key.
rig = find("Rig")
doc.frame_current = 15
rig.bone_set("head", position=(rig.bones[0]["position"][0] + 0.4, rig.bones[0]["position"][1]))
at(15)
acting = find("Acting")
copy_keys(acting, [0, 1, 2, 3])
wanted = sorted(acting.shown()[0][:4])
into = find("Into")
paste_key(into)
at(15)
into = find("Into")
got = sorted(into.shown()[0][-4:])
assert all(close(a, b, 1e-3) for a, b in zip(got, wanted)), ("where it was copied from", got, wanted)
assert all(close(q[:2], (p[0] - 0.4, p[1]), 1e-3) for p, q in zip(sorted(wanted), sorted(tuple(p) for p in into.points[-4:]))), (
    "the bones' move isn't in its setup", into.points[-4:], wanted)
ops.ed.undo()
doc.frame_current = 0

# --- Looks, and point groups ---------------------------------------------------------------------------------------------
doc.frame_current = 0
ops.layer.new()
styled = layers.active
styled.name = "Styled"
box(styled, 0.1, 0.3)
styled.select_points([0, 1])
ops.mesh.point_group_add(name="Rim")
assert ops.mesh.point_group_assign(weight="0.5") == {'FINISHED'}
kept = new_layer("Kept")
box(kept, 0.5, 0.7)
kept.select_points([0])
ops.mesh.point_group_add(name="Rim")
assert ops.mesh.point_group_assign(weight="1") == {'FINISHED'}
none = new_layer("NoGroups")
box(none, 0.5, 0.7)
copy_keys(find("Styled"), [0, 1, 2, 3])
paste_key(find("Kept"))
kept = find("Kept")
layers.active = kept
ops.mesh.select_none()
assert ops.mesh.point_group_select() in ({'FINISHED'}, {'CANCELLED'})
assert sum(p[2] for p in kept.points) == 3, ("the group it has takes the pasted points in", kept.points)
paste_key(find("NoGroups"))
assert [g[0] for g in find("NoGroups").point_groups] == [], "a group it hasn't isn't made"

# --- Refused -----------------------------------------------------------------------------------------------------------
doc.frame_current = 0
layers.active = find("Rig")
assert ops.mesh.paste() == {'CANCELLED'}, "not a drawing"
layers.active = find("Styled")
assert ops.layer.make_reference() == {'FINISHED'}
assert layers.active.reference_source
assert ops.mesh.paste() == {'CANCELLED'}, "a copy of a layer can't get new shapes"
ops.ed.undo()

# --- One clipboard -------------------------------------------------------------------------------------------------
copy_keys(find("Styled"), [0, 1, 2, 3])
before = [l.name for l in layers.all()]
layers.active = find("Kept")
assert wm.event_simulate("V", ctrl=True, area="Layers")
after = [l.name for l in layers.all()]
made = [n for n in after if n not in before]
assert len(made) == 1 and find(made[0]).point_count == 4, ("shapes over the Layers editor: a new layer", made)
ops.ed.undo()

layers.active = find("Styled")
assert wm.event_simulate("C", ctrl=True, area="Layers")
count = len(list(layers.all()))
assert wm.event_simulate("V", ctrl=True, area="Viewport")
assert len(list(layers.all())) == count + 1, "layers over the viewport: layers"

print("shape clipboard: ok")
