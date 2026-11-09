"""Key Together across saving and opening: a layer folded inside an action keys its mainline only once something there
is keyed (entering an action and leaving changes nothing). Opening the file must keep to that, not fill the mainline
on the first frame shown because it forgot which timeline it last keyed together.

Run with:  OpenAnimate --background --python tests/test_key_together_reopen.py
"""

import os
import tempfile

import oanim

ops, layers = oanim.ops, oanim.data.layers
assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
doc = oanim.data.document
doc.auto_key = True
doc.auto_freeze_keys = False


def frames(layer, path):
    try:
        return [(k["frame"], k["interp"]) for k in layer.keyframes(path)]
    except KeyError:
        return []


def settle():
    doc.frame_current = doc.frame_current + 1
    doc.frame_current = doc.frame_current - 1


def keys(layer):
    paths = ["position", "rotation", "scale", "opacity"] + ["points[%d].pos" % i for i in range(len(layer.points))]
    return {p: frames(layer, p) for p in paths}


# --- Folded inside an action, then the action left and the file saved and opened -------------------------------------
ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.layer.new()
hat = layers.active
hat.name = "Hat"
hat.key_together = False
hat.add_curve([(0.0, 5.0), (1.0, 5.0)])
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
# The mainline: the hat moves on 14, its points not keyed.
doc.frame_current = 14
hat.location_x = 1.0
doc.frame_current = 0
settle()
layers.active = rig
assert ops.anim.action_new(name="wave") == {'FINISHED'}
rig.select_bone("arm")
doc.frame_current = 6
assert ops.armature.rotate(angle="30") == {'FINISHED'}
doc.frame_current = 0
hat.key_together = True
settle()
assert ops.anim.action_set(name="") == {'FINISHED'}
settle()
mainline = keys(hat)
assert mainline["points[0].pos"] == [], ("left the action, nothing keyed on the mainline: not filled yet", mainline)
path = os.path.join(tempfile.mkdtemp(), "reopen.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
doc = oanim.data.document
hat = next(l for l in layers.all() if l.name == "Hat")
settle()
assert keys(hat) == mainline, ("opening the file changes no keys", mainline, keys(hat))
# Keyed on the mainline, it fills as it would have before saving.
doc.frame_current = 20
hat.location_x = 2.0
settle()
assert [f for f, _ in frames(hat, "points[0].pos")] == [0, 14, 20], frames(hat, "points[0].pos")

print("key together reopen: ok")
