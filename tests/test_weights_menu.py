"""The weight tools' Weights menu: Remove All Weights takes the selected points out of every bone (they
follow none), Clean takes points out of bones that move them less than a limit, the others sharing it.

Run with:  OpenAnimate --background --python tests/test_weights_menu.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="b", head_x="0", head_y="1", tail_x="1", tail_y="1", parent="")
ops.layer.new()
art = layers.active
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
art = next(l for l in layers.all() if l.id == art.id)
art.add_curve([(0.2, 0.1), (0.5, 0.5), (0.8, 0.9)])
layers.active = art
ops.wm.tool_weight_select() if hasattr(ops.wm, "tool_weight_select") else None
ops.mesh.select_all()
rig.select_bone("a")
rig.select_bone("b", extend=True)
assert ops.weight.automatic() == {'FINISHED'}
assert all(art.weights[i] for i in range(3)), art.weights


def bones_of(i):
    return {b: w for b, w in art.weights[i].items() if w > 0}


# Remove All Weights: only the selected points, and nothing without a selection.
ops.mesh.select_none()
assert ops.weight.remove_all() == {'CANCELLED'}, "nothing selected: nothing happens"
art.select_points([1])
assert ops.weight.remove_all() == {'FINISHED'}
assert bones_of(1) == {}, art.weights[1]
assert bones_of(0) and bones_of(2), "the others keep theirs"
# Unweighted, a point stays put when the bones move.
doc.frame_current = 10
rig.bone_set("a", angle=0.7)
rig.bone_set("b", angle=-0.7)
points = art.shown()[0]
assert abs(points[1][0] - 0.5) < 1e-5 and abs(points[1][1] - 0.5) < 1e-5, points[1]
doc.frame_current = 0

# Clean: a 1% weight goes, the bone left takes it (the point's total stays as it was).
ops.mesh.select_none()
art.select_points([0])
rig.select_bone("a")
assert ops.weight.assign(weight="1.0") == {'FINISHED'}
rig.select_bone("b")
oanim.wm.set_weight_normalize(False) if hasattr(oanim.wm, "set_weight_normalize") else None
assert ops.weight.assign(weight="0.01") == {'FINISHED'}
w = bones_of(0)
assert "b" in w and w["b"] < 0.02, w
total = sum(w.values())
ops.mesh.select_none()
assert ops.weight.clean() == {'FINISHED'}, "none selected: every point"
w = bones_of(0)
assert "b" not in w and abs(sum(w.values()) - total) < 1e-5, (w, total)
assert ops.weight.clean() == {'CANCELLED'}, "nothing left to clean"
# A higher limit takes more, but a point always keeps its strongest bone.
assert all(bones_of(i) for i in (0, 2))
ops.weight.clean(threshold="1.0")
assert len(bones_of(2)) == 1, art.weights[2]

# Keep Animation from a frame other than 0: the point stays where it shows on the keyed frame (10).
doc.frame_current = 10
shown_before = art.shown()[0][2]
ops.mesh.select_none()
art.select_points([2])
assert ops.weight.remove_all(keep_animation="1") == {'FINISHED'}
assert bones_of(2) == {}
doc.frame_current = 11
doc.frame_current = 10
now = art.shown()[0][2]
assert abs(now[0] - shown_before[0]) < 1e-4 and abs(now[1] - shown_before[1]) < 1e-4, ("kept", now, shown_before)

# A detail point is bent by its own bones too: Keep Animation puts it back as well.
doc.frame_current = 0
ops.layer.new()
fine = layers.active
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
fine = next(l for l in layers.all() if l.id == fine.id)
layers.active = fine
fine.add_curve([(0.0, 0.2), (1.0, 0.2), (1.0, 0.8), (0.0, 0.8)], closed=True, fill=True)
ops.mesh.select_all()
assert ops.mesh.detail_level_set(level="1") == {'FINISHED'}
d = fine.split_segment(0, 0, 0.5)
assert fine.detail["levels"][d] == 1
ops.mesh.select_all()
rig.select_bone("a")
rig.select_bone("b", extend=True)
assert ops.weight.automatic() == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("a", angle=0.9)
doc.frame_current = 11
doc.frame_current = 10
detail_before = fine.shown()[0][d]
ops.mesh.select_none()
fine.select_points([d])
assert ops.weight.remove_all(keep_animation="1") == {'FINISHED'}
doc.frame_current = 11
doc.frame_current = 10
now = fine.shown()[0][d]
assert abs(now[0] - detail_before[0]) < 1e-3 and abs(now[1] - detail_before[1]) < 1e-3, ("detail kept", now, detail_before)

# Assign has Weight and Keep Animation in the Adjust Last Operation panel: ticking Keep Animation there
# runs it again keeping the point where it showed.
doc.frame_current = 10
layers.active = art
for l in layers.all():
    l.select = l.id == art.id
rig.select_bone("b")
assert ops.mesh.select_all() == {'FINISHED'}  # an undo step holding this selection, as clicking makes
was = art.shown()[0][0]
assert ops.weight.assign(weight="1.0", keep_animation="0") == {'FINISHED'}
moved = art.shown()[0][0]
assert abs(moved[0] - was[0]) + abs(moved[1] - was[1]) > 1e-3, "without Keep Animation it moves"
idname, args, area = oanim.wm.last_operation()
assert idname == "weight.assign" and args["keep_animation"] == "0" and abs(float(args["weight"]) - 1.0) < 1e-6, args
assert oanim.wm.redo_last(keep_animation="1") == "FINISHED"
doc.frame_current = 11
doc.frame_current = 10
now = art.shown()[0][0]
assert abs(now[0] - was[0]) < 1e-4 and abs(now[1] - was[1]) < 1e-4, ("kept from the panel", now, was)

print("weights menu: ok")
