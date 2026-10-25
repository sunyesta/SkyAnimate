"""Alt G, Alt R and Alt S (Clear Location, Rotation, Scale) leave the ViewportActionPanel as moves do:
Amount (part of the way back), X and Y (Clear Location), and for bones Move Only Bones and Move Without
Children, kept with the moves' choices until the frame or the action changes.

Run with:  OpenAnimate --background --python tests/test_clear_options.py
"""

import oanim

layers = oanim.data.layers
doc = oanim.data.document
ops = oanim.ops
wm = oanim.wm
doc.frame_current = 0


def close(a, b, eps=1e-4):
    if isinstance(a, (int, float)):
        return abs(a - b) < eps
    return all(abs(x - y) < eps for x, y in zip(a, b))


assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="prop", head_x="0.5", head_y="0.5", tail_x="0.5", tail_y="1", parent="arm")
ops.layer.new()
art = layers.active
art.add_curve([(0.2, 0.1), (0.6, 0.2), (0.9, -0.1)])
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
art = next(l for l in layers.all() if l.id == art.id)


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


def pick(name):
    layers.active = rig
    ops.armature.select_none()
    rig.select_bone(name)


doc.frame_current = 10
rig.bone_set("arm", angle=0.8, position=(0.3, 0.2))
pick("arm")
ops.anim.keyframe_insert()
posed = bone("arm")

# Alt R over the viewport: the bone turns back, and the panel can adjust it.
assert wm.event_simulate("R", alt=True, area="Viewport")
assert close(bone("arm")["angle"], bone("arm")["setup_angle"])
last = wm.last_operation()
assert last and last[0] == "armature.clear_rotation" and last[2] == "Viewport", last
assert last[1]["amount"] == "1" and last[1]["only_bones"] == "0" and last[1]["without_children"] == "0", last
assert wm.redo_last(amount="0.5") == "FINISHED"
assert close(bone("arm")["angle"], (posed["angle"] + posed["setup_angle"]) / 2), bone("arm")
ops.ed.undo()
assert close(bone("arm")["angle"], posed["angle"])

# Move Without Children: the prop stays where it shows.
prop_head = bone("prop")["head"]
assert wm.event_simulate("R", alt=True, area="Viewport")
assert wm.redo_last(without_children="1") == "FINISHED"
assert close(bone("prop")["head"], prop_head), (bone("prop")["head"], prop_head)
assert wm.move_options()["without_children"], "kept for the next moves"
ops.ed.undo()

# Move Only Bones: the drawing stays where it shows; the choice shows on the next clear.
drawn = art.evaluated()[0]
assert wm.event_simulate("R", alt=True, area="Viewport")
assert wm.last_operation()[1]["without_children"] == "1", "the kept choice shows in the panel"
assert wm.redo_last(only_bones="1", without_children="0") == "FINISHED"
assert all(close(a, b) for a, b in zip(art.evaluated()[0], drawn))
ops.ed.undo()

# Alt G: X only leaves its move up; Alt S has Amount too.
assert wm.event_simulate("G", alt=True, area="Viewport")
assert wm.last_operation()[0] == "armature.clear_location"
assert close(bone("arm")["position"], bone("arm")["setup_position"])
assert wm.redo_last(y="0") == "FINISHED"
p, s = bone("arm")["position"], bone("arm")["setup_position"]
assert close(p[0], s[0]) and not close(p[1], s[1]), (p, s)
ops.ed.undo()
assert ops.armature.resize(scale="1.6") == {'FINISHED'}
grown = bone("arm")["scale"]
assert not close(grown, (1.0, 1.0)), grown
assert wm.event_simulate("S", alt=True, area="Viewport")
assert wm.last_operation()[0] == "armature.clear_scale"
assert close(bone("arm")["scale"], (1.0, 1.0))
assert wm.redo_last(amount="0.5") == "FINISHED"
assert close(bone("arm")["scale"], [(g + 1.0) / 2 for g in grown]), (bone("arm")["scale"], grown)

# Points (no bone picked): Alt G clears the point's move, with Amount and X / Y.
layers.active = rig
ops.armature.select_none()
doc.auto_key = True
ops.layer.new()
loose = layers.active
doc.frame_current = 0
loose.add_curve([(0.0, 0.0), (1.0, 0.0)])
doc.frame_current = 10
loose.select_points(range(loose.point_count), False)
loose.select_points([1])
assert ops.mesh.translate(move_x="0.4", move_y="0.2") == {'FINISHED'}
assert close(loose.points[1][:2], (1.4, 0.2))
assert wm.event_simulate("G", alt=True, area="Viewport")
assert wm.last_operation()[0] == "mesh.clear_location", wm.last_operation()
assert close(loose.points[1][:2], (1.0, 0.0))
assert wm.redo_last(amount="0.25") == "FINISHED"
assert close(loose.points[1][:2], (1.3, 0.15)), loose.points[1]
assert wm.redo_last(amount="1", x="0") == "FINISHED"
assert close(loose.points[1][:2], (1.4, 0.0)), loose.points[1]

# Alt S on points (Scale Radius) ends as mesh.radius_scale, which the panel adjusts too.
r = loose.radii[1]
assert ops.mesh.radius_scale(factor="0.5") == {'FINISHED'}
assert close(loose.radii[1], r * 0.5)
assert wm.redo_last(factor="0.25") == "FINISHED"
assert close(loose.radii[1], r * 0.25), loose.radii

# The choices go off when the frame changes, as the moves' do.
doc.frame_current = 12
assert not wm.move_options()["without_children"]

print("clear_options: ok")
