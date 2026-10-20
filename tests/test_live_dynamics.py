"""Live dynamics and editing from the keyed skeleton: when a bone's aim changes on a frame, the
dynamic bones simulate in real time (shown only) until the frame changes; and
posing a swung bone works from where its keys put it, so it lands where it is put.

Run with:  SkyAnimate --background --python tests/test_live_dynamics.py
"""

import math

import _sky
import sky

doc = sky.data.document
doc.frame_current = 0
sky.ops.layer.new_bone()
rig = sky.data.layers.active
for name, head, tail, parent in (("body", (0, 0), (1, 0), ""), ("tail", (1, 0), (2, 0), "body")):
    assert sky.ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]),
                                     tail_y=str(tail[1]), parent=parent) == {'FINISHED'}
rig.select_bone("tail")
assert sky.ops.armature.constraint_add(type="DYNAMICS", bone="tail") == {'FINISHED'}


def bones():
    doc.frame_current = doc.frame_current  # sync
    return {b["name"]: b for b in rig.bones}


def direction(b):
    return math.atan2(b["tail"][1] - b["head"][1], b["tail"][0] - b["head"][0])


def tick():
    running, angles = _sky.dynamics_live_tick(1 / 60)
    return running, angles["tail"]


# --- Live: turning the body on a frame, the tail trails it in real time, then settles ----------------
doc.frame_current = 10
assert abs(direction(bones()["tail"])) < 1e-6
assert not tick()[0], "nothing changed: nothing live"
live = []
running = []
for i in range(240):  # four seconds at 60 a second, the body turned over the first quarter, as a drag
    if i < 15:
        rig.bone_set("body", angle=(i + 1) / 15)
        bones()
    r, angle = tick()
    running.append(r)
    live.append(angle)
assert live[0] < 0.6, ("it trails the body at first", live[0])
assert max(live) > 1.0 + 1e-3, "and swings past it, in real time"
steps = [abs(b - a) for a, b in zip(live, live[1:])]
assert max(steps) < 0.25, ("smoothly, no jumps", max(steps))
assert all(running), "it stays live on this frame (no easing back)"
assert abs(live[-1] - 1.0) < 0.01, ("and settles there", live[-1])
assert abs(live[-1] - tick()[1]) < 1e-4, "settled, it stays put"
# Changing frame (scrubbing) shows the frames' own results again.
doc.frame_current = 11
assert not tick()[0], "not live after scrubbing"
doc.frame_current = 10
assert not tick()[0]
assert rig.keyframes('bones["tail"].angle') == [], "the live swing never becomes keys"

# --- Editing works from the keyed skeleton: a bone bent by gravity is posed where its keys are -----
rig.bone_set("body", angle=0.0)
doc.gravity = 8.0
doc.frame_current = 30
swung = direction(bones()["tail"])
assert swung < -0.05, ("gravity bends it", swung)
rig.select_bone("tail", part="TAIL")
assert rig.move_bone_ends((-0.3, 0.5))
# The keyed tip went where it was moved: from where the keys had it, not from the swung tip below it.
tip = {b["name"]: b for b in rig.bones_keyed}["tail"]["tail"]
doc.gravity = 0.0
assert abs(tip[0] - 1.7) < 2e-3 and abs(tip[1] - 0.5) < 2e-3, tip

# --- Wind is drag on how fast the air goes past: moving with it, a bone feels less; into it, more -----
def slide(speed):
    """A hanging bone carried sideways at `speed` (document units a second) in a steady wind of 5
    blowing right; its angle once the slide is steady (inertia off, so only the wind bends it)."""
    doc.frame_current = 0
    sky.ops.layer.new_bone()
    rig = sky.data.layers.active
    sky.ops.armature.bone_add(name="mast", head_x="0", head_y="0", tail_x="0", tail_y="0.2")
    sky.ops.armature.bone_add(name="hair", head_x="0", head_y="0", tail_x="0", tail_y="-0.8", parent="mast")
    rig.select_bone("hair")
    sky.ops.armature.constraint_add(type="DYNAMICS", bone="hair")
    rig.bone_constraint_set("hair", 0, own_forces=True, wind=5.0, wind_direction=0.0,
                            angle_spring={"inertia": 0.0})
    path = 'bones["mast"].position'
    for f in (1, 40):
        doc.frame_current = f
        rig.bone_set("mast", position=(speed * (f - 1) / doc.fps, 0.0))
        assert sky.ops.anim.keyframe_insert() == {'FINISHED'}
        rig.keyframe_set(path, f, interp="LINEAR")
    doc.frame_current = 30
    b = {x["name"]: x for x in rig.bones}["hair"]
    return math.atan2(b["tail"][1] - b["head"][1], b["tail"][0] - b["head"][0])


rest, with_wind, into_wind = slide(0.0), slide(4.0), slide(-4.0)
assert rest > -math.pi / 2 + 0.1, ("the wind bends a still bone", rest)
assert with_wind < rest - 0.05, ("moving with the wind, it feels less of it", rest, with_wind)
assert into_wind > rest + 0.05, ("moving into the wind, it feels more", rest, into_wind)

print("Live dynamics tests passed")
