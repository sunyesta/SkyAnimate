"""Apply Pose as Rest Pose with Copy Angle and Copy Position (Follow Move): they copy their target's turn and move from
its rest, so when the target's rest moves, the bones copying it make up for it in their own keys (or a key holding
it), and every keyed pose, in the mainline and every action, shows as it did. Apply Selected with the target alone
picked: the bone copying it keeps its rest and still shows as it did.

Run with:  OpenAnimate --background --python tests/test_apply_rest_copy.py
"""

import math

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers


def scene(own_keys):
    """ctrl turns and moves (keyed on 10), mid turns in action wave (8); foll copies half ctrl's turn, slid copies
    half its move, echo copies foll's own turn. With `own_keys`, foll and slid have keys of their own (on 20) as well."""
    ops.wm.read_homefile(confirmed="1")
    doc.auto_key = True
    doc.frame_current = 0
    ops.layer.new_bone()
    rig = layers.active
    for n, h, t in (("mid", (1, 0), (2, 0)), ("ctrl", (0, 3), (1, 3)), ("foll", (0, -2), (1, -2)),
                    ("slid", (3, -2), (4, -2)), ("echo", (0, -4), (1, -4))):
        ops.armature.bone_add(name=n, head_x=str(h[0]), head_y=str(h[1]), tail_x=str(t[0]), tail_y=str(t[1]), parent="")
    for n, kind in (("foll", "COPY_ANGLE"), ("slid", "COPY_POSITION"), ("echo", "COPY_ANGLE")):
        ops.armature.select_none()
        rig.select_bone(n)
        assert ops.armature.constraint_add(type=kind, bone=n) == {'FINISHED'}
    rig.bone_constraint_set("foll", 0, target="ctrl", factor=0.5)
    rig.bone_constraint_set("slid", 0, target="ctrl", factor=0.5, follow_move=True)
    rig.bone_constraint_set("echo", 0, target="foll", factor=1.0)
    doc.frame_current = 10
    rig.bone_set("ctrl", angle=0.6, position=(0.5, 3.4))
    ops.armature.select_none()
    rig.select_bone("ctrl")
    ops.anim.keyframe_insert()
    if own_keys:
        doc.frame_current = 20
        rig.bone_set("foll", angle=-0.4)
        rig.bone_set("slid", position=(3.3, -1.8))
        ops.armature.select_none()
        for n in ("foll", "slid"):
            rig.select_bone(n, extend=True)
        ops.anim.keyframe_insert()
    ops.anim.action_new(name="wave")
    doc.frame_current = 8
    ops.armature.select_none()
    rig.select_bone("mid")
    ops.armature.rotate(angle="30")
    ops.anim.action_set(name="")
    return rig


def look(rig, action, frame):
    if action:
        ops.anim.action_set(name=action)
    doc.frame_current = frame
    out = {b["name"]: (tuple(b["head"]), tuple(b["tail"])) for b in rig.bones}
    if action:
        ops.anim.action_set(name="")
    return out


def worst(a, b):
    return max(math.dist(p, q) for n in a for p, q in zip(a[n], b[n]))


# Keyed frames (and past the last key): between frame 0 and a key, the new rest shows through as frame 0 changes.
FRAMES = [("", 10), ("", 20), ("", 30), ("wave", 8), ("wave", 12)]

for own_keys in (False, True):
    for selected in (False, True):
        rig = scene(own_keys)
        before = {f: look(rig, *f) for f in FRAMES}
        rest = look(rig, "", 0)
        doc.frame_current = 10
        layers.active = rig
        if selected:
            ops.armature.select_none()
            rig.select_bone("ctrl")
            assert ops.armature.apply_selected_as_rest() == {'FINISHED'}
        else:
            assert ops.armature.apply_pose_as_rest() == {'FINISHED'}
        what = ("own keys" if own_keys else "no keys of their own") + (", Apply Selected" if selected else "")
        for f in FRAMES:
            w = worst(look(rig, *f), before[f])
            assert w < 1e-4, (what, f, w, look(rig, *f), before[f])
        now = look(rig, "", 0)
        assert math.dist(now["ctrl"][1], rest["ctrl"][1]) > 0.1, (what, "ctrl's rest is its pose on 10")
        for n in ("foll", "slid"):
            if selected:
                assert math.dist(now[n][1], rest[n][1]) < 1e-5, (what, n, "left out: rests where it did")
            else:
                assert math.dist(now[n][1], before[("", 10)][n][1]) < 1e-4, (what, n, "its rest is its pose on 10")

print("apply rest copy: ok")
