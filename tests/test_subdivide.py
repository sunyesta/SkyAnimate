"""The Resample modifier (called Subdivide at first): splits segments before the bones bend them, so one long segment bends along a
bone chain. Placed above Armature; at rest the drawing looks the same; By Length (default) or By Count;
the new points' weights Blend (default) or come From Bones; saved; Apply makes the points real.

Run with:  OpenAnimate --background --python tests/test_subdivide.py
"""

import math
import os
import shutil
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="upper", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="lower", head_x="1", head_y="0", tail_x="2", tail_y="0", parent="upper")
ops.layer.new()
arm = layers.active
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
arm = next(l for l in layers.all() if l.id == arm.id)
arm.add_curve([(0.0, 0.1), (2.0, 0.1)])
layers.active = arm
for l in layers.all():
    l.select = l.id == arm.id
# One point on each bone.
for point, bone in ((0, "upper"), (1, "lower")):
    ops.mesh.select_none()
    arm.select_points([point])
    rig.select_bone(bone)
    assert ops.weight.assign(weight="1.0") == {'FINISHED'}
    ops.mesh.select_none()
    arm.select_points([point])
    for other in ("upper", "lower"):
        if other != bone:
            rig.select_bone(other)
            ops.weight.remove()
ops.mesh.select_none()


def evaluated():
    return arm.evaluated()[0]


def off_line(points):
    """How far the points stray from the straight line between the curve's ends."""
    (ax, ay), (bx, by) = points[0], points[1]
    dx, dy = bx - ax, by - ay
    length = math.hypot(dx, dy)
    return max(abs((p[0] - ax) * dy - (p[1] - ay) * dx) / length for p in points)


# Without it: bending the lower bone moves only the end point; the line stays straight.
doc.frame_current = 10
rig.bone_set("lower", angle=1.0)
doc.frame_current = 11
doc.frame_current = 10
assert len(evaluated()) == 2
doc.frame_current = 0

# Added: above the Armature modifier, named Resample, 4 pieces per segment and Blend by default.
assert ops.object.modifier_add(type="RESAMPLE") == {'FINISHED'}
mods = arm.modifiers
types = [m["type"] for m in mods]
assert types.index("RESAMPLE") < types.index("ARMATURE"), types
sub = types.index("RESAMPLE")
assert mods[sub]["name"] == "Resample", mods[sub]
assert mods[sub]["split"] == "COUNT" and mods[sub]["pieces"] == 4 and mods[sub]["weights"] == "BLEND", mods[sub]
assert len(evaluated()) == 2 + 3, "4 pieces: 3 points added"
# By Length: a point at least every Spacing (0.05 to start with).
arm.modifier_set(sub, split="LENGTH")
assert abs(arm.modifiers[sub]["spacing"] - 0.05) < 1e-6

# At rest the drawing looks the same: the new points lie on the old line, one at least every 0.05.
points = evaluated()
assert len(points) == 2 + 39, len(points)
assert all(abs(p[1] - 0.1) < 1e-5 and -1e-5 <= p[0] <= 2.0 + 1e-5 for p in points), points[:5]
assert max(abs(a[0] - b[0]) for a, b in zip(sorted(points), sorted(points)[1:])) <= 0.05 + 1e-3

# Posed: the line now bends through the elbow.
doc.frame_current = 10
bent = evaluated()
assert off_line(bent) > 0.05, ("it bends", off_line(bent))
mid = min(bent[2:], key=lambda p: abs(p[0] - 1.0) + abs(p[1] - 0.1))
doc.frame_current = 0

# Preserve Volume (on by default): folded far at the elbow, the split point halfway along (half on each
# bone) keeps its distance from the joint; blending the bones' matrices pulls it in toward it.
assert mods[sub]["preserve_volume"] is True
arm.modifier_set(sub, split="COUNT", pieces=2)
doc.frame_current = 20
rig.bone_set("lower", angle=2.8)
doc.frame_current = 21
doc.frame_current = 20


def halfway():
    pts = evaluated()
    return math.hypot(pts[2][0] - 1.0, pts[2][1] - 0.0)


kept = halfway()
arm.modifier_set(sub, preserve_volume=False)
pinched = halfway()
arm.modifier_set(sub, preserve_volume=True)
assert abs(kept - 0.1) < 0.01, ("kept its distance from the elbow", kept)
assert pinched < 0.05, ("without it, pinched in", pinched)
doc.frame_current = 0
assert abs(evaluated()[2][1] - 0.1) < 1e-5, "at rest it doesn't change anything"
arm.modifier_set(sub, split="LENGTH")

# By Count.
arm.modifier_set(sub, split="COUNT", pieces=4)
assert len(evaluated()) == 2 + 3
arm.modifier_set(sub, split="LENGTH")

# From Bones: points near the shoulder follow the upper bone, near the hand the lower one. With Blend
# a point a quarter of the way along still follows the lower bone a quarter; From Bones keeps it on upper.
def quarter_point():
    doc.frame_current = 10
    pts = evaluated()
    doc.frame_current = 0
    rest = evaluated()
    i = min(range(2, len(rest)), key=lambda k: abs(rest[k][0] - 0.5))
    return rest[i], pts[i]


rest, blend_pose = quarter_point()
arm.modifier_set(sub, weights="BONES")
rest, bones_pose = quarter_point()
assert math.hypot(bones_pose[0] - rest[0], bones_pose[1] - rest[1]) < math.hypot(blend_pose[0] - rest[0], blend_pose[1] - rest[1]), \
    ("From Bones keeps it on the upper bone", rest, blend_pose, bones_pose)
arm.modifier_set(sub, weights="BLEND")

# A corner stays a corner: a polyline's middle point is still there, unmoved.
ops.layer.new()
corner = layers.active
corner.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0)])
layers.active = corner
assert ops.object.modifier_add(type="SUBDIVIDE") == {'FINISHED'}, "scripts from before the rename still work"
assert "RESAMPLE" in [x["type"] for x in corner.modifiers], corner.modifiers
cpts = corner.evaluated()[0]
assert (1.0, 0.0) in [(round(p[0], 5), round(p[1], 5)) for p in cpts], "the corner point stays"
assert len(cpts) > 3

# Saved with the project.
folder = tempfile.mkdtemp(prefix="oanim_subdiv_")
path = os.path.join(folder, "subdiv.oanim")
layers.active = arm
arm.modifier_set(sub, spacing=0.1, render_spacing=0.02, max_pieces=12, group="")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
arm = next(l for l in oanim.data.layers.all() if l.point_count == 2 and "RESAMPLE" in [m["type"] for m in l.modifiers])
m = [x for x in arm.modifiers if x["type"] == "RESAMPLE"][0]
assert abs(m["spacing"] - 0.1) < 1e-6 and abs(m["render_spacing"] - 0.02) < 1e-6 and m["max_pieces"] == 12, m
assert len(arm.evaluated()[0]) == 2 + 12 - 1, "max pieces caps it"
assert len(arm.evaluated(render=True)[0]) == 2 + 12 - 1
shutil.rmtree(folder, ignore_errors=True)

# Apply on the setup frame makes the points real (the drawing isn't animated).
oanim.data.document.frame_current = 0
layers = oanim.data.layers
layers.active = arm
sub = [x["type"] for x in arm.modifiers].index("RESAMPLE")
assert ops.object.modifier_apply(index=str(sub)) == {'FINISHED'}
assert arm.point_count == 2 + 11 and "RESAMPLE" not in [x["type"] for x in arm.modifiers]

# A file from before the rename (type SUBDIVIDE, named Subdivide) opens as Resample, its default name with it.
import zipfile
layers.active = arm
assert ops.object.modifier_add(type="RESAMPLE") == {'FINISHED'}
arm.modifier_set([x["type"] for x in arm.modifiers].index("RESAMPLE"), name="Resample.001")
folder = tempfile.mkdtemp(prefix="oanim_resample_")
path, old = os.path.join(folder, "new.oanim"), os.path.join(folder, "old.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
with zipfile.ZipFile(path) as src, zipfile.ZipFile(old, "w", zipfile.ZIP_DEFLATED) as dst:
    for item in src.infolist():
        data = src.read(item.filename)
        if item.filename.endswith(".json"):
            data = data.decode("utf-8").replace('"RESAMPLE"', '"SUBDIVIDE"').replace('"Resample', '"Subdivide').encode("utf-8")
        dst.writestr(item, data)
assert ops.wm.open_mainfile(filepath=old, confirmed="1") == {'FINISHED'}
found = [(m["type"], m["name"]) for l in oanim.data.layers.all() for m in l.modifiers if m["type"] == "RESAMPLE"]
assert ("RESAMPLE", "Resample.001") in found, found
shutil.rmtree(folder, ignore_errors=True)

print("subdivide: ok")
