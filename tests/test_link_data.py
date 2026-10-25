"""Ctrl L, Blender's Link/Transfer Data menu. On points: Link Materials, the active shape's material (or its
own look) given to every shape with at least one point selected, on every layer being edited. On a bone
layer: Copy Constraints, the active bone's constraints added to the other selected bones.

Run with:  OpenAnimate --background --python tests/test_link_data.py
"""

import oanim

layers = oanim.data.layers
doc = oanim.data.document
ops = oanim.ops
doc.frame_current = 0


def close(a, b):
    return all(abs(x - y) < 1e-5 for x, y in zip(a, b))


# --- Link Materials -----------------------------------------------------------------------------------
art = layers.active
for x in (0, 2, 4, 6):
    art.add_curve([(x, 0), (x + 1, 0), (x + 1, 1), (x, 1)], closed=True, fill=True)
art.set_shape_style(0, fill=(0.2, 0.6, 0.3, 1.0), line_width=0.02)
ops.mesh.select_none()
art.select_points([0, 1, 2, 3])
assert ops.material.new(name="Skin") == {'FINISHED'}

# The menu: Ctrl L over the viewport asks for it.
ops.mesh.select_none()
art.select_points([5])
assert oanim.wm.event_simulate("L", ctrl=True, area="Viewport"), "Ctrl L opens Link/Transfer Data"

# One point of shape 1 and one of shape 3 selected, shape 0 active: both take Skin; shape 2 doesn't.
art.select_points([13])
art.set_active_shape(0)
assert ops.material.link_selected() == {'FINISHED'}
assert [art.shape_material(i) for i in range(4)] == ["Skin", "Skin", "", "Skin"], [art.shape_material(i) for i in range(4)]
assert close(art.shape_style(3)["fill"], (0.2, 0.6, 0.3, 1.0))
assert oanim.wm.undo_history()[0][-1] == "Link Skin"
assert ops.material.link_selected() == {'CANCELLED'}, "they have it already"

# Across the layers being edited too.
ops.layer.new()
other = layers.active
other.add_curve([(0, 3), (1, 3), (1, 4)], closed=True, fill=True)
art.selected = True
layers.active = other
other.select_points([0])
art.set_active_shape(0)
assert ops.material.link_selected() == {'FINISHED'}
assert other.shape_material(0) == "Skin"

# A shape with a look of its own passes that on (unlinking the others).
layers.active = art
other.selected = False
ops.mesh.select_none()
art.set_shape_style(2, fill=(0.1, 0.1, 0.9, 1.0))
art.select_points([1])  # shape 0
art.set_active_shape(2)
assert ops.material.link_selected() == {'FINISHED'}
assert art.shape_material(0) == "" and close(art.shape_style(0)["fill"], (0.1, 0.1, 0.9, 1.0))
assert oanim.wm.undo_history()[0][-1] == "Link Look"
ops.ed.undo()
assert art.shape_material(0) == "Skin"

# --- Copy Constraints ---------------------------------------------------------------------------------
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
for name, y in (("a", 0), ("b", 1), ("c", 2), ("target", 3)):
    ops.armature.bone_add(name=name, head_x="0", head_y=str(y), tail_x="1", tail_y=str(y), parent="")
rig.select_bone("a")
assert ops.armature.constraint_add(type="LIMIT_ANGLE") == {'FINISHED'}
assert ops.armature.constraint_add(type="DYNAMICS") == {'FINISHED'}
assert ops.armature.constraint_add(type="COPY_ANGLE") == {'FINISHED'}


def kinds(name):
    return [c["type"] for c in rig.bone_constraints(name)]


# b gets them all; c's own constraints (an IK, driven) are replaced by them.
rig.select_bone("c")
assert ops.armature.constraint_add(type="IK") == {'FINISHED'}
assert ops.anim.driver_add(layer=str(rig.id), path='bones["c"].constraints[0].influence', expression="0.5") == {'FINISHED'}
own_drivers = len(oanim.data.drivers)
rig.select_bone("b")
rig.select_bone("c", extend=True)
rig.select_bone("a", extend=True)  # active: the last clicked
assert oanim.wm.event_simulate("L", ctrl=True, area="Viewport"), "Ctrl L on bones too"
assert ops.armature.constraints_copy() == {'FINISHED'}
assert kinds("b") == ["LIMIT_ANGLE", "COPY_ANGLE", "DYNAMICS"], kinds("b")
assert kinds("c") == ["LIMIT_ANGLE", "COPY_ANGLE", "DYNAMICS"], ("replaced, not added to", kinds("c"))
assert len(oanim.data.drivers) == own_drivers - 1, "the drivers on c's old constraints go with them"
assert kinds("a") == ["LIMIT_ANGLE", "COPY_ANGLE", "DYNAMICS"], "the active one as it was"
assert kinds("target") == [], "not selected"
ops.ed.undo()
assert kinds("b") == [] and kinds("c") == ["IK"] and len(oanim.data.drivers) == own_drivers

# Nothing else selected, or nothing to copy: nothing happens.
rig.select_bone("a")
assert ops.armature.constraints_copy() == {'CANCELLED'}
rig.select_bone("target")
rig.select_bone("b", extend=True)
assert ops.armature.constraints_copy() == {'CANCELLED'}, "b has none"

# --- Copy Constraints with Drivers: the copies' settings follow the original's -----------------------
rig.select_bone("b")
rig.select_bone("c", extend=True)
rig.select_bone("a", extend=True)
drivers_before = len(oanim.data.drivers)
assert ops.armature.constraints_copy(drivers="1") == {'FINISHED'}
assert oanim.wm.undo_history()[0][-1] == "Copy Constraints with Drivers"
assert kinds("b") == ["LIMIT_ANGLE", "COPY_ANGLE", "DYNAMICS"], kinds("b")
targets = [d["target"]["path"] for d in oanim.data.drivers[drivers_before:]]
assert 'bones["b"].constraints[0].min_angle' in targets and 'bones["c"].constraints[1].factor' in targets, targets
assert 'bones["b"].constraints[2].angle_spring.bounce' in targets, "the Dynamics settings too"
assert 'bones["c"].constraints[2].weight' in targets, "c's copied Dynamics is driven too"
assert not any(d["target"]["path"] == 'bones["c"].constraints[0].influence' and d["expression"] == "0.5"
               for d in oanim.data.drivers), "c's old driver is gone"
assert all(d["error"] == "" for d in oanim.data.drivers), [d["error"] for d in oanim.data.drivers]


def setting(bone, index, key):
    return rig.bone_constraints(bone)[index][key]


# Changing the original changes the copies, on any frame.
for frame in (5, 0):
    doc.frame_current = frame
    rig.bone_constraint_set("a", 0, min_angle=-0.3, influence=0.4)
    rig.bone_constraint_set("a", 1, factor=0.25)
    doc.frame_current = frame + 1
    doc.frame_current = frame
    for bone in ("b", "c"):
        assert abs(setting(bone, 0, "min_angle") + 0.3) < 1e-5, (frame, bone, rig.bone_constraints(bone)[0])
        assert abs(setting(bone, 0, "influence") - 0.4) < 1e-5, (frame, bone, rig.bone_constraints(bone)[0])
        assert abs(setting(bone, 1, "factor") - 0.25) < 1e-5, (frame, bone)
    rig.bone_constraint_set("a", 0, min_angle=-1.0, influence=1.0)
ops.ed.undo()
assert len(oanim.data.drivers) == drivers_before and kinds("b") == []

print("link_data: ok")
