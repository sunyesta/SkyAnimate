"""Editing a path quickly. A layer's origin is placed with no copy of the document (its own transform given the values,
layer_world_transform_with); a point by itself, not with every point of its layer (layer_point_shown): weighted to
bones, or bent by a deform. Each comes out as working everything out on copies makes it: dots, key handles and control
points. A dot moved is placed on the document held as shown, not on another copy of it; a timing edit on a drawing
keyed whole fits its timing once for all its channels.

Run with:  OpenAnimate --background --python tests/test_motion_path_edit_fast.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def grab():
    """Every path's dots, its keys' handles and its control points, as numbers."""
    out = []
    for p in wm.motion_paths():
        out.append((p["label"], [tuple(q) for q in p["points"]],
                    [(h["frame"], h["in"], h["out"]) for h in p["handles"]]))
    out.append([(c["layer"], c["end"], c["index"], c["at"], c["curve"], c["in_tip"], c["out_tip"])
                for c in wm.motion_path_controls()])
    return out


def close(a, b, eps=1e-4):
    if a is None or b is None:
        return a is b
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    if isinstance(a, str):
        return a == b
    return abs(a - b) < eps


def same_ways(what):
    """The shortcuts against working everything out on copies."""
    quick = grab()
    wm.motion_path_own_from_keys(False)
    worked = grab()
    wm.motion_path_own_from_keys(True)
    assert close(quick, worked), (what, quick, worked)
    return quick


def only(layer, points=()):
    for other in layers.all():
        other.selected = other.id == layer.id
    layers.active = layer
    layer.select_points(list(points))


ops.wm.read_homefile()
anchor = layers.active
doc.auto_key = False
doc.motion_paths = True
doc.path_all = True
doc.frame_end = 40
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert doc.path_own_motion

# --- A layer's origin: turned, scaled and moved in a group that turns ----------------------------------------------
ops.layer.new()
ball = layers.active
ball.add_curve([(0.0, 0.0), (0.5, 0.0), (0.5, 0.5)])
ball.key_whole_drawing = False
only(ball)
assert ops.layer.group() == {'FINISHED'}
group = layers.active
doc.auto_key = True
doc.frame_current = 40
group.rotation = 25.0
group.location_x = 3.0
doc.frame_current = 10
ball.location_x, ball.location_y = 0.0, 0.0
doc.frame_current = 30
ball.location_x, ball.location_y = 4.0, 1.0
ball.rotation = 15.0
ball.scale_x = 1.6
doc.frame_current = 0
doc.auto_key = False
args = dict(layer=str(ball.id))
for f in (14, 22):
    assert ops.anim.motion_path_control_add(frame=str(f), **args) == {'FINISHED'}
only(ball)
assert [p["label"] for p in wm.motion_paths()] == [ball.name], [p["label"] for p in wm.motion_paths()]
for f in (0, 12, 20, 33):
    doc.frame_current = f
    same_ways("a layer, frame %d" % f)
wm.motion_path_places_clear()
grab()
assert wm.motion_path_places()["copies"] == 0, ("a layer's origin placed with no copy", wm.motion_path_places())

# A dot moved: the document held as shown is copied once, and placed on as it is.
doc.frame_current = 12
dot = wm.motion_paths()[0]["points"][20 - wm.motion_paths()[0]["first"]]
wm.motion_path_places_clear()
assert ops.anim.motion_path_move(frame="20", x=str(dot[0] + 0.3), y=str(dot[1]), **args) == {'FINISHED'}
used = wm.motion_path_places()
assert used["copies"] == 1, ("held as shown, once", used)
moved = wm.motion_paths()[0]["points"][20 - wm.motion_paths()[0]["first"]]
assert close(moved, (dot[0] + 0.3, dot[1]), 1e-3), (moved, dot)
same_ways("a layer, after a move")

# --- A point weighted to bones that turn -------------------------------------------------------------------------
doc.frame_current = 0
layers.active = anchor
ops.layer.new()
art = layers.active
ops.layer.new_bone()
rig = layers.active
layers.active = art
ops.layer.move_to_group(group=str(rig.id))
layers.active = rig
ops.armature.bone_add(name="upper", head_x="0", head_y="0", tail_x="0.5", tail_y="0")
ops.armature.bone_add(name="lower", head_x="0.5", head_y="0", tail_x="1", tail_y="0")
layers.active = art
art.add_curve([(0.1, 0.0), (0.3, 0.0), (0.7, 0.0), (0.9, 0.0)])
art.key_whole_drawing = False
ops.mesh.select_none()
assert ops.weight.automatic() == {'FINISHED'}
assert any(w for w in art.weights), art.weights
doc.auto_key = True
doc.frame_current = 20
rig.bone_set("lower", angle=40.0)
rig.bone_set("upper", angle=-15.0)
doc.frame_current = 10
art.set_point(2, (0.7, 0.1))
doc.frame_current = 30
art.set_point(2, (0.8, -0.3))
doc.frame_current = 0
doc.auto_key = False
only(art, [2])
labels = [p["label"] for p in wm.motion_paths()]
assert labels == [art.name + " point 2"], labels
for f in (0, 15, 20, 26):
    doc.frame_current = f
    same_ways("a weighted point, frame %d" % f)

# --- A point bent by a deform ------------------------------------------------------------------------------------
layers.active = anchor
ops.layer.new()
bent = layers.active
bent.add_curve([(-0.55, -0.3), (-0.2, 0.35), (0.25, -0.15), (0.6, 0.3)])
bent.key_whole_drawing = False
layers.active = anchor
ops.layer.new()
cage = layers.active
assert cage.add_primitive("BOX", [(-1, -1), (1, 1)])
layers.active = bent
assert ops.object.modifier_add(type="MESH_DEFORM") == {'FINISHED'}
bent.modifier_set(len(bent.modifiers) - 1, curve_method="SMOOTH", cage_method="MEAN_VALUE")
bent.modifier_set(len(bent.modifiers) - 1, layer=cage.name, shape=cage.shapes[0][0])
doc.auto_key = True
doc.frame_current = 20
for i, p in enumerate(cage.points):
    cage.set_point(i, (p[0] * 1.3 + 0.2 * p[1] + 0.1, p[1] * 0.9 + (0.12 if p[0] > 0.5 else 0.0)))
doc.frame_current = 10
bent.set_point(1, (-0.2, 0.3))
doc.frame_current = 30
bent.set_point(1, (0.1, 0.5))
doc.frame_current = 0
doc.auto_key = False
only(bent, [1])
labels = [p["label"] for p in wm.motion_paths()]
assert labels == [bent.name + " point 1"], labels
for f in (5, 20, 25):
    doc.frame_current = f
    same_ways("a bent point, frame %d" % f)

# --- A timing edit on a drawing keyed whole: its timing fitted once, not once a channel ----------------------------
layers.active = anchor
ops.layer.new()
many = layers.active
many.add_curve([(0.1 * i, 0.05 * (i % 3)) for i in range(12)])
assert many.key_whole_drawing
doc.auto_key = True
doc.frame_current = 10
many.set_point(0, (0.0, 0.2))
doc.frame_current = 30
many.set_point(0, (1.0, 0.6))
doc.frame_current = 0
doc.auto_key = False
only(many, [0])
doc.frame_current = 20
before = many.points[0]
wm.motion_path_places_clear()
assert ops.anim.motion_path_slide(frame="20", progress="0.3", layer=str(many.id), drawing="1") == {'FINISHED'}
fits = wm.motion_path_places()["fits"]
assert 1 <= fits <= 4, ("one fit for the drawing's channels (a dozen points' worth)", fits)
assert not close(many.points[0], before), ("the point slid", many.points[0], before)

print("Motion path edit fast tests passed")
