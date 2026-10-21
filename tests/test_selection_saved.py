"""The selection is saved with the project: selected layers, the active layer and bone, selected
bones and their ends, points, handles picked on their own, and keys picked in the timeline.

Run with:  SkyAnimate --background --python tests/test_selection_saved.py
"""

import pathlib
import tempfile

import sky

doc = sky.data.document
layers = sky.data.layers
doc.frame_current = 0

art = layers.active
art.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.5)])
sky.ops.mesh.select_none()
art.select_points([0, 2])
art.select_handle(0, 1, "OUT")
doc.frame_current = 5
art.set_point(1, (1.0, 0.3))
doc.frame_current = 0

sky.ops.layer.new_bone()
rig = layers.active
sky.ops.armature.bone_add(name="arm", head_x="0", head_y="2", tail_x="1", tail_y="2", parent="")
sky.ops.armature.bone_add(name="hand", head_x="1", head_y="2", tail_x="2", tail_y="2", parent="arm")
sky.ops.armature.select_none()
rig.select_bone("hand", part="TAIL")
layers.active = art
art.selected = True
rig.selected = True
sky.ops.anim.keys_select(frame_start="5")


def state():
    a = next(l for l in layers.all() if l.name == art.name)
    r = next(l for l in layers.all() if l.type == "BONE")
    hand = next(b for b in r.bones if b["name"] == "hand")
    keys = [(k["frame"], k["selected"]) for k in a.keyframes("points[1].pos")]
    return {"points": [p[2] for p in a.points], "active": layers.active.name, "layers": (a.selected, r.selected),
            "hand": (hand["selected"], hand["select_head"], hand["select_tail"]), "keys": keys,
            "active_bone": [b["name"] for b in r.bones if b["active"]]}


before = state()
assert before["points"][0] and before["points"][2], before
assert any(selected for frame, selected in before["keys"]), ("a key picked", before["keys"])
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_selection_")) / "selection.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
after = state()
assert after == before, (before, after)

print("Selection saving tests passed")
