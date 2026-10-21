"""Weights, as in Blender: new points get automatic weights, the Weight Paint brush has a smooth
falloff and Draw, Add and Subtract modes (and a Selection Mask), weights stay normalized, Automatic
Weights / Assign / Remove / Normalize, mirrored halves follow the other side's bones, weights are
saved, and older files' binding turns into the same weights.

Run with:  OpenAnimate --background --python tests/test_weight_paint.py
"""

import json
import math
import pathlib
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


doc.frame_current = 0
art = layers.active
oanim.ops.layer.new_bone()
rig = layers.active
layers.active = art
oanim.ops.layer.move_to_group(group=str(rig.id))

layers.active = rig
oanim.ops.armature.bone_add(name="upper_L", head_x="0", head_y="0", tail_x="0.5", tail_y="0")
oanim.ops.armature.bone_add(name="lower_L", head_x="0.5", head_y="0", tail_x="1", tail_y="0")

layers.active = art
art.add_curve([(0.1, 0.0), (0.3, 0.0), (0.7, 0.0), (0.9, 0.0)])

# New points get automatic weights at the next undo step: nearer bones pull harder.
assert art.weights == [{}, {}, {}, {}], "not weighted yet"
oanim.ops.mesh.select_none()
upper = art.bone_weights("upper_L")
assert upper[0] > 0.9 and upper[3] < 0.1, upper
assert all(abs(sum(w.values()) - 1.0) < 1e-4 for w in art.weights), art.weights
unpainted = art.weights

# --- The brush --------------------------------------------------------------------------------
# Draw lower_L at full weight right on point 0: it moves entirely to lower_L.
assert art.weight_paint((0.1, 0.0), 0.15, "lower_L", weight=1.0, strength=1.0) == 1
w = art.painted_weights
assert abs(w[0]["lower_L"] - 1.0) < 1e-4 and "upper_L" not in w[0], w[0]
assert w[1] == unpainted[1] and w[2] == unpainted[2], "points outside the brush are untouched"
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
oanim.ops.armature.symmetrize()
assert {b["name"] for b in rig.bones} == {"upper_L", "lower_L", "upper_R", "lower_R"}
layers.active = art
oanim.ops.object.modifier_add(type="MIRROR")
doc.frame_current = 10
rig.bone_set("lower_R", angle=-math.pi / 2)
doc.frame_current = 10
points, _, _ = art.evaluated()
assert len(points) == 8
assert close(points[4], (-0.5, -0.4)), ("the mirror of point 0 follows lower_R", points[4])
doc.frame_current = 0

# --- Saved -------------------------------------------------------------------------------------
before = art.painted_weights
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_weights_")) / "weights.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in oanim.data.layers if l.type == "BONE")
art = rig.children[0]
after = art.painted_weights
assert len(after) == len(before)
for a, b in zip(after, before):
    assert a.keys() == b.keys() and all(abs(a[k] - b[k]) < 1e-5 for k in a), (a, b)

# --- Weight Select's buttons ------------------------------------------------------------------
oanim.data.layers.active = art
# Weight tools (like Draw and Split Curve) are for the setup frame only.
doc.frame_current = 4
assert oanim.ops.wm.tool_weight_paint() == {'CANCELLED'} and oanim.ops.wm.tool_draw() == {'CANCELLED'}
assert oanim.ops.wm.tool_select() == {'FINISHED'}
doc.frame_current = 0
assert oanim.ops.wm.tool_weight_select() == {'FINISHED'}
oanim.ops.mesh.select_all()
rig.select_bone("lower_L")
assert oanim.ops.weight.assign(weight="1") == {'FINISHED'}
assert all(w == {"lower_L": 1.0} for w in art.weights), art.weights
assert oanim.ops.weight.remove() == {'FINISHED'}
assert art.weights == [{}, {}, {}, {}] and max(art.bone_weights("lower_L")) == 0.0, "removed: they stay put"
oanim.ops.mesh.select_none()
assert art.weights == [{}, {}, {}, {}], "removed points don't get automatic weights again"
# Automatic Weights uses every bone, even with the painted bone selected (it would be all red).
assert rig.bones[[b["name"] for b in rig.bones].index("lower_L")]["selected"]
assert oanim.ops.weight.automatic() == {'FINISHED'}
assert art.bone_weights("upper_L")[0] > 0.9 and art.bone_weights("lower_L")[3] > 0.5, art.weights
# bones=SELECTED limits it to the selected bones.
assert oanim.ops.weight.automatic(bones="SELECTED") == {'FINISHED'}
assert all(set(w) == {"lower_L"} for w in art.weights), "only the selected bone"
assert oanim.ops.weight.automatic() == {'FINISHED'}
# Normalize: weights that don't add up to 1 do again.
art.select_points([0])
art.weight_paint((0.1, 0.0), 0.05, "lower_L", weight=0.5, strength=1.0, mode="ADD", normalize=False)
assert sum(art.weights[0].values()) > 1.01
assert oanim.ops.weight.normalize() == {'FINISHED'}
assert abs(sum(art.weights[0].values()) - 1.0) < 1e-4

# Selection Mask: only the selected point takes paint.
oanim.ops.mesh.select_none()
art.select_points([3])
before = art.weights
assert art.weight_paint((0.5, 0.0), 1.0, "upper_L", weight=1.0, strength=1.0, selected_only=True) == 1
after = art.weights
assert after[:3] == before[:3] and after[3] != before[3]

# A new stroke follows the bones at once.
art.add_curve([(0.2, 0.3), (0.8, 0.3)])
oanim.ops.mesh.select_none()
assert art.weights[-1] and art.weights[-2], "new points got weights"
assert art.bone_weights("lower_L")[-1] > 0.5

# --- Older files: point binding and flexi-binding become weights -------------------------------
path2 = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_weights_")) / "old.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path2) == {'FINISHED'}
with zipfile.ZipFile(path2) as z:
    data = json.loads(z.read("document.json"))
bone_layer = next(l for l in data["document"]["layers"] if l.get("bones"))
lower_id = next(b["id"] for b in bone_layer["bones"] if b["name"] == "lower_L")
points = bone_layer["children"][0]["drawing"]["points"]
for pj in points:
    pj.pop("weights", None)
points[1]["bone"] = lower_id
with zipfile.ZipFile(path2, "w") as z:
    z.writestr("document.json", json.dumps(data))
assert oanim.ops.wm.open_mainfile(filepath=path2, confirmed="1") == {'FINISHED'}
rig = next(l for l in oanim.data.layers if l.type == "BONE")
art = rig.children[0]
assert art.weights[1] == {"lower_L": 1.0}, "a bound point follows its bone alone"
assert art.weights[0] and art.bone_weights("upper_L")[0] > 0.9, "flexi-bound points get the same weights"

print("Weight paint tests passed")
