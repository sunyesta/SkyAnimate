"""Own Motion: in the Motion Path tool every path shows only its own motion, picked or not, whatever else moves it (a
group above, a parent bone, its own constraints) held as on the frame shown, and nothing else drawn (no ghost of where it
really goes). Edits made on the path land where they are drawn. Off in the header, or outside the tool, the path is the
real one."""

import os
import tempfile

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return all(abs(x - y) < eps for x, y in zip(a, b))
    return abs(a - b) < eps


def path(label):
    return next(p for p in oanim.wm.motion_paths() if p["label"] == label)


def real_path(label):
    """The path as it really goes: Own Motion off for a moment."""
    doc.path_own_motion = False
    try:
        return path(label)
    finally:
        doc.path_own_motion = True


ops.wm.read_homefile()
assert doc.path_own_motion, "on by default"
doc.auto_key = True
doc.frame_current = 0
ops.layer.new()
art = layers.active
art.key_whole_drawing = False
art.add_curve([(0.0, 0.0), (1.0, 0.0)])
for l in layers.all():
    l.selected = l.id == art.id
assert ops.layer.group() == {'FINISHED'}
group = layers.active

# The group slides right 0-10; the point goes up 0-10.
doc.frame_current = 10
group.location_x = 5.0
layers.active = art
art.set_point(0, (0.0, 2.0))
doc.frame_current = 5
for l in layers.all():
    l.selected = l.id == art.id
art.select_points([0])
doc.path_all = True
doc.frame_end = 10
assert not doc.motion_paths
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert doc.motion_paths, "the Motion Path tool shows the paths it edits"
label = art.name + " point 0"
pid = str(art.point_ids[0])
on = dict(layer=str(art.id), point=pid)

# --- Nothing picked: only its own motion, the group held where it is now --------------------------------------
at = lambda q, f: q["points"][f - q["first"]]
assert not oanim.wm.motion_path_dots() and not any(k["selected"] for k in art.keyframes("points[0].pos"))
p = path(label)
assert p["own_motion"], "in the tool, picked or not"
assert close(at(p, 10), (2.5, 2.0)) and close(at(p, 0), (2.5, 0.0)), ("the group held as on frame 5", at(p, 10))
assert close(at(p, 5), (2.5, 1.0)), "the frame shown is where it is"
assert "ghost" not in p, "only its own motion drawn: no ghost of where it really goes"
assert all(close(q[0], 2.5) for q in p["points"]), "straight up: its own move alone"
real = real_path(label)
assert close(at(real, 10), (5.0, 2.0)) and close(at(real, 0), (0.0, 0.0)), "Own Motion off: the group carries it"

# --- A key picked: the same path, nothing flips -------------------------------------------------------------------
assert ops.anim.motion_path_select(frame="10", **on) == {'FINISHED'}
picked = path(label)
assert picked["own_motion"] and picked["points"] == p["points"], "picking changes nothing"

# Edits land where they are drawn: the key's dot dragged to x=3.5 on the held path.
assert ops.anim.motion_path_move(frame="10", x="3.5", y="2.0", **on) == {'FINISHED'}
p = path(label)
assert close(at(p, 10), (3.5, 2.0)), ("under the mouse", at(p, 10))
k = next(k for k in art.keyframes("points[0].pos") if k["frame"] == 10)
assert close(k["value"], (1.0, 2.0)), ("its own key: a step right", k["value"])

# The handle too: dragged on the held path, it lands there.
assert ops.anim.motion_path_handle(frame="10", side="IN", x="3.5", y="1.0", **on) == {'FINISHED'}
p = path(label)
h = next(h for h in p["handles"] if h["frame"] == 10)
assert close(h["in"], (3.5, 1.0)), ("the handle where it was dragged", h)
# Picking the handle lets the keys go: still its own motion, not the real one.
assert ops.anim.motion_path_handle_select(frame="10", out="0", **on) == {'FINISHED'}
assert oanim.wm.motion_path_handle_picks() and not any(k["selected"] for k in art.keyframes("points[0].pos"))
p = path(label)
assert p["own_motion"] and close(at(p, 10), (3.5, 2.0)), ("a key handle picked: still its own motion", at(p, 10))
assert ops.anim.motion_path_select(frame="10", **on) == {'FINISHED'}

# --- Outside the tool: only looked at, the real path, the key still picked -------------------------------------
ops.wm.tool_select()
p = path(label)
assert not p["own_motion"], "nothing is edited outside the tool"
assert close(at(p, 10), (6.0, 2.0)), ("the real path", at(p, 10))
ops.wm.tool_motion_path()
assert path(label)["own_motion"], "back in the tool: as it was"

# --- Off in the header: the real path --------------------------------------------------------------------------
doc.path_own_motion = False
p = path(label)
assert not p["own_motion"] and close(at(p, 10), (6.0, 2.0))
doc.path_own_motion = True

# --- Nothing picked again: still its own motion; a dot picked and dragged lands on it ----------------------------
assert ops.anim.motion_path_select(none="1") == {'FINISHED'}
assert path(label)["own_motion"] and close(at(path(label), 10), (3.5, 2.0)), "let go: still its own motion"
assert ops.anim.motion_path_select(frame="7", **on) == {'FINISHED'}
assert oanim.wm.motion_path_dots(), "a dot picked"
# Dragged at once, it lands on the held path (the group held at 2.5 across, not its 3.5 on frame 7).
assert ops.anim.motion_path_move(frame="7", x="3.5", y="1.4", **on) == {'FINISHED'}
k = next(k for k in art.keyframes("points[0].pos") if k["frame"] == 7)
assert close(k["value"][0], 1.0), ("a dot picked: held as shown", k["value"])
assert path(label)["own_motion"], "a dot picked: the same"
assert ops.anim.motion_path_select(none="1") == {'FINISHED'}

# --- A child bone: its own move, the parent's turn held -------------------------------------------------------
doc.frame_current = 0
layers.active = group  # beside the group, which moves nothing in it
ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.select_none()
ops.armature.bone_add(name="hand", head_x="2", head_y="0", tail_x="3", tail_y="0", parent="arm")
doc.frame_current = 10
rig.bone_set("arm", angle=1.0)
rig.bone_set("hand", position=(0.0, 0.5))
doc.frame_current = 5
for l in layers.all():
    l.selected = l.id == rig.id
rig.select_bone("hand")
head = dict(layer=str(rig.id), bone="hand", head="1")
real = real_path("hand head")
assert not real["own_motion"]
own = path("hand head")
assert own["own_motion"], own
f5 = 5 - own["first"]
assert close(own["points"][f5], real["points"][f5]), "where it is now, either way"
d0 = (own["points"][0][0] - own["points"][f5][0], own["points"][0][1] - own["points"][f5][1])
d10 = (own["points"][10 - own["first"]][0] - own["points"][f5][0],
       own["points"][10 - own["first"]][1] - own["points"][f5][1])
assert close(d0, (-d10[0], -d10[1])), ("a straight move either side of now, not the arm's swing", d0, d10)

# Nothing else moves the arm: its own motion is where it really goes.
rig.select_bone("arm")
arm = path("arm tip")
assert arm["own_motion"] and all(close(a, b) for a, b in zip(arm["points"], real_path("arm tip")["points"]))
assert ops.anim.motion_path_select(none="1") == {'FINISHED'}

# --- The bone's own constraints are where it really goes, not its own motion ------------------------------------
# A pupil kept inside a box (Limit to Shape), keyed out past the box's side: its own path goes on past, the curve its
# keys and control points shape (Own Motion off shows where the constraint stops it).
doc.frame_current = 0
layers.active = group
ops.layer.new()
box = layers.active
box.name = "Box"
box.add_curve([(9.0, -1.0), (11.0, -1.0), (11.0, 1.0), (9.0, 1.0)], closed=True, fill=True)
ops.layer.new_bone()
eye = layers.active
ops.armature.bone_add(name="pupil", head_x="10", head_y="0", tail_x="10.2", tail_y="0", parent="")
assert ops.armature.constraint_add(type="LIMIT_SHAPE", bone="pupil") == {'FINISHED'}
# (Not written into its keys: they hold where it is put, the constraint where it shows.)
eye.bone_constraint_set("pupil", 0, shape_layer=box.name, fit="CENTER", affect_transform=False)
doc.frame_current = 10
eye.bone_set("pupil", position=(4.0, 0.0))  # out past the box's left side, to x = 4
doc.frame_current = 5
for l in layers.all():
    l.selected = l.id == eye.id
ops.armature.select_none()
eye.select_bone("pupil")
pupil = dict(layer=str(eye.id), bone="pupil", head="1")
real = real_path("pupil head")
assert not real["own_motion"] and min(q[0] for q in real["points"]) > 8.5, "the box stops it"
own = path("pupil head")
assert own["own_motion"] and close(at(own, 10), (4.0, 0.0)), ("its own keys take it on past", at(own, 10))
assert all(close(q[1], 0.0) for q in own["points"]), "straight out, as keyed"
assert "ghost" not in own and real_path("pupil head")["points"] == real["points"], "the box's stop: Own Motion off"
# A control point on it sits on the path as drawn, not where the box holds the pupil.
assert ops.anim.motion_path_control_add(frame="8", **pupil) == {'FINISHED'}
assert ops.anim.motion_path_select(frame="10", **pupil) == {'FINISHED'}
own = path("pupil head")
assert own["own_motion"]
# A control point picked (the keys let go): still its own motion, the box's stop not shown.
assert ops.anim.motion_path_handle_select(frame="10", control="0", **pupil) == {'FINISHED'}
assert min(c["at"][0] for c in oanim.wm.motion_path_controls() if c["end"] == 10) < 7.0, "on its own path"
picked = path("pupil head")
assert picked["own_motion"] and close(at(picked, 10), (4.0, 0.0)), ("a control point picked", at(picked, 10))
# (The tip's path has it too, a bone's length on.)
c = min((c for c in oanim.wm.motion_path_controls() if c["end"] == 10),
        key=lambda c: abs(c["at"][0] - at(own, c["near_frame"])[0]))
assert c["at"][0] < 7.0 and close(c["at"], at(own, c["near_frame"]), 0.05), (c["at"], at(own, c["near_frame"]))
assert ops.anim.motion_path_select(none="1") == {'FINISHED'}

# --- Saved with the file --------------------------------------------------------------------------------------
# (A dot picked as a new document comes: let go with the old one, whose layer ids the new one's may share.)
assert ops.anim.motion_path_select(frame="7", **pupil) == {'FINISHED'}
assert oanim.wm.motion_path_dots()
doc.path_own_motion = False
folder = tempfile.mkdtemp()
try:
    file = os.path.join(folder, "own.oanim")
    assert ops.wm.save_as_mainfile(filepath=file) == {'FINISHED'}
    ops.wm.read_homefile()
    assert not oanim.wm.motion_path_dots(), "nothing picked on the old document's paths carries over"
    assert oanim.data.document.path_own_motion
    assert ops.wm.open_mainfile(filepath=file) == {'FINISHED'}
    assert not oanim.data.document.path_own_motion
    # (A control point picked as the file is opened again: let go the same.)
    eye = next(l for l in layers.all() if str(l.id) == pupil["layer"])
    layers.active = eye
    eye.select_bone("pupil")
    assert ops.anim.motion_path_handle_select(frame="10", control="0", **pupil) == {'FINISHED'}
    assert oanim.wm.motion_path_handle_picks()
    assert ops.wm.open_mainfile(filepath=file) == {'FINISHED'}
    assert not oanim.wm.motion_path_handle_picks(), "nothing picked on the old document's paths carries over"
finally:
    for name in os.listdir(folder):
        os.remove(os.path.join(folder, name))
    os.rmdir(folder)

print("Motion path own motion tests passed")
