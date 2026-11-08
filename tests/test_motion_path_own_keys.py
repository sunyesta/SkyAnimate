"""A path's keys and its timing row are its own: a point's moves, a bone's moves, turns and sizes (and its knob's keys,
which play the actions moving it), a layer's turns, moves and sizes. A bone moving a point, a parent bone or a group
above has its own row; its keys would split stretches the row can't retime. A point only a bone moves has no row."""

import oanim

oanim.data.document.auto_freeze_keys = False  # keyed channel by channel here, not holding the poses either side

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers


def paths():
    return oanim.wm.motion_paths()


def path(label):
    return next(p for p in paths() if p["label"] == label)


def rows():
    return sorted(r["label"] for r in oanim.wm.motion_path_timing())


def bone(rig, name):
    return next(b for b in rig.bones if b["name"] == name)


ops.wm.read_homefile()
oanim.data.document.auto_freeze_keys = False  # keyed channel by channel here, not holding the poses either side
doc.auto_key = True
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="brow", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.select_none()
ops.armature.bone_add(name="arm", head_x="0", head_y="2", tail_x="1", tail_y="2", parent="")
ops.armature.select_none()
ops.armature.bone_add(name="hand", head_x="1", head_y="2", tail_x="2", tail_y="2", parent="arm")
ops.layer.new()
face = layers.active
face.key_whole_drawing = False
face.add_curve([(0.0, 0.0), (1.0, 0.0)])
ops.layer.move_to_group(group=str(rig.id))

# The brow bone moves at 9; the hand's parent turns at 6, the hand itself at 12.
layers.active = rig
doc.frame_current = 9
rig.bone_set("brow", position=(0.0, -1.0))
doc.frame_current = 6
rig.bone_set("arm", angle=0.3)
doc.frame_current = 12
rig.bone_set("hand", angle=0.4)
doc.frame_current = 13
doc.motion_paths = True
doc.path_all = True  # every key the path has, not just those near the frame

# --- A child bone: its own turn, not its parent's ---------------------------------------------------------------
rig.select_bone("hand")
p = path("hand tip")
assert p["keys"] == [12], ("the hand's own key, not the arm's at 6", p["keys"])
assert "hand tip" in rows()

# --- A point a bone moves --------------------------------------------------------------------------------------
layers.active = face
for l in layers:
    l.selected = l.name == face.name
face.select_points([0])
before = [k["frame"] for k in rig.keyframes('bones["brow"].position')]
assert before == [0, 9], before
p = path(face.name + " point 0")
assert p["keys"] == [], ("nothing of its own: the bone's key isn't the point's", p["keys"])
assert not any(r.startswith(face.name + " point") for r in rows()), ("no row to retime", rows())
doc.frame_current = 4
face.set_point(0, (0.5, 0.5))
doc.frame_current = 13
p = path(face.name + " point 0")
assert p["keys"] == [4], ("its own key only", p["keys"])
assert face.name + " point 0" in rows(), rows()
face.select_points([0], False)

# --- A layer in a group: its own moves, not the group's -------------------------------------------------------
doc.frame_current = 0
ops.layer.new()
inner = layers.active
inner.add_curve([(0.0, 0.0), (1.0, 0.0)])
for l in layers.all():
    l.selected = l.id == inner.id
assert ops.layer.group() == {'FINISHED'}
group = layers.active
assert group.id != inner.id
doc.frame_current = 7
group.location_x = 1.0
layers.active = inner
for l in layers.all():
    l.selected = l.id == inner.id
doc.frame_current = 15
inner.location_x = 2.0
doc.frame_current = 16
p = path(inner.name)
assert p["keys"] == [15], ("the layer's own move, not its group's at 7", p["keys"])

# --- A bone a controller plays: the knob's keys are how it moves ----------------------------------------------
layers.active = rig
for l in layers.all():
    l.selected = l.id == rig.id
rig.select_bone("hand")
assert ops.anim.action_new(name="raise") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("hand", angle=bone(rig, "hand")["setup_angle"] + 0.8)
doc.frame_current = 11
doc.action = ""
doc.frame_current = 0
ops.armature.select_none()
ops.armature.bone_add(name="stick", head_x="4", head_y="4", tail_x="4.5", tail_y="4", parent="")
rig.select_bone("stick")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="UP", action="raise") == {'FINISHED'}
doc.frame_current = 20
rig.bone_set("stick", knob=(0.0, 10.0))
doc.frame_current = 21
rig.select_bone("hand")
p = path("hand tip")
assert p["keys"] == [12, 20], ("its own key and the knob's", p["keys"])

print("Motion path own keys tests passed")
