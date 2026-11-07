"""Apply Selected as Rest Pose with Keep Animation on a frame where the bones squash the drawing nearly flat (an eye seen
edge-on): the points are keyed back towards where they showed, never farther off than without Keep Animation, and never
sent far for it. Undoing the squash exactly would take a point many times as far as it is off, and every frame between
its keys would show it flying there and back (on a deformed eye, with seconds of work a frame).

Run with:  OpenAnimate --background --python tests/test_keep_animation_squashed.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
anchor = layers.active


def scene(other_head, keep):
    """Two bones side by side bending a drawing; a turned on 10 (applied), both squashed across on 20."""
    doc.frame_current = 0
    layers.active = anchor
    assert ops.layer.new_bone() == {'FINISHED'}
    rig = layers.active
    ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
    ops.armature.bone_add(name="b", head_x="0", head_y=str(other_head), tail_x="1", tail_y=str(other_head), parent="")
    ops.layer.new()
    art = layers.active
    assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
    art = next(l for l in layers.all() if l.id == art.id)
    art.add_curve([(0.1, 0.0), (0.5, 0.3), (0.9, 0.1), (0.6, -0.2)], closed=True)
    layers.active = art
    ops.mesh.select_all()
    rig.select_bone("a")
    rig.select_bone("b", extend=True)
    assert ops.weight.automatic() == {'FINISHED'}
    layers.active = rig
    for frame, bones in ((10, dict(a=dict(angle=1.5, position=(0.1, 0.2)), b=dict(angle=-0.2))),
                         (20, dict(a=dict(scale=(0.002, 1.0)), b=dict(scale=(0.002, 1.0), angle=0.5)))):
        doc.frame_current = frame
        for name, values in bones.items():
            rig.bone_set(name, **values)
        doc.frame_current = frame + 1
        doc.frame_current = frame
    doc.frame_current = 20
    shown = list(art.shown()[0])
    doc.frame_current = 10
    rig.select_bone("a")
    assert ops.armature.apply_selected_as_rest(keep_animation=keep) == {'FINISHED'}
    doc.frame_current = 20
    after = [p[:2] for p in art.points], list(art.shown()[0])
    rig.visible = False
    return shown, after


for other_head in (0.4, 1.0):
    shown, (stored_plain, shown_plain) = scene(other_head, "0")
    _, (stored_kept, shown_kept) = scene(other_head, "1")
    off_plain = max(math.dist(p, q) for p, q in zip(shown, shown_plain))
    off_kept = max(math.dist(p, q) for p, q in zip(shown, shown_kept))
    moved = max(math.dist(p, q) for p, q in zip(stored_plain, stored_kept))
    print(other_head, "off without %.4f, with %.4f; keyed back by %.4f" % (off_plain, off_kept, moved), flush=True)
    assert off_plain > 0.01, "the squash is there to be kept"
    assert off_kept < off_plain * 0.9, ("kept nearer", off_kept, off_plain)
    assert moved < 5.0 * off_plain, ("a point sent far to make up a little across the squash", moved, off_plain)

print("keep animation on a squashed frame: all good")
