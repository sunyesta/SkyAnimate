"""Weight painting, as in Blender: painted weights decide how bones move points, the brush has a
smooth falloff and Draw, Add and Subtract modes, weights stay normalized, mirrored halves follow
the other side's bones, and weights are saved.

Run with:  SkyAnimate --background --python tests/test_weight_paint.py
"""

import math
import pathlib
import tempfile

import sky

doc = sky.data.document
layers = sky.data.layers


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


doc.frame_current = 0
art = layers.active
sky.ops.layer.new_bone()
rig = layers.active
layers.active = art
sky.ops.layer.move_to_group(group=str(rig.id))

layers.active = rig
sky.ops.armature.bone_add(name="upper_L", head_x="0", head_y="0", tail_x="0.5", tail_y="0")
sky.ops.armature.bone_add(name="lower_L", head_x="0.5", head_y="0", tail_x="1", tail_y="0")

layers.active = art
art.add_curve([(0.1, 0.0), (0.3, 0.0), (0.7, 0.0), (0.9, 0.0)])

# Before painting, points follow the bones by distance (flexi).
upper = art.bone_weights("upper_L")
assert upper[0] > 0.9 and upper[3] < 0.1, upper
assert art.painted_weights == [{}, {}, {}, {}]

# --- The brush --------------------------------------------------------------------------------
# Draw lower_L at full weight right on point 0: it moves entirely to lower_L.
assert art.weight_paint((0.1, 0.0), 0.15, "lower_L", weight=1.0, strength=1.0) == 1
w = art.painted_weights
assert abs(w[0]["lower_L"] - 1.0) < 1e-4 and "upper_L" not in w[0], w[0]
assert w[1] == {} and w[2] == {}, "points outside the brush are untouched"
assert art.bone_weights("lower_L")[0] > 0.999

# Falloff: a point at half the radius changes by about half.
art.weight_paint((0.2, 0.0), 0.2, "lower_L", weight=1.0, strength=1.0)
w1 = art.painted_weights[1]["lower_L"]
assert 0.4 < w1 < 0.8, w1
assert abs(sum(art.painted_weights[1].values()) - 1.0) < 1e-4, "normalized: the weights add up to 1"

# Subtract takes weight away; Add puts it back.
art.weight_paint((0.3, 0.0), 0.05, "lower_L", weight=0.2, strength=1.0, mode="SUBTRACT")
assert abs(art.painted_weights[1]["lower_L"] - (w1 - 0.2)) < 1e-4
art.weight_paint((0.3, 0.0), 0.05, "lower_L", weight=0.2, strength=1.0, mode="ADD")
assert abs(art.painted_weights[1]["lower_L"] - w1) < 1e-4

# --- Painted weights move the points -------------------------------------------------------------
doc.frame_current = 10
rig.bone_set("lower_L", angle=math.pi / 2)  # the lower bone turns up about (0.5, 0)
doc.frame_current = 10
points, _, _ = art.evaluated()
# Point 0 (0.1, 0) follows lower_L only: turned 90 degrees about (0.5, 0) -> (0.5, -0.4).
assert close(points[0], (0.5, -0.4)), points[0]
doc.frame_current = 0

# --- Mirror: the reflection follows the other side's bones -------------------------------------
layers.active = rig
rig.select_bone("")
sky.ops.armature.symmetrize()
assert {b["name"] for b in rig.bones} == {"upper_L", "lower_L", "upper_R", "lower_R"}
layers.active = art
sky.ops.object.modifier_add(type="MIRROR")
doc.frame_current = 10
rig.bone_set("lower_R", angle=-math.pi / 2)
doc.frame_current = 10
points, _, _ = art.evaluated()
assert len(points) == 8
assert close(points[4], (-0.5, -0.4)), ("the mirror of point 0 follows lower_R", points[4])
doc.frame_current = 0

# --- Saved -------------------------------------------------------------------------------------
before = art.painted_weights
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_weights_")) / "weights.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in sky.data.layers if l.type == "BONE")
art = rig.children[0]
after = art.painted_weights
assert len(after) == len(before)
for a, b in zip(after, before):
    assert a.keys() == b.keys() and all(abs(a[k] - b[k]) < 1e-5 for k in a), (a, b)

# Release Points clears painted weights (back to flexi).
sky.data.layers.active = art
sky.ops.mesh.select_all()
assert sky.ops.armature.unbind_points() == {'FINISHED'}
assert art.painted_weights == [{}, {}, {}, {}]

print("Weight paint tests passed")
