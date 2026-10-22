"""Editing bones as in Blender's armature edit mode: head and tail joints selected on their own,
an active bone, Shift A, E (extrude), Shift D, Ctrl P (Connected / Keep Offset), Alt P, and moving
bone ends on the setup frame.

Run with:  OpenAnimate --background --python tests/test_bone_editing.py
"""

import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def bones():
    return {b["name"]: b for b in rig.bones}


assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
doc.frame_current = 0

# --- Shift A: a new bone at the origin, pointing up ------------------------------------------------
assert wm.event_simulate("A", shift=True, area="Viewport")
assert len(rig.bones) == 1
root = rig.bones[0]
assert close(root["head"], (0, 0)) and close(root["tail"], (0, 0.5))
assert root["selected"] and root["active"] and root["parent"] == ""
name = root["name"]

# --- Joints: a tail on its own; both ends make the bone selected ------------------------------------
rig.select_bone(name, part="TAIL")
b = bones()[name]
assert b["select_tail"] and not b["select_head"] and not b["selected"]
rig.select_bone(name, extend=True, part="HEAD")
assert bones()[name]["selected"], "both ends selected selects the bone"

# --- E: extrude from the selected tip ----------------------------------------------------------------
rig.select_bone(name, part="TAIL")
assert wm.event_simulate("E", area="Viewport")
assert len(rig.bones) == 2
child = next(b for b in rig.bones if b["name"] != name)
assert child["parent"] == name and child["connected"], "extruded bones hang from their tip, connected"
assert close(child["head"], (0, 0.5)) and child["active"]
assert child["select_tail"] and not child["select_head"], "only the new tip is selected, for the move"
assert rig.move_bone_ends((0.5, 0.0))  # the move that follows E
child = bones()[child["name"]]
assert close(child["head"], (0, 0.5)) and close(child["tail"], (0.5, 0.5)), child
assert abs(child["length"] - 0.5) < 1e-4

# G on a top ball moves just that tip, as in Blender's edit mode: the bone stretches, and the
# connected child's head (the same joint) comes along while its tail stays.
rig.select_bone(name, part="TAIL")
assert bones()[child["name"]]["select_head"], "a connected head is the parent's tail"
for key in ("G", "R", "S"):
    assert wm.event_simulate(key, area="Viewport"), key + " starts with just a ball selected"
assert rig.move_bone_ends((0.0, 0.25))
b, c = bones()[name], bones()[child["name"]]
assert close(b["tail"], (0, 0.75)) and close(c["head"], (0, 0.75)) and close(c["tail"], (0.5, 0.5)), (b, c)
assert abs(b["length"] - 0.75) < 1e-4

# G on a bottom ball moves just that end: the bone stretches, and what hangs from it stays.
rig.select_bone(name, part="HEAD")
assert rig.move_bone_ends((-0.25, 0.0))
assert close(bones()[name]["head"], (-0.25, 0)) and close(bones()[name]["tail"], (0, 0.75))
assert close(bones()[child["name"]]["head"], (0, 0.75)) and close(bones()[child["name"]]["tail"], (0.5, 0.5))
assert rig.move_bone_ends((0.25, 0.0))
assert close(bones()[name]["head"], (0, 0))

# The whole bone (its body) moves both ends: a connected child's head comes along, its tail stays.
rig.select_bone(name)
assert rig.move_bone_ends((0.25, 0.0))
assert close(bones()[name]["head"], (0.25, 0)) and close(bones()[name]["tail"], (0.25, 0.75))
assert close(bones()[child["name"]]["head"], (0.25, 0.75)) and close(bones()[child["name"]]["tail"], (0.5, 0.5))
assert rig.move_bone_ends((-0.25, 0.0))

# --- Shift D: duplicate ------------------------------------------------------------------------------
rig.select_bone(child["name"])
assert wm.event_simulate("D", shift=True, area="Viewport")
assert len(rig.bones) == 3
copy = next(b for b in rig.bones if b["name"] not in (name, child["name"]))
assert copy["selected"] and copy["active"] and not bones()[child["name"]]["selected"]
assert copy["parent"] == name and close(copy["tail"], (0.5, 0.5))

# --- Ctrl P: parent the selected bones to the active one ------------------------------------------
assert oanim.ops.armature.bone_add(name="free", head_x="2", head_y="0", tail_x="2", tail_y="1", parent="") == {'FINISHED'}
assert bones()["free"]["parent"] == ""
rig.select_bone("free")
rig.select_bone(name, extend=True)  # the parent last: it becomes active
assert oanim.ops.armature.parent_set(type="OFFSET") == {'FINISHED'}
f = bones()["free"]
assert f["parent"] == name and not f["connected"] and close(f["head"], (2, 0)) and close(f["tail"], (2, 1))
assert oanim.ops.armature.parent_set(type="CONNECTED") == {'FINISHED'}
f = bones()["free"]
assert f["connected"] and close(f["head"], bones()[name]["tail"]), "connected: the base slides onto the tip"
assert abs(f["length"] - 1.0) < 1e-4, "keeping its length and direction"

# Alt P: disconnect, then clear.
rig.select_bone("free")
assert oanim.ops.armature.parent_clear(type="DISCONNECT") == {'FINISHED'}
assert bones()["free"]["parent"] == name and not bones()["free"]["connected"]
assert oanim.ops.armature.parent_clear(type="CLEAR") == {'FINISHED'}
f = bones()["free"]
assert f["parent"] == "" and close(f["head"], bones()[name]["tail"]), "cleared, it stays where it is"

# The menus open from Ctrl P and Alt P.
assert wm.event_simulate("P", ctrl=True, area="Viewport")
assert wm.event_simulate("P", alt=True, area="Viewport")

# --- X with a ball selected deletes that bone ------------------------------------------------------
count = len(rig.bones)
rig.select_bone(copy["name"], part="TAIL")
assert wm.event_simulate("X", area="Viewport")
assert len(rig.bones) == count - 1 and copy["name"] not in bones()
rig.select_bone(name, part="TAIL")  # the parent's tip: its connected child stays
assert bones()[child["name"]]["select_head"]

# --- Away from frame 0, G on a tip only turns the bone toward it (length isn't animated) ------------
before = bones()
assert oanim.ops.armature.select_none() == {'FINISHED'}
rig.select_bone(name, part="TAIL")
doc.frame_current = 12
assert rig.move_bone_ends((0.75, -0.75))
b, c = bones()[name], bones()[child["name"]]
assert close(b["head"], before[name]["head"]) and abs(b["length"] - before[name]["length"]) < 1e-4, "posed: it only turns"
assert abs(math.atan2(b["tail"][1] - b["head"][1], b["tail"][0] - b["head"][0])) < 1e-3, "aimed at the mouse"
doc.frame_current = 0
assert close(bones()[name]["tail"], before[name]["tail"]), "the setup frame is untouched"

# --- Rig edits only on the setup frame --------------------------------------------------------------
doc.frame_current = 10
assert oanim.ops.armature.extrude() == {'CANCELLED'}
assert oanim.ops.armature.bone_primitive_add() == {'CANCELLED'}
doc.frame_current = 0

# --- S on a parent and child together grows both by the same amount ---------------------------------
rig.select_bone(name)
rig.select_bone(child["name"], extend=True)
before = bones()
assert rig.scale_bones(2.0)
after = bones()
for n in (name, child["name"]):
    l0 = ((before[n]["tail"][0] - before[n]["head"][0]) ** 2 + (before[n]["tail"][1] - before[n]["head"][1]) ** 2) ** 0.5
    l1 = ((after[n]["tail"][0] - after[n]["head"][0]) ** 2 + (after[n]["tail"][1] - after[n]["head"][1]) ** 2) ** 0.5
    assert abs(l1 - 2 * l0) < 1e-4, (n, l0, l1)
    assert close(after[n]["scale"], (1, 1)), "frame 0 changes sizes, not scale"
assert rig.scale_bones(0.5, axis="X"), "frame 0 has no axes: it resizes the same every way"
after = bones()
assert close(after[name]["scale"], (1, 1)) and abs(after[name]["length"] - before[name]["length"]) < 1e-4

# R on a parent and child together turns the pair as one, the child no extra, on any frame.
def direction(b):
    return math.atan2(b["tail"][1] - b["head"][1], b["tail"][0] - b["head"][0])


for frame in (0, 7):
    doc.frame_current = frame
    before = bones()
    assert rig.rotate_bones(math.pi / 2)
    after = bones()
    for n in (name, child["name"]):
        turn = (direction(after[n]) - direction(before[n]) + math.pi) % (2 * math.pi) - math.pi
        assert abs(turn - math.pi / 2) < 1e-4, (frame, n, turn)
    assert rig.rotate_bones(-math.pi / 2)
doc.frame_current = 0

# --- Alt G / Alt R / Alt S clear a pose back to the setup pose (not on frame 0) ---------------------
rig.select_bone(name)
setup = bones()[name]
assert not wm.event_simulate("R", alt=True, area="Viewport"), "nothing to clear on the setup frame"
doc.frame_current = 9
assert rig.move_bone_ends((0.3, 0.2))
assert rig.rotate_bones(0.7)
assert rig.scale_bones(1.5)
posed = bones()[name]
assert not close(posed["position"], setup["position"]) and abs(posed["angle"] - setup["angle"]) > 0.1
assert wm.event_simulate("R", alt=True, area="Viewport")
b = bones()[name]
assert abs(b["angle"] - setup["angle"]) < 1e-5 and not close(b["position"], setup["position"])
assert not close(b["scale"], setup["scale"]), "only the rotation goes back"
assert wm.event_simulate("G", alt=True, area="Viewport")
assert close(bones()[name]["position"], setup["position"])
assert wm.event_simulate("S", alt=True, area="Viewport")
assert close(bones()[name]["scale"], setup["scale"])
doc.frame_current = 0
assert close(bones()[name]["head"], setup["head"]) and close(bones()[name]["tail"], setup["tail"])

# --- Posing, S scales each bone along its own axes: X across (thickness), Y along (length) ------------
doc.frame_current = 9
rig.select_bone(name)
b0 = bones()[name]
reach = math.dist(b0["head"], b0["tail"])
assert rig.scale_bones(2.0, axis="Y")
b = bones()[name]
assert close(b["scale"], (1, 2)) and abs(math.dist(b["head"], b["tail"]) - 2 * reach) < 1e-4, "S Y: twice as long"
assert abs(direction(b) - direction(b0)) < 1e-4, "along its own direction"
assert rig.scale_bones(3.0, axis="X")
b = bones()[name]
assert close(b["scale"], (3, 2)) and abs(math.dist(b["head"], b["tail"]) - 2 * reach) < 1e-4, "S X: only thicker"
# S S: squash and stretch, longer and thinner, keeping its area.
assert rig.scale_bones(2.0, squash=True)
assert close(bones()[name]["scale"], (1.5, 4)), bones()[name]["scale"]
assert wm.event_simulate("S", alt=True, area="Viewport")
assert close(bones()[name]["scale"], (1, 1))
doc.frame_current = 0

# --- Posing, G on a connected bone turns it instead of moving it --------------------------------------
assert bones()[child["name"]]["connected"]
rig.select_bone(child["name"])
before = bones()
doc.frame_current = 9
assert rig.move_bone_ends((0.0, 0.4))
after = bones()
c0, c1 = before[child["name"]], after[child["name"]]
assert close(c1["head"], c0["head"]), "it stays on its parent's tip"
assert abs(c1["angle"] - c0["angle"]) > 0.1, "it turned"
assert close(after[name]["tail"], before[name]["tail"]), "the parent doesn't move"
assert wm.event_simulate("R", alt=True, area="Viewport")
doc.frame_current = 0

# --- X mirror editing: the other side (_L / _R) follows -----------------------------------------------
oanim.ops.armature.bone_add(name="arm_L", head_x="1", head_y="1", tail_x="1.5", tail_y="1", parent="")
oanim.ops.armature.bone_add(name="arm_R", head_x="-1", head_y="1", tail_x="-1.5", tail_y="1", parent="")
wm.set_mirror_editing(x=True)
rig.select_bone("arm_L")
assert rig.move_bone_ends((0.25, 0.5))
r = bones()["arm_R"]
assert close(r["head"], (-1.25, 1.5)) and close(r["tail"], (-1.75, 1.5)), r
rig.select_bone("arm_L", part="TAIL")
assert rig.move_bone_ends((0.0, 0.5))
l, r = bones()["arm_L"], bones()["arm_R"]
assert close(r["tail"], (-l["tail"][0], l["tail"][1])) and abs(r["length"] - l["length"]) < 1e-4, (l, r)
doc.frame_current = 12  # posing mirrors too
assert rig.move_bone_ends((0.0, -0.5))
l, r = bones()["arm_L"], bones()["arm_R"]
assert close(r["tail"], (-l["tail"][0], l["tail"][1])), (l, r)
doc.frame_current = 0

# Extrude grows both sides, with matching names; the move mirrors the new tip.
rig.select_bone("arm_L", part="TAIL")
assert oanim.ops.armature.extrude() == {'FINISHED'}
assert "arm_L.001" in bones() and "arm_R.001" in bones()
assert bones()["arm_R.001"]["parent"] == "arm_R" and bones()["arm_R.001"]["connected"]
assert rig.move_bone_ends((0.5, 0.0))
l, r = bones()["arm_L.001"], bones()["arm_R.001"]
assert close(r["tail"], (-l["tail"][0], l["tail"][1])), (l, r)

# Subdivide splits both sides into connected halves; the child moves to the last piece.
rig.select_bone("arm_L")
before = bones()
assert oanim.ops.armature.subdivide() == {'FINISHED'}
after = bones()
assert len(after) == len(before) + 2
assert abs(after["arm_L"]["length"] - before["arm_L"]["length"] / 2) < 1e-4
assert abs(after["arm_R"]["length"] - before["arm_R"]["length"] / 2) < 1e-4
piece = next(b for n, b in after.items() if b["parent"] == "arm_L" and n != "arm_L.001")
assert piece["connected"] and close(piece["tail"], before["arm_L"]["tail"]), piece
assert after["arm_L.001"]["parent"] == piece["name"] and close(after["arm_L.001"]["tail"], before["arm_L.001"]["tail"])

# Delete takes the other side along.
rig.select_bone("arm_L.001")
assert oanim.ops.armature.bone_delete() == {'FINISHED'}
assert "arm_L.001" not in bones() and "arm_R.001" not in bones()
wm.set_mirror_editing()

# --- Box selection and the Transform Bones gizmo (Moho's sz_transform_bones_gizmo) ------------------
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig2 = layers.active
oanim.ops.armature.bone_add(name="p", head_x="0", head_y="0", tail_x="0", tail_y="1", parent="")
oanim.ops.armature.bone_add(name="c", head_x="0", head_y="1", tail_x="1", tail_y="1", parent="p")


def bones2():
    return {b["name"]: b for b in rig2.bones}


# Select tool: the ends inside the box are picked; a bone with both ends inside is selected.
rig2.box_select_bones((-0.1, -0.1), (0.1, 1.1))
b = bones2()
assert b["p"]["selected"] and b["c"]["select_head"] and not b["c"]["selected"], b
# The gizmo's box: any bone it touches is picked whole.
rig2.box_select_bones((0.4, 0.9), (0.6, 1.1), whole=True)
b = bones2()
assert b["c"]["selected"] and not b["p"]["selected"] and not b["p"]["select_head"], b
rig2.box_select_bones((0.4, 0.9), (0.6, 1.1), whole=True, deselect=True)
assert not bones2()["c"]["selected"]
# Posing (away from frame 0), as Blender's pose mode: a bone either end of which is inside is picked
# whole, never one end alone.
frame_before = doc.frame_current
doc.frame_current = 12
rig2.box_select_bones((-0.1, -0.1), (0.1, 0.5))
b = bones2()
assert b["p"]["selected"] and b["p"]["select_head"] and b["p"]["select_tail"], ("p whole, by its head", b["p"])
assert not b["c"]["selected"] and not b["c"]["select_head"], ("c's head (p's tail) wasn't in the box", b["c"])
rig2.box_select_bones((0.9, 0.9), (1.1, 1.1))
b = bones2()
assert b["c"]["selected"] and b["c"]["select_head"] and b["c"]["select_tail"] and not b["p"]["selected"], b
rig2.box_select_bones((0.9, 0.9), (1.1, 1.1), deselect=True)
assert not bones2()["c"]["selected"] and not bones2()["c"]["select_head"]
doc.frame_current = frame_before

assert oanim.ops.wm.tool_transform_bones() == {'FINISHED'}
rig2.select_bone("p")
assert rig2.selected_bone_bounds is not None
# Moving a bone leaves its unselected child where it was.
assert rig2.transform_bones(offset=(1.0, 0.0))
b = bones2()
assert close(b["p"]["head"], (1, 0)) and close(b["p"]["tail"], (1, 1))
assert close(b["c"]["head"], (0, 1)) and close(b["c"]["tail"], (1, 1)), b["c"]
# Turning goes about the middle of the box.
assert rig2.transform_bones(angle=math.pi / 2)
b = bones2()
assert close(b["p"]["head"], (1.5, 0.5)) and close(b["p"]["tail"], (0.5, 0.5)), b["p"]
assert close(b["c"]["head"], (0, 1)) and close(b["c"]["tail"], (1, 1))
assert rig2.transform_bones(angle=-math.pi / 2)
# Scaling on the setup frame changes lengths.
assert rig2.transform_bones(scale=(1.0, 2.0), pivot=(1.0, 0.0))
assert close(bones2()["p"]["tail"], (1, 2)) and abs(bones2()["p"]["length"] - 2.0) < 1e-4
# Posing, it takes a scale instead; the child still stays put.
doc.frame_current = 6
assert rig2.transform_bones(scale=(1.0, 0.5), pivot=(1.0, 0.0))
b = bones2()
assert close(b["p"]["tail"], (1, 1)) and abs(b["p"]["length"] - 2.0) < 1e-4 and abs(b["p"]["scale"][1] - 0.5) < 1e-4
assert close(b["c"]["head"], (0, 1)) and close(b["c"]["tail"], (1, 1)), b["c"]
doc.frame_current = 0
assert close(bones2()["p"]["tail"], (1, 2)), "the setup pose is untouched"
oanim.ops.wm.tool_select()
layers.active = rig

# --- Connected is saved -------------------------------------------------------------------------------
rig_name = rig.name
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_bones_")) / "bones.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in oanim.data.layers if l.name == rig_name)
assert bones()[child["name"]]["connected"] and not bones()["free"]["connected"]

# --- Frame 0 edits the rig as Blender's edit mode: only what is picked moves ---------------------------
# A parent, a child hanging from it (not connected), a connected child, and a pin under the parent.
doc = oanim.data.document
doc.frame_current = 0
oanim.ops.layer.new_bone()
rig = oanim.data.layers.active
oanim.ops.armature.bone_add(name="up", head_x="0", head_y="0", tail_x="0", tail_y="1", parent="")
oanim.ops.armature.bone_add(name="loose", head_x="1", head_y="1", tail_x="2", tail_y="1", parent="up")
oanim.ops.armature.bone_add(name="pinned", head_x="-1", head_y="0.5", tail_x="-0.9", tail_y="0.5", parent="up")
rig.bone_set("pinned", length=0.0)  # a pin: no length
rig.select_bone("up", part="TAIL")
assert wm.event_simulate("E", area="Viewport")
tip = next(b["name"] for b in rig.bones if b["name"] not in ("up", "loose", "pinned"))
assert rig.move_bone_ends((0.0, 1.0))  # the extruded, connected child: (0, 1) to (0, 2)
pin_angle = bones()["pinned"]["angle"] + bones()["up"]["angle"]


def ends(n):
    return bones()[n]["head"], bones()[n]["tail"]


start = {n: ends(n) for n in ("up", "loose", "pinned", tip)}
# G on the parent: it moves; the loose child stays; the connected child's head comes along, not its tail.
rig.select_bone("up")
assert rig.move_bone_ends((0.5, 0.0))
assert close(ends("up")[0], (0.5, 0)) and close(ends("up")[1], (0.5, 1))
assert close(ends("loose")[0], start["loose"][0]) and close(ends("loose")[1], start["loose"][1]), "not connected: stays"
assert close(ends("pinned")[0], start["pinned"][0]), "a pin under it stays too"
assert close(ends(tip)[0], (0.5, 1)) and close(ends(tip)[1], start[tip][1]), "connected: only its head follows"
assert rig.move_bone_ends((-0.5, 0.0))
# R on the parent: the same, and the pin keeps pointing the way it did.
assert rig.rotate_bones(0.5)
assert close(ends("loose")[0], start["loose"][0]) and close(ends("loose")[1], start["loose"][1])
assert close(ends("pinned")[0], start["pinned"][0])
assert abs(bones()["pinned"]["angle"] + bones()["up"]["angle"] - pin_angle) < 1e-4, "the pin didn't turn"
assert close(ends(tip)[1], start[tip][1]) and close(ends(tip)[0], ends("up")[1])
assert rig.rotate_bones(-0.5)
# The Transform Bones gizmo: the same.
assert rig.transform_bones(offset=(0.0, 0.5))
assert close(ends("loose")[0], start["loose"][0]) and close(ends(tip)[1], start[tip][1])
assert close(ends(tip)[0], ends("up")[1])
# Posing (any other frame) still carries children along, as Moho and Blender's pose mode do.
doc.frame_current = 5
before = ends("loose")
assert rig.move_bone_ends((0.5, 0.0))
assert close(ends("loose")[0], (before[0][0] + 0.5, before[0][1])), "posed: children come along"
doc.frame_current = 0

print("Bone editing tests passed")
