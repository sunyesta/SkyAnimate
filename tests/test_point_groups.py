"""Point groups (Blender's vertex groups): named sets of a layer's points, each with a weight (0-1),
kept through edits by the points' ids, saved with the file.

Run with:  OpenAnimate --background --python tests/test_point_groups.py
"""

import os
import tempfile

import oanim

layers = oanim.data.layers
doc = oanim.data.document
doc.frame_current = 0
art = layers.active
art.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0), (3.0, 0.0)])


def pick(*indices):
    art.select_points(range(art.point_count), False)
    art.select_points(list(indices))


def groups():
    return art.point_groups


# Add: "Group", then "Group.001", the new one active.
assert oanim.ops.mesh.point_group_add() == {'FINISHED'}
assert oanim.ops.mesh.point_group_add(name="Corners") == {'FINISHED'}
assert oanim.ops.mesh.point_group_add() == {'FINISHED'}
assert [g[0] for g in groups()] == ["Group", "Corners", "Group.001"] and art.active_point_group == 2

# Rename (a name taken gets a number), make one active.
assert oanim.ops.mesh.point_group_set(index="2", name="Corners") == {'FINISHED'}
assert groups()[2][0] == "Corners.001"
assert oanim.ops.mesh.point_group_set(group="Corners", active="1") == {'FINISHED'}
assert art.active_point_group == 1

# Assign with a weight, Remove, Select, Deselect.
pick(0, 3)
assert oanim.ops.mesh.point_group_assign(weight="0.5") == {'FINISHED'}
pick(1)
assert oanim.ops.mesh.point_group_assign() == {'FINISHED'}
assert groups()[1][1] == {0: 0.5, 1: 1.0, 3: 0.5}, groups()[1]
pick(0)
assert oanim.ops.mesh.point_group_unassign() == {'FINISHED'}
assert groups()[1][1] == {1: 1.0, 3: 0.5}
pick()
assert oanim.ops.mesh.point_group_assign() == {'CANCELLED'}, "nothing selected"
assert oanim.ops.mesh.point_group_select() == {'FINISHED'}
assert [p[2] for p in art.points] == [False, True, False, True]
pick(0, 1, 2, 3)
assert oanim.ops.mesh.point_group_deselect() == {'FINISHED'}
assert [p[2] for p in art.points] == [True, False, True, False]

# Through edits. Split: a point between two of the group's is in it, weighted between them.
pick(1, 2)
oanim.ops.mesh.point_group_set(group="Corners", active="1")
oanim.ops.mesh.point_group_assign(weight="0.2")  # points 1 and 2 at 0.2, point 3 at 0.5
new = art.split_segment(0, 2, 0.5)  # between points 2 (0.2) and 3 (0.5)
members = groups()[1][1]
assert abs(members.get(new, -1) - 0.35) < 1e-4, (new, members)
assert new not in groups()[0][1], "not in a group its neighbours aren't in"

# Duplicate: the copies are in the groups too.
before = art.point_count
pick(0, 1)
assert oanim.ops.mesh.duplicate_move() == {'FINISHED'}
assert art.point_count == before + 2
assert art.point_count - 1 in groups()[1][1] or art.point_count - 2 in groups()[1][1], ("a copy of point 1", groups())

# Delete: the points gone leave their groups.
pick(1)
assert oanim.ops.mesh.delete() == {'FINISHED'}
assert all(i < art.point_count for i in groups()[1][1]), groups()

# Saved and opened.
snapshot = groups()
path = os.path.join(tempfile.mkdtemp(), "groups.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
art = layers.active
assert [(n, {i: round(w, 4) for i, w in m.items()}, k) for n, m, k in groups()] == \
    [(n, {i: round(w, 4) for i, w in m.items()}, k) for n, m, k in snapshot], (groups(), snapshot)
assert art.active_point_group == 1

# Remove a group (its points stay), and undo brings it back.
count = art.point_count
assert oanim.ops.mesh.point_group_remove() == {'FINISHED'}
assert [g[0] for g in groups()] == ["Group", "Corners.001"] and art.point_count == count
oanim.ops.ed.undo()
assert [g[0] for g in groups()] == ["Group", "Corners", "Corners.001"]

# Locked: Assign and Remove leave it alone (saved with the file).
assert oanim.ops.mesh.point_group_set(group="Corners", active="1", lock="1") == {'FINISHED'}
assert groups()[1][2] is True
pick(2)
assert oanim.ops.mesh.point_group_assign() == {'CANCELLED'}
assert oanim.ops.mesh.point_group_unassign() == {'CANCELLED'}
# Remove from All Groups skips locked ones.
oanim.ops.mesh.point_group_set(group="Group", active="1")
oanim.ops.mesh.point_group_assign()
assert 2 in groups()[0][1]
assert oanim.ops.mesh.point_group_unassign_all() == {'FINISHED'}
assert 2 not in groups()[0][1] and 2 in groups()[1][1], "the locked group keeps it"

# Move, Copy, Sort by Name, Lock / Unlock All, Delete All (Unlocked).
oanim.ops.mesh.point_group_set(group="Corners.001", active="1")
assert oanim.ops.mesh.point_group_move(direction="-1") == {'FINISHED'}
assert [g[0] for g in groups()] == ["Group", "Corners.001", "Corners"] and art.active_point_group == 1
assert oanim.ops.mesh.point_group_move(direction="-1") == {'FINISHED'}
assert oanim.ops.mesh.point_group_move(direction="-1") == {'CANCELLED'}, "already first"
oanim.ops.mesh.point_group_set(group="Corners", active="1")
assert oanim.ops.mesh.point_group_copy() == {'FINISHED'}
assert groups()[art.active_point_group][0] == "Corners_copy"
assert groups()[art.active_point_group][1] == groups()[art.active_point_group - 1][1], "same points and weights"
assert groups()[art.active_point_group][2] is False, "a copy starts unlocked"
assert oanim.ops.mesh.point_group_sort() == {'FINISHED'}
assert [g[0] for g in groups()] == sorted(g[0] for g in groups())
assert groups()[art.active_point_group][0] == "Corners_copy", "the same group stays active"
assert oanim.ops.mesh.point_group_lock_all(lock="0") == {'FINISHED'}
assert not any(g[2] for g in groups())
oanim.ops.mesh.point_group_set(group="Corners", lock="1")
assert oanim.ops.mesh.point_group_remove_all(unlocked="1") == {'FINISHED'}
assert [g[0] for g in groups()] == ["Corners"]
assert oanim.ops.mesh.point_group_remove_all() == {'FINISHED'}
assert groups() == []

print("point_groups: ok")
