"""Frame 0's key is the setup (the rest pose, in the mainline and every action): deleting it goes only with
the last of the keys. Deleting it first used to leave frame 0 showing the next key's pose, which recording
there then took as the bone's rest.

Run with:  OpenAnimate --background --python tests/test_setup_key_delete.py
"""

import oanim

doc = oanim.data.document
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True

assert ops.layer.new_bone() == {'FINISHED'}
rig = oanim.data.layers.active
ops.armature.bone_add(name="lid", head_x="5", head_y="5", tail_x="5", tail_y="6", parent="")
rest = next(b for b in rig.bones if b["name"] == "lid")["setup_position"]
path = 'bones["lid"].position'


def lid():
    return next(b for b in rig.bones if b["name"] == "lid")


def close(a, b):
    return max(abs(x - y) for x, y in zip(a, b)) < 1e-6


for action in ("blink", ""):
    if action:
        assert ops.anim.action_new(name=action) == {'FINISHED'}
    doc.frame_current = 14
    rig.bone_set("lid", position=(rest[0] + 0.5, rest[1] + 0.3))
    doc.frame_current = 0
    assert [k["frame"] for k in rig.keyframes(path)] == [0, 14], (action, rig.keyframes(path))
    # The script's way (oanim's keyframe_delete) and Alt I both keep the rest key while others are left.
    assert not rig.keyframe_delete(path, 0), "the rest key stays while there are others"
    assert ops.anim.keyframe_delete() in ({'FINISHED'}, {'CANCELLED'})
    assert [k["frame"] for k in rig.keyframes(path)] == [0, 14], "Alt I on frame 0 keeps it too"
    assert rig.keyframe_delete(path, 14)
    assert not rig.keyframes(path), "the lone rest key goes with the last"
    doc.frame_current = 1
    doc.frame_current = 0
    assert close(lid()["setup_position"], rest), (action or "mainline", lid()["setup_position"], rest)
    if action:
        assert ops.anim.action_set(name="") == {'FINISHED'}
        doc.frame_current = 1
        doc.frame_current = 0
        assert close(lid()["setup_position"], rest), ("back on the mainline", lid()["setup_position"], rest)

print("setup key delete: ok")
