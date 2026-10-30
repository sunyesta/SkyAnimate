"""Links between layers stay relative when they are copied together: Duplicate, Symmetrize and Copy/Paste of a
group (or of several picked layers) point every link inside what was copied at the copies, not the originals:
references, masks, Draw Order, bones' custom shapes, Limit to Shape constraints, and modifiers' layers and
shapes (a Curve Deform's shape, a Boolean's targets). Symmetrize keeps a reference whose original goes across
with it (showing the mirrored copy, what it moved mirrored too) and flips a Mirror modifier's centre.

Run with:  OpenAnimate --background --python tests/test_relative_links.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0


def find(name, under=None):
    pool = walk(under) if under else layers.all()
    return next((l for l in pool if l.name == name), None)


def walk(layer):
    out = []
    for c in layer.children:
        out.append(c)
        out.extend(walk(c))
    return out


def new(name, points, group):
    layers.active = group
    ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve(points, closed=True, fill=True)
    assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
    return find(name, group)


def remove(*doomed):
    """Delete the copies (rather than undo, which could also step back past the rig's own setup)."""
    for l in doomed:
        layers.active = l
        assert ops.layer.delete() == {'FINISHED'}


def modifier(layer, kind):
    return next(m for m in layer.modifiers if m["type"] == kind)


# --- A rig on the left whose layers name each other (in a group named for its side, so it goes across whole) ---
assert ops.layer.new_group() == {'FINISHED'}
arm = layers.active
arm.name = "Arm_L"
arm.location_x = 3.0
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
rig.name = "Rig_L"
assert ops.layer.move_to_group(group=str(arm.id)) == {'FINISHED'}
rig = find("Rig_L")
layers.active = rig
assert ops.armature.bone_add(name="arm_L", head_x="0.2", head_y="0", tail_x="0.8", tail_y="0.3", parent="") == {'FINISHED'}
body = new("Body_L", [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], rig)
cage = new("Cage_L", [(-0.2, -0.2), (1.2, -0.2), (1.2, 1.2), (-0.2, 1.2)], rig)
ear = new("Ear_L", [(0.8, 0.8), (1.4, 0.9), (1.2, 1.4)], rig)
shape = new("Shape_L", [(0.3, 0.1), (0.6, 0.1), (0.45, 0.3)], rig)
# A reference of the body inside the rig, a point of it moved.
layers.active = find("Body_L", rig)
assert ops.layer.make_reference() == {'FINISHED'}
ref = layers.active
ref.name = "BodyRef_L"
ref.location_x = 0.5
# The body: bent by the cage's shape, merged with the ear, mirrored about x = 0.3.
layers.active = find("Body_L", rig)
assert ops.object.modifier_add(type="CURVE_DEFORM") == {'FINISHED'}
body = find("Body_L", rig)
body.modifier_set(len(body.modifiers) - 1, layer="Cage_L")
assert ops.object.modifier_add(type="BOOLEAN", targets="Ear_L") == {'FINISHED'}
assert ops.object.modifier_add(type="MIRROR") == {'FINISHED'}
body = find("Body_L", rig)
body.modifier_set(len(body.modifiers) - 1, center=(0.3, 0.0))
# The ear behind the body; the bone drawn as Shape_L and kept inside the cage.
layers.active = find("Ear_L", rig)
assert ops.layer.draw_order_set(mode="BEHIND", target="Body_L") == {'FINISHED'}
layers.active = rig
rig.bone_set("arm_L", shape="CUSTOM", shape_layer="Shape_L")
assert ops.armature.constraint_add(type="LIMIT_SHAPE", bone="arm_L") == {'FINISHED'}
rig.bone_constraint_set("arm_L", 0, shape_layer="Cage_L")
# A second bone, arm_R, on the left too (its name swaps with arm_L's when mirrored); the ear follows arm_L.
assert ops.armature.bone_add(name="arm_R", head_x="0.2", head_y="0.6", tail_x="0.5", tail_y="0.9", parent="arm_L") == {'FINISHED'}
layers.active = find("Ear_L", rig)
assert ops.layer.parent_bone_set(bone="arm_L") == {'FINISHED'}
rig = find("Rig_L")


def check_links(top, body_name, cage_name, ear_name, shape_name, ref_name, bone_name, label):
    b = find(body_name, top)
    c = find(cage_name, top)
    e = find(ear_name, top)
    r = find(ref_name, top)
    assert b and c and e and r, (label, [l.name for l in walk(top)])
    assert r.reference_source is not None and r.reference_source.id == b.id, \
        (label, "reference", r.reference_source and r.reference_source.name)
    assert modifier(b, "CURVE_DEFORM")["layer"] == cage_name, (label, modifier(b, "CURVE_DEFORM"))
    assert modifier(b, "BOOLEAN")["targets"] == [ear_name], (label, modifier(b, "BOOLEAN")["targets"])
    assert e.draw_order == ("BEHIND", body_name), (label, e.draw_order)
    bone = next(x for x in top.bones if x["name"] == bone_name)
    assert bone["shape_layer"] == shape_name, (label, bone["shape_layer"])
    con = top.bone_constraints(bone["name"])[0]
    assert con["shape_layer"] == c.id, (label, "constraint", con["shape_layer"], c.id)


check_links(rig, "Body_L", "Cage_L", "Ear_L", "Shape_L", "BodyRef_L", "arm_L", "the original")

# --- Duplicate: the copy's links name the copy's layers ----------------------------------------------------------
layers.active = rig
assert ops.layer.duplicate() == {'FINISHED'}
copy = layers.active
assert copy.id != rig.id
for l in walk(copy):
    l.name = l.name + " C"
check_links(copy, "Body_L C", "Cage_L C", "Ear_L C", "Shape_L C", "BodyRef_L C", "arm_L", "duplicate")
check_links(rig, "Body_L", "Cage_L", "Ear_L", "Shape_L", "BodyRef_L", "arm_L", "the original after duplicating")
remove(copy)
rig = find("Rig_L")

# Two layers picked (not their group): a reference copied with its original shows the original's copy.
layers.active = find("Body_L", rig)
find("BodyRef_L", rig).selected = True
assert ops.layer.duplicate() == {'FINISHED'}
copies = [l for l in walk(rig) if l.selected or l.id == layers.active.id]
body_copy = next(l for l in copies if l.reference_source is None)
ref_copy = next(l for l in copies if l.reference_source is not None)
assert body_copy.id != find("Body_L", rig).id
assert ref_copy.reference_source.id == body_copy.id, (ref_copy.reference_source.name, body_copy.name)
remove(ref_copy, body_copy)
rig = find("Rig_L")

# --- Symmetrize: the right side links to the right side --------------------------------------------------------
layers.active = find("Arm_L")
assert ops.layer.symmetrize(direction="POSITIVE_TO_NEGATIVE") == {'FINISHED'}
right = find("Rig_R")
assert right is not None and right.parent.name == "Arm_R", [(l.name, l.parent and l.parent.name) for l in layers.all()]
ref_r = find("BodyRef_R", right)
assert ref_r is not None and ref_r.reference_source is not None, "the reference stays one, its original mirrored with it"
# A bone layer inside a mirrored group has its bones mirrored, each taking its other side's name (arm_L and arm_R
# swap), so what follows arm_L on the left follows arm_R on the right.
check_links(right, "Body_R", "Cage_R", "Ear_R", "Shape_R", "BodyRef_R", "arm_R", "symmetrize")


def ends(layer, name):
    b = next(x for x in layer.bones if x["name"] == name)
    return [tuple(round(v, 4) for v in layer.to_document(p)) for p in (b["head"], b["tail"])]


for left_name, right_name in (("arm_L", "arm_R"), ("arm_R", "arm_L")):
    mirrored_ends = [(-x, y) for x, y in ends(rig, left_name)]
    assert ends(right, right_name) == mirrored_ends, (left_name, ends(right, right_name), mirrored_ends)
assert next(b for b in right.bones if b["name"] == "arm_L")["parent"] == "arm_R", "the hierarchy goes across too"
assert find("Ear_R", right).parent_bone == "arm_R", find("Ear_R", right).parent_bone
assert abs(modifier(find("Body_R", right), "MIRROR")["center"][0] + 0.3) < 1e-6, "the Mirror's centre goes across"
# The reference shows the mirrored body, moved as the left one is (mirrored).
left_pts = [find("BodyRef_L", rig).to_document(p) for p in find("BodyRef_L", rig).evaluated()[0]]
right_pts = [ref_r.to_document(p) for p in ref_r.evaluated()[0]]
assert sorted((round(-x, 4), round(y, 4)) for x, y in left_pts) == sorted((round(x, 4), round(y, 4)) for x, y in right_pts), \
    (left_pts, right_pts)
check_links(rig, "Body_L", "Cage_L", "Ear_L", "Shape_L", "BodyRef_L", "arm_L", "the original after symmetrizing")
remove(find("Arm_R"))
rig = find("Rig_L")

# --- Copy and paste: the pasted group's links name the pasted layers -----------------------------------------
layers.active = rig
assert ops.layer.copy() == {'FINISHED'}
assert ops.layer.paste() == {'FINISHED'}
pasted = layers.active
assert pasted.id != rig.id
for l in walk(pasted):
    l.name = l.name + " P"
check_links(pasted, "Body_L 1 P", "Cage_L 1 P", "Ear_L 1 P", "Shape_L 1 P", "BodyRef_L 1 P", "arm_L", "paste")
remove(pasted)
rig = find("Rig_L")
# The body alone: its deform's layer and Boolean's target, left behind, are this file's layers of those names.
layers.active = find("Body_L", rig)
assert ops.layer.copy() == {'FINISHED'}
assert ops.layer.paste() == {'FINISHED'}
alone = layers.active
assert alone.id != find("Body_L", rig).id
assert modifier(alone, "CURVE_DEFORM")["layer"] == "Cage_L" and modifier(alone, "BOOLEAN")["targets"] == ["Ear_L"]

print("relative links: ok")
