"""Bone constraints: Blender's stack with Moho's kinds (Limit Angle, Independent Angle, Squash &
Stretch, Copy Angle / Position / Scale with factor and delay, Track To, Stretch To, IK), never on
the setup frame; interactive IK while posing; Dynamics simulated (and baked); all saved.

Run with:  OpenAnimate --background --python tests/test_bone_constraints.py
"""

import math
import pathlib
import tempfile

import oanim

oanim.data.document.auto_freeze_keys = False  # keyed channel by channel here, not holding the poses either side

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0

oanim.ops.layer.new_bone()
rig = layers.active


def add(name, head, tail, parent=""):
    assert oanim.ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]),
                                     tail_y=str(tail[1]), parent=parent) == {'FINISHED'}


def bones():
    doc.frame_current = doc.frame_current  # sync
    return {b["name"]: b for b in rig.bones}


def direction(b):
    return math.atan2(b["tail"][1] - b["head"][1], b["tail"][0] - b["head"][0])


def close(a, b, eps=1e-3):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def constrain(bone, kind, **values):
    rig.select_bone(bone)
    assert oanim.ops.armature.constraint_add(type=kind, bone=bone) == {'FINISHED'}
    index = len(rig.bone_constraints(bone)) - 1
    if kind != "DYNAMICS":
        index = [c["type"] for c in rig.bone_constraints(bone)].index(kind)
    if values:
        rig.bone_constraint_set(bone, index, **values)
    return index


add("root", (0, 0), (1, 0))
add("arm", (1, 0), (2, 0), parent="root")
add("ctrl", (0, 2), (1, 2))
add("follower", (0, -2), (1, -2))
add("squash", (4, 0), (5, 0))
add("perp", (4.5, 0.5), (4.5, 1.0), parent="squash")
add("eye", (0, -4), (0.5, -4))
add("tgt", (3, -1), (3.5, -1))
add("reacher", (0, -6), (0.5, -6))
add("base", (5, 3), (6, 3))
add("hand", (6, 3), (7, 3), parent="base")
add("goal", (5.5, 4.5), (5.7, 4.5))

# --- Limit Angle, and the setup frame stays raw -------------------------------------------------
constrain("arm", "LIMIT_ANGLE", min_angle=-0.5, max_angle=0.5)
doc.frame_current = 0
rig.bone_set("arm", angle=1.2)
assert abs(direction(bones()["arm"]) - 1.2) < 1e-3, "frame 0: constraints don't act on the setup pose"
rig.bone_set("arm", angle=0.0)
doc.frame_current = 10
rig.bone_set("arm", angle=1.2)
assert abs(direction(bones()["arm"]) - 0.5) < 1e-3, direction(bones()["arm"])
rig.bone_constraint_set("arm", 0, influence=0.5)
assert abs(direction(bones()["arm"]) - 0.85) < 1e-3, "influence: part of the way"
rig.bone_constraint_set("arm", 0, influence=1.0)
rig.bone_set("arm", angle=0.0)

# Its Min and Max are turns from the setup angle, as the Bone panel's Angle shows them, and Set to
# Current Angle takes the bone's turn now.
doc.frame_current = 0
perp_rest = next(b for b in rig.bones if b["name"] == "perp")["angle"]
assert abs(perp_rest) > 0.5, "perp rests turned from its parent"
perp = constrain("perp", "LIMIT_ANGLE", min_angle=-0.4, max_angle=0.4)
doc.frame_current = 10
rig.bone_set("perp", angle=perp_rest + 0.3)
assert oanim.ops.armature.limit_angle_set_current(bone="perp", index=str(perp), end="MAX") == {'FINISHED'}
rig.bone_set("perp", angle=perp_rest - 0.2)
assert oanim.ops.armature.limit_angle_set_current(bone="perp", index=str(perp), end="MIN") == {'FINISHED'}
c = rig.bone_constraints("perp")[perp]
assert abs(c["max_angle"] - 0.3) < 1e-5 and abs(c["min_angle"] + 0.2) < 1e-5, c
rig.bone_set("perp", angle=perp_rest + 1.0)
held = next(b for b in rig.bones if b["name"] == "perp")
assert abs(direction(held) - direction(next(b for b in rig.bones if b["name"] == "squash")) - perp_rest - 0.3) < 1e-3,     "held at Max, a turn from its rest"
rig.bone_set("perp", angle=perp_rest)
doc.frame_current = 0
assert oanim.ops.armature.constraint_remove(index=str(perp), bone="perp") == {'FINISHED'}
doc.frame_current = 10

# --- Independent Angle: keeps its angle when the parent turns -----------------------------------
constrain("arm", "INDEPENDENT_ANGLE")
rig.bone_set("root", angle=0.8)
b = bones()
assert abs(direction(b["root"]) - 0.8) < 1e-3
assert abs(direction(b["arm"])) < 1e-3, ("arm stays level", direction(b["arm"]))
rig.bone_set("root", angle=0.0)

# --- Copy Angle, with a factor and a delay ---------------------------------------------------------
constrain("follower", "COPY_ANGLE", target="ctrl", factor=0.5)
rig.bone_set("ctrl", angle=1.0)
assert abs(direction(bones()["follower"]) - 0.5) < 1e-3
doc.frame_current = 11  # records ctrl's key at frame 10 (and 0)
rig.bone_constraint_set("follower", 0, delay=5)
doc.frame_current = 12
assert abs(direction(bones()["follower"]) - 0.35) < 1e-3, ("ctrl at frame 7 is 0.7", direction(bones()["follower"]))
rig.bone_constraint_set("follower", 0, delay=0)

# --- Squash & Stretch: scaled up it thins --------------------------------------------------------
doc.frame_current = 10
# --- Maintain Volume: however it gets longer, it thins (in place of its own X scale) ---------------
constrain("squash", "MAINTAIN_VOLUME", amount=1.0)
rig.bone_set("squash", scale=2.0)
b = bones()
assert close(b["squash"]["tail"], (6, 0)), "twice as long"
assert close(b["perp"]["head"], (5.0, 0.25)), ("half as thick: the child's offset across it shrinks", b["perp"]["head"])
rig.bone_set("squash", scale=(1.0, 2.0))
assert close(bones()["perp"]["head"], (5.0, 0.25)), "keyed length alone thins it too"
rig.bone_set("squash", scale=(3.0, 2.0))
assert close(bones()["perp"]["head"], (5.0, 0.25)), "at 100% the width comes from the length, not its X"
rig.bone_constraint_set("squash", 0, amount=0.0)
assert close(bones()["perp"]["head"], (5.0, 1.5)), "0%: its own X again"
rig.bone_constraint_set("squash", 0, amount=1.0)
rig.bone_set("squash", scale=1.0)
assert rig.bone_constraints("squash")[0]["name"] == "Maintain Volume"

# --- Track To and Stretch To ----------------------------------------------------------------------
constrain("eye", "TRACK_TO", target="tgt")
assert abs(direction(bones()["eye"]) - math.pi / 4) < 1e-3, "points at the target's base"
constrain("reacher", "STRETCH_TO", target="tgt")
assert close(bones()["reacher"]["tail"], (3, -1)), "stretches to reach it"

# --- IK target -------------------------------------------------------------------------------------
constrain("hand", "IK", target="goal")
b = bones()
assert close(b["hand"]["tail"], (5.5, 4.5), 2e-3), ("the tip reaches the goal", b["hand"]["tail"])
assert abs(direction(b["base"])) > 0.1, "the base turned too"
# Ignored by IK: the chain stops below it.
rig.bone_set("goal", position=(6, 4))
rig.bone_set("base", ik_ignore=True)
b = bones()
assert abs(direction(b["base"])) < 1e-3 and close(b["hand"]["tail"], (6, 4), 2e-3), b["hand"]
rig.bone_set("base", ik_ignore=False)
# Out of reach: it stretches, up to max stretch.
rig.bone_set("goal", position=(8.5, 3.0))
assert not close(bones()["hand"]["tail"], (8.5, 3)), "out of reach without stretching"
rig.bone_constraint_set("hand", 0, max_stretch=2.0)
assert close(bones()["hand"]["tail"], (8.5, 3), 2e-3), ("stretched to reach", bones()["hand"]["tail"])
rig.bone_constraint_set("hand", 0, max_stretch=1.0)
rig.bone_set("goal", position=(5.5, 4.5))
# The arc solver also reaches.
rig.bone_set("hand", arc_ik=True)
assert close(bones()["hand"]["tail"], (5.5, 4.5), 0.02), bones()["hand"]["tail"]
rig.bone_set("hand", arc_ik=False)
rig.bone_constraint_set("hand", 0, enabled=False)

# --- Interactive IK: posing by the tip turns the parents -------------------------------------------
doc.frame_current = 20
rig.select_bone("hand", part="TAIL")
before = bones()
assert rig.move_bone_ends((-0.5, 1.0))
b = bones()
assert close(b["hand"]["tail"], (before["hand"]["tail"][0] - 0.5, before["hand"]["tail"][1] + 1.0), 2e-3), b["hand"]
assert abs(direction(b["base"]) - direction(before["base"])) > 0.05, "the parent turned"
assert close(b["base"]["head"], before["base"]["head"]), "the chain's base stays put"

# --- Dynamics: a tail swings behind its body, and settles ------------------------------------------
doc.frame_current = 0
add("body", (10, 0), (11, 0))
add("tail", (11, 0), (12, 0), parent="body")
constrain("tail", "DYNAMICS")
assert rig.bone_constraints("tail")[-1]["angle_spring"]["enabled"]
doc.frame_current = 1
rig.bone_set("body", angle=0.0)
assert oanim.ops.anim.keyframe_insert() == {'FINISHED'}
doc.frame_current = 4
rig.bone_set("body", angle=1.0)
assert oanim.ops.anim.keyframe_insert() == {'FINISHED'}
doc.frame_current = 4
first_visit = direction(bones()["tail"])
lag = abs(first_visit - 1.0)
assert lag > 0.05, ("the tail lags behind", lag)
doc.frame_current = 80
assert abs(direction(bones()["tail"]) - 1.0) < 0.02, ("and settles", direction(bones()["tail"]))
# Turning the parent on the same frame (as R does, before any scrub) moves the dynamic child at once.
settled = direction(bones()["tail"])
rig.select_bone("body")
assert rig.rotate_bones(0.5)
assert abs(direction(bones()["tail"]) - settled) > 0.02, ("the child follows straight away", direction(bones()["tail"]))
rig.bone_set("body", angle=1.0)  # back as keyed, so nothing new is recorded
doc.frame_current = 0
assert abs(direction(bones()["tail"])) < 1e-6, "the setup pose never swings"
doc.frame_current = 4
assert abs(direction(bones()["tail"]) - first_visit) < 1e-6, "going back after later frames gives the same swing"
# Gravity pulls it down, through its torque.
doc.gravity = 5.0
doc.frame_current = 80
assert direction(bones()["tail"]) < 0.98, ("gravity", direction(bones()["tail"]))
# Own Gravity & Wind: the bone's own in place of the document's.
rig.bone_constraint_set("tail", 0, own_forces=True, gravity=0.0)
assert abs(direction(bones()["tail"]) - 1.0) < 0.02, "its own (none) in place of the document's"
doc.gravity = 0.0
rig.bone_constraint_set("tail", 0, gravity=5.0)
assert direction(bones()["tail"]) < 0.98, "its own pulls it while the document has none"
rig.bone_constraint_set("tail", 0, gravity=0.0, wind=5.0, wind_direction=0.0)
assert direction(bones()["tail"]) < 0.98, "its own wind blows it to the right"
rig.bone_constraint_set("tail", 0, own_forces=False)
assert abs(direction(bones()["tail"]) - 1.0) < 0.02, "off: the document's again"
# Unticked, its own are kept, and ticking it again brings them back (not the document's).
doc.wind = 2.0
assert rig.bone_constraints("tail")[0]["wind"] == 5.0, "kept while unticked"
rig.bone_constraint_set("tail", 0, own_forces=True)
assert rig.bone_constraints("tail")[0]["wind"] == 5.0, "ticked again: its own, as they were"
rig.bone_constraint_set("tail", 0, own_forces=False, wind=0.0)
doc.wind = 0.0
# Own Gravity and Own Wind are separate: its own gravity (none) with the document's wind.
doc.gravity, doc.wind, doc.wind_direction = 5.0, 5.0, 0.0
rig.bone_constraint_set("tail", 0, own_gravity=True, gravity=0.0)
blown = direction(bones()["tail"])
assert blown < 0.98, ("own gravity only: the document's wind still blows it", blown)
rig.bone_constraint_set("tail", 0, own_wind=True)
assert abs(direction(bones()["tail"]) - 1.0) < 0.02, "and its own wind (none) too: nothing moves it"
rig.bone_constraint_set("tail", 0, own_wind=False)
# Weight is how hard gravity pulls; it leaves the wind alone. Wind Catch is how much the wind catches it.
rig.bone_constraint_set("tail", 0, weight=3.0)
assert abs(direction(bones()["tail"]) - blown) < 1e-4, "Weight doesn't change the wind's push"
rig.bone_constraint_set("tail", 0, weight=1.0, wind_catch=0.5)
half = direction(bones()["tail"])
assert blown + 0.01 < half < 0.995, ("half the catch: less bend", blown, half)
rig.bone_constraint_set("tail", 0, wind_catch=0.0)
assert abs(direction(bones()["tail"]) - 1.0) < 0.02, "no catch: the wind passes it by"
rig.bone_constraint_set("tail", 0, own_gravity=False)
light = direction(bones()["tail"])
rig.bone_constraint_set("tail", 0, weight=3.0)
assert direction(bones()["tail"]) < light - 0.01, ("heavier: gravity pulls it further", light)
rig.bone_constraint_set("tail", 0, weight=1.0, wind_catch=1.0)
doc.gravity, doc.wind = 0.0, 0.0

# Gusts and turbulence: the wind isn't steady, so the bone keeps moving (the same every time).
def sway(frames):
    out = []
    for f in frames:
        doc.frame_current = f
        out.append(direction(bones()["tail"]))
    return out


doc.wind = 5.0
steady = sway(range(60, 100, 4))
assert max(steady) - min(steady) < 1e-3, ("steady wind: it holds still", steady)
doc.wind_gusts = 0.5
gusty = sway(range(40, 120, 4))
assert max(gusty) - min(gusty) > 0.03, ("gusts: it keeps swaying", max(gusty) - min(gusty))
assert sway(range(40, 120, 4)) == gusty, "the same gusts every time"
doc.wind_gusts = 0.0
doc.wind_turbulence = 1.0
fluttery = sway(range(60, 80))
assert max(fluttery) - min(fluttery) > 0.01, ("turbulence: it flutters", max(fluttery) - min(fluttery))
doc.wind_turbulence = 0.0
# A bone's own gusts, while the document's wind is steady.
rig.bone_constraint_set("tail", 0, own_forces=True, wind=5.0, wind_gusts=0.8, wind_gust_speed=2.0)
own = sway(range(40, 120, 4))
assert max(own) - min(own) > 0.03, "its own gusts"
rig.bone_constraint_set("tail", 0, own_forces=False, wind=0.0, wind_gusts=0.0, wind_gust_speed=1.0)
doc.wind = 0.0
# Gravity, the wind and the dynamics settings are animatable: keyed with their key buttons (I and
# auto-keying leave settings alone until then), each frame simulated with its own value.
sim = oanim.data.simulation
doc.frame_current = 20
doc.gravity = 4.0
doc.frame_current = 21
assert sim.keyframes("gravity") == [], "editing a setting doesn't key it by itself"
doc.frame_current = 30
doc.gravity = 0.0
assert oanim.ops.anim.keyframe_toggle(layer=str(sim.id), paths="gravity") == {'FINISHED'}
doc.frame_current = 60
doc.gravity = 6.0  # animated now: the edit keys itself
doc.frame_current = 45
assert [k["frame"] for k in sim.keyframes("gravity")] == [0, 30, 60], sim.keyframes("gravity")
assert 0.0 < doc.gravity < 6.0, ("gravity between its keys", doc.gravity)
doc.frame_current = 30
calm = direction(bones()["tail"])
doc.frame_current = 90
assert direction(bones()["tail"]) < calm - 0.05, "heavier gravity later pulls it down"
for f in (60, 30, 0):
    doc.frame_current = f
    if any(k["frame"] == f for k in sim.keyframes("gravity")):
        assert oanim.ops.anim.keyframe_toggle(layer=str(sim.id), paths="gravity") == {'FINISHED'}
# With its keys gone it is what frame 0 had: the 4 it was set to (unkeyed) before it was keyed.
assert sim.keyframes("gravity") == [] and doc.gravity == 4.0, (sim.keyframes("gravity"), doc.gravity)
doc.gravity = 0.0
# A bone's own wind, and a spring setting, keyed the same way.
rig.bone_constraint_set("tail", 0, own_forces=True)
base = 'bones["tail"].constraints[0].'
doc.frame_current = 30
assert oanim.ops.anim.keyframe_toggle(layer=str(rig.id), paths=base + "wind") == {'FINISHED'}
doc.frame_current = 60
rig.bone_constraint_set("tail", 0, wind=6.0)  # keyed by the next record (it's animated)
assert oanim.ops.anim.keyframe_toggle(layer=str(rig.id), paths=base + "angle_spring.settle_speed") == {'FINISHED'}
assert [k["frame"] for k in rig.keyframes(base + "wind")] == [0, 30, 60]
assert [k["frame"] for k in rig.keyframes(base + "angle_spring.settle_speed")] == [0, 60]
doc.frame_current = 30
still = direction(bones()["tail"])
doc.frame_current = 90
assert direction(bones()["tail"]) < still - 0.05, "its wind rises after frame 30"
for path in ("wind", "angle_spring.settle_speed"):
    for f in (60, 30, 0):
        doc.frame_current = f
        if any(k["frame"] == f for k in rig.keyframes(base + path)):
            oanim.ops.anim.keyframe_toggle(layer=str(rig.id), paths=base + path)
assert rig.keyframes(base + "wind") == [] and rig.keyframes(base + "angle_spring.settle_speed") == []
rig.bone_constraint_set("tail", 0, own_forces=False, wind=0.0)

# A setting keyed differently on the setup frame (0) doesn't leak into the first frames: here a
# bone's own wind is 9.65 on frame 0, 0 on frame 1 and rises to 9 by frame 45, so from frame 1 the
# bone only bends further and further into the wind, never swinging back.
doc.frame_current = 0
add("mast", (48, 0), (48, 0.4))
add("streamer", (48, 0), (48, -0.4), parent="mast")
constrain("streamer", "DYNAMICS", scale_spring={"enabled": True})
rig.bone_constraint_set("streamer", 0, own_forces=True, wind=9.65, wind_direction=0.0)
own_wind = 'bones["streamer"].constraints[0].wind'
assert oanim.ops.anim.keyframe_toggle(layer=str(rig.id), paths=own_wind) == {'FINISHED'}
doc.frame_current = 1
rig.bone_constraint_set("streamer", 0, wind=0.0)
doc.frame_current = 45
rig.bone_constraint_set("streamer", 0, wind=9.0)
doc.frame_current = 46
assert [(k["frame"], round(k["value"], 2)) for k in rig.keyframes(own_wind)] == [(0, 9.65), (1, 0.0), (45, 9.0)]
bend = []
for f in range(1, 21):
    doc.frame_current = f
    bend.append(math.degrees(direction(bones()["streamer"])))
assert abs(bend[0] + 90.0) < 1.0, ("frame 1 starts in no wind", bend[0])
assert all(b >= a - 0.05 for a, b in zip(bend, bend[1:])), ("it only bends further into the wind", bend)

# It starts already bent by the wind and gravity, so it doesn't swing over into the bend.
doc.frame_current = 0
add("still", (40, 0), (40, 0.3))
add("dangle", (40, 0), (40, -0.3), parent="still")
add("dangle2", (40, -0.3), (40, -0.6), parent="dangle")
constrain("dangle", "DYNAMICS")
constrain("dangle2", "DYNAMICS")
doc.wind, doc.gravity = 5.0, 3.0
for name in ("dangle", "dangle2"):
    start = []
    for f in range(1, 16):
        doc.frame_current = f
        start.append(direction(bones()[name]))
    assert max(start) - min(start) < 1e-4 and start[0] > -math.pi / 2 + 0.05, (name, "no start-up swing", start)
doc.wind, doc.gravity = 0.0, 0.0
# Animated wind eases in and out of its keys in the simulation, so where it stops rising the bone
# settles in instead of being jolted into a bounce.
doc.frame_current = 0
add("pole", (44, 0), (44, 0.3))
add("flag", (44, 0), (44, -0.5), parent="pole")
constrain("flag", "DYNAMICS")
sim = oanim.data.simulation
doc.frame_current = 1
doc.wind = 0.0
assert oanim.ops.anim.keyframe_toggle(layer=str(sim.id), paths="wind") == {'FINISHED'}
doc.frame_current = 20
doc.wind = 9.0
doc.frame_current = 21
settle = []
for f in range(20, 60):
    doc.frame_current = f
    settle.append(direction(bones()["flag"]))
overshoot = max(settle) - settle[-1]
assert overshoot < math.radians(0.4), ("no jolt where the wind stops rising", math.degrees(overshoot))
for f in (20, 1, 0):
    doc.frame_current = f
    if any(k["frame"] == f for k in sim.keyframes("wind")):
        oanim.ops.anim.keyframe_toggle(layer=str(sim.id), paths="wind")
assert sim.keyframes("wind") == [] and doc.wind == 0.0
# Wind already rising when the simulation starts (keyed from frame 0): the bone starts moving with
# it, so its speed rises and eases off without a wobble catching up.
doc.frame_current = 0
doc.wind = 0.0
assert oanim.ops.anim.keyframe_toggle(layer=str(sim.id), paths="wind") == {'FINISHED'}
doc.frame_current = 20
doc.wind = 9.0
doc.frame_current = 21
angles = []
for f in range(1, 16):
    doc.frame_current = f
    angles.append(math.degrees(direction(bones()["flag"])))
speeds = [b - a for a, b in zip(angles, angles[1:])]
top = speeds.index(max(speeds))
assert all(b >= a - 0.05 for a, b in zip(speeds[:top], speeds[1:top + 1])), ("it speeds up smoothly", speeds)
assert all(b <= a + 0.05 for a, b in zip(speeds[top:], speeds[top + 1:])), ("then eases off, no wobble", speeds)
for f in (20, 0):
    doc.frame_current = f
    if any(k["frame"] == f for k in sim.keyframes("wind")):
        oanim.ops.anim.keyframe_toggle(layer=str(sim.id), paths="wind")
assert sim.keyframes("wind") == [] and doc.wind == 0.0

# Bounce: none never swings past; more swings further. Settle Speed: lower takes longer to calm.
def swing():
    angles = []
    for f in range(1, 40):
        doc.frame_current = f
        angles.append(direction(bones()["tail"]))
    return angles


dyn = rig.bone_constraints("tail")[-1]
spring = dyn["angle_spring"]
assert not spring["use_limit"] and abs(math.degrees(spring["limit"]) - 45) < 1e-3, "Max Swing off, 45 degrees ready"
assert close([spring[k] for k in ("settle_speed", "bounce", "inertia")], (1.0, 0.3, 1.0), 1e-6), spring
assert "weight" not in spring and (dyn["weight"], dyn["wind_catch"]) == (1.0, 1.0), "one Weight for the bone"
assert abs(dyn["scale_spring"]["stretch"] - 0.5) < 1e-6, dyn
rig.bone_constraint_set("tail", 0, angle_spring={"bounce": 0.0})
assert max(swing()) <= 1.0 + 1e-3, "no bounce: eases in"
rig.bone_constraint_set("tail", 0, angle_spring={"bounce": 0.6})
bouncy = max(swing())
assert bouncy > 1.05, ("bounce swings past", bouncy)
rig.bone_constraint_set("tail", 0, angle_spring={"bounce": 0.6, "settle_speed": 0.3})
slow = swing()
rig.bone_constraint_set("tail", 0, angle_spring={"settle_speed": 1.0})
fast = swing()
assert abs(slow[20] - 1.0) > 0.02 and abs(fast[20] - 1.0) < 0.01, ("slower to settle", slow[20], fast[20])
rig.bone_constraint_set("tail", 0, angle_spring={"bounce": 0.3})


def key_bone(name, frame, **values):
    doc.frame_current = frame
    rig.bone_set(name, **values)
    assert oanim.ops.anim.keyframe_insert() == {'FINISHED'}


def over(name, frames, measure):
    out = []
    for f in frames:
        doc.frame_current = f
        out.append(measure(bones()[name]))
    return out


DOWN = -math.pi / 2

# Inertia: moving the body without turning it swings what hangs from it -------------------------
doc.frame_current = 0
add("mover", (20, 0), (21, 0))
add("hair", (21, 0), (21, -1), parent="mover")
constrain("hair", "DYNAMICS")
key_bone("mover", 1, position=(20.0, 0.0))
key_bone("mover", 6, position=(23.0, 0.0))
moving = over("hair", range(1, 7), direction)
assert min(moving) < DOWN - 0.05, ("moving right, the tip stays behind (left)", moving)
after = over("hair", range(7, 25), direction)
assert max(after) > DOWN + 0.02, ("when it stops, it swings on past", after)
assert abs(after[-1] - DOWN) < 0.02, ("and settles", after)
rig.bone_constraint_set("tail", 0, angle_spring={"inertia": 0.0})
doc.frame_current = 3
assert abs(direction(bones()["tail"]) - direction(bones()["body"])) < 1e-3, "no inertia: it turns with the body"
rig.bone_constraint_set("tail", 0, angle_spring={"inertia": 1.0})
rig.bone_constraint_set("hair", 0, angle_spring={"inertia": 0.0})
assert max(abs(d - DOWN) for d in over("hair", range(1, 12), direction)) < 1e-3, "no inertia: it moves with the body"
# Max Swing: however hard it is flung, never further than its limit.
rig.bone_constraint_set("hair", 0, angle_spring={"inertia": 2.0, "use_limit": True, "limit": 0.1})
flung = [abs(d - DOWN) for d in over("hair", range(1, 20), direction)]
assert 0.05 < max(flung) < 0.1, ("max swing", max(flung))
rig.bone_constraint_set("hair", 0, angle_spring={"inertia": 1.0, "use_limit": False})
flung = [abs(d - DOWN) for d in over("hair", range(1, 20), direction)]
assert max(flung) > 0.1, ("unticked: no limit, the value kept", max(flung))
assert abs(rig.bone_constraints("hair")[0]["angle_spring"]["limit"] - 0.1) < 1e-6
# A Limit Angle above Dynamics limits what it aims at, not its swing (Dynamics is later in the stack).
doc.frame_current = 0
# Limit Angle holds turns from the setup angle (the Bone panel's Angle); the hair rests pointing down.
limit = constrain("hair", "LIMIT_ANGLE", min_angle=-0.01, max_angle=0.01)
swung = [abs(d - DOWN) for d in over("hair", range(1, 20), direction)]
assert max(swung) > 0.1, ("swings past the limit", max(swung))
rig.bone_constraint_set("hair", limit, min_angle=-1.0 - DOWN, max_angle=-0.9 - DOWN)
assert abs(over("hair", [40], direction)[0] + 1.0) < 0.02, "settles where the limit holds its aim"
doc.frame_current = 0
assert oanim.ops.armature.constraint_remove(index=str(limit), bone="hair") == {'FINISHED'}

# Stretch: moving along the bone squashes it (the tip stays behind) --------------------------------
doc.frame_current = 0
add("pusher", (20, 4), (21, 4))
add("spring", (21, 4), (22, 4), parent="pusher")
constrain("spring", "DYNAMICS", angle_spring={"enabled": False}, scale_spring={"enabled": True, "stretch": 1.0})
key_bone("pusher", 1, position=(20.0, 4.0))
key_bone("pusher", 6, position=(23.0, 4.0))
lengths = over("spring", range(1, 30), lambda b: math.dist(b["head"], b["tail"]))
assert min(lengths) < 0.95, ("pushed along, it squashes", min(lengths))
assert max(lengths) > 1.01 and abs(lengths[-1] - 1.0) < 0.01, ("then stretches past and settles", lengths)
assert abs(direction(bones()["spring"])) < 1e-4, "Angle off: it keeps its direction"

# Squash & Stretch feels forces too: the wind and gravity along the bone (by Weight), and its own
# swing, which pulls it longer the faster it goes (centrifugal, by Stretch).
def length_of(name):
    b = bones()[name]
    return math.dist(b["head"], b["tail"])


doc.frame_current = 0
add("rod", (20, 12), (21, 12))
constrain("rod", "DYNAMICS", angle_spring={"enabled": False}, scale_spring={"enabled": True})
add("drop", (24, 12), (24, 11))
constrain("drop", "DYNAMICS", angle_spring={"enabled": False}, scale_spring={"enabled": True})
doc.wind, doc.wind_direction = 5.0, 0.0
doc.frame_current = 30
assert abs(length_of("rod") - 1.1) < 0.01, ("the wind along it stretches it", length_of("rod"))
assert abs(length_of("drop") - 1.0) < 1e-3, "the wind across it doesn't"
doc.wind, doc.gravity = 0.0, 5.0
doc.frame_current = 31
assert abs(length_of("drop") - 1.1) < 0.01, ("gravity stretches a hanging bone", length_of("drop"))
rig.bone_constraint_set("drop", 0, weight=0.0)
assert abs(length_of("drop") - 1.0) < 1e-3, "no weight: gravity leaves it alone"
doc.gravity = 0.0
doc.frame_current = 0
add("whipper", (28, 12), (29, 12))
add("whip", (29, 12), (30, 12), parent="whipper")
constrain("whip", "DYNAMICS", scale_spring={"enabled": True})
key_bone("whipper", 1, angle=0.0)
key_bone("whipper", 5, angle=2.0)
whipped = over("whip", range(1, 30), lambda b: math.dist(b["head"], b["tail"]))
assert 1.1 < max(whipped) < 1.5 and abs(whipped[-1] - 1.0) < 0.01, ("a fast swing stretches it, then settles", whipped)
rig.bone_constraint_set("whip", 0, scale_spring={"stretch": 0.0})
assert max(over("whip", range(1, 30), lambda b: math.dist(b["head"], b["tail"]))) < 1.001, "no stretch: it keeps its length"

# Position: the head lags behind as the bone above moves, and Weight drops it ----------------------
doc.frame_current = 0
add("torso", (20, 8), (21, 8))
add("belly", (21, 8), (21.5, 8), parent="torso")
constrain("belly", "DYNAMICS", angle_spring={"enabled": False}, position_spring={"enabled": True})
key_bone("torso", 1, position=(20.0, 8.0))
key_bone("torso", 6, position=(23.0, 8.0))
doc.frame_current = 4
assert bones()["belly"]["head"][0] < bones()["torso"]["tail"][0] - 0.02, "the head lags behind"
doc.gravity = 5.0
doc.frame_current = 60
assert bones()["belly"]["head"][1] < 8.0 - 0.02, ("gravity drops it", bones()["belly"]["head"])
doc.gravity = 0.0

# Chains whip: each bone aims from where the one above has swung to, so the lag grows down it ------
doc.frame_current = 0
add("root2", (30, 0), (31, 0))
add("c1", (31, 0), (32, 0), parent="root2")
add("c2", (32, 0), (33, 0), parent="c1")
add("c3", (33, 0), (34, 0), parent="c2")
for name in ("c1", "c2", "c3"):
    constrain(name, "DYNAMICS")
key_bone("root2", 1, angle=0.0)
key_bone("root2", 6, angle=1.0)
doc.frame_current = 5
b = bones()
keyed = direction(b["root2"])
lags = [keyed - direction(b[n]) for n in ("c1", "c2", "c3")]
assert 0 < lags[0] < lags[1] < lags[2], ("the lag grows down the chain", lags)

# Moving the bone layer itself swings the hair too -------------------------------------------------
doc.frame_current = 1
rig.location_x = 0.0
doc.frame_current = 6
rig.location_x = 3.0
doc.frame_current = 4
assert direction(bones()["hair"]) < DOWN - 0.05, ("the layer moving swings it", direction(bones()["hair"]))
doc.frame_current = 1
rig.location_x = 0.0
doc.frame_current = 6
rig.location_x = 0.0

rig.bone_constraint_set("hair", 0, own_forces=True, gravity=2.0, wind=1.0, wind_direction=0.5)

# Bake: the swing becomes keys, and the springs stop.
doc.frame_end = 30
doc.frame_current = 4
swung = direction(bones()["tail"])
assert oanim.ops.armature.dynamics_bake() == {'FINISHED'}
assert not rig.bone_constraints("tail")[-1]["enabled"]
assert 4 in [k["frame"] for k in rig.keyframes('bones["tail"].angle')]
doc.frame_current = 4
assert abs(direction(bones()["tail"]) - swung) < 1e-3, "the keys hold the same motion"

# --- Reorder, remove, and Dynamics stays last ------------------------------------------------------
rig.select_bone("tail")
constrain("tail", "LIMIT_ANGLE")
assert [c["type"] for c in rig.bone_constraints("tail")] == ["LIMIT_ANGLE", "DYNAMICS"], "added above Dynamics"
assert oanim.ops.armature.constraint_move(index="0", direction="1", bone="tail") == {'CANCELLED'}
assert oanim.ops.armature.constraint_add(type="DYNAMICS", bone="tail") == {'CANCELLED'}, "one Dynamics"
constrain("tail", "COPY_SCALE")
assert [c["type"] for c in rig.bone_constraints("tail")] == ["LIMIT_ANGLE", "COPY_SCALE", "DYNAMICS"]
assert rig.bone_constraints("tail")[1]["name"] == "Copy Scale", "named after its type"
# Dragging a box: to a place in the stack, never below Dynamics.
assert oanim.ops.armature.constraint_move_to(index="1", to="0", bone="tail") == {'FINISHED'}
assert [c["type"] for c in rig.bone_constraints("tail")] == ["COPY_SCALE", "LIMIT_ANGLE", "DYNAMICS"]
assert oanim.ops.armature.constraint_move_to(index="0", to="2", bone="tail") == {'FINISHED'}
assert [c["type"] for c in rig.bone_constraints("tail")] == ["LIMIT_ANGLE", "COPY_SCALE", "DYNAMICS"]
assert oanim.ops.armature.constraint_move_to(index="2", to="0", bone="tail") == {'CANCELLED'}, "Dynamics stays put"
assert oanim.ops.armature.constraint_remove(index="0", bone="tail") == {'FINISHED'}
assert oanim.ops.armature.constraint_remove(index="0", bone="tail") == {'FINISHED'}
assert [c["type"] for c in rig.bone_constraints("tail")] == ["DYNAMICS"]

# --- Saved ---------------------------------------------------------------------------------------------
doc.frame_current = 0
constrain("perp", "LIMIT_ANGLE", min_angle=-0.2, max_angle=0.3)  # perp rests turned: old files convert
saved = {name: rig.bone_constraints(name) for name in ("arm", "follower", "hand", "tail", "hair")}
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_constraints_")) / "constraints.oanim")
doc.wind_gusts, doc.wind_gust_speed, doc.wind_turbulence = 0.3, 1.5, 0.2
doc.frame_current = 10
assert oanim.ops.anim.keyframe_toggle(layer=str(oanim.data.simulation.id), paths="wind") == {'FINISHED'}
rig.bone_constraint_set("hair", 0, angle_spring={"bounce": 0.4})
assert oanim.ops.anim.keyframe_toggle(layer=str(rig.id), paths='bones["hair"].constraints[0].angle_spring.bounce') == {'FINISHED'}
saved = {name: rig.bone_constraints(name) for name in saved}
saved_keys = (oanim.data.simulation.keyframes("wind"), rig.keyframes('bones["hair"].constraints[0].angle_spring.bounce'))
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
d = oanim.data.document
assert close((d.wind_gusts, d.wind_gust_speed, d.wind_turbulence), (0.3, 1.5, 0.2), 1e-6), "the wind is saved"
assert (oanim.data.simulation.keyframes("wind"),
        rig.keyframes('bones["hair"].constraints[0].angle_spring.bounce')) == saved_keys, "their keys are saved"
rig = next(l for l in oanim.data.layers if l.type == "BONE")
def same(a, b):
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, float):
        return abs(a - b) < 1e-5
    return a == b


for name, constraints in saved.items():
    loaded = rig.bone_constraints(name)
    assert len(loaded) == len(constraints) and all(same(x, y) for x, y in zip(loaded, constraints)), (name, loaded)

# Saved before Weight, with Torque (twice the bend's number): it becomes Weight.
import json
import zipfile


def with_torque(value):
    if isinstance(value, dict):
        if "angle_spring" in value:
            # Saved before the bone's one Weight (each channel had its own).
            for k in ("weight", "weight_keys", "wind_catch", "own_gravity", "own_wind"):
                value.pop(k, None)
        if "settle_speed" in value:
            for k in ("weight", "inertia", "stretch", "use_limit", "limit"):
                value.pop(k, None)
            value["torque"] = 4.0
        for v in value.values():
            with_torque(v)
    elif isinstance(value, list):
        for v in value:
            with_torque(v)


with zipfile.ZipFile(path) as z:
    data = json.loads(z.read("document.json"))
with_torque(data)


def old_scale(value):
    # Bone scale saved as one number, before X and Y.
    if isinstance(value, dict):
        if "strength" in value and isinstance(value.get("scale"), list):
            value["scale"] = 1.5
            for key in value.get("scale_keys", []):
                key["value"] = 1.5
        for v in value.values():
            old_scale(v)
    elif isinstance(value, list):
        for v in value:
            old_scale(v)


old_scale(data)


def old_limits(value):
    # Limit Angle saved as angles in the parent's space, before turns from the setup angle.
    if isinstance(value, dict):
        for c in value.get("constraints", []) if "setup_angle" in value else []:
            if "min_turn" in c:
                c["min_angle"] = c.pop("min_turn") + value["setup_angle"]
                c["max_angle"] = c.pop("max_turn") + value["setup_angle"]
        for v in value.values():
            old_limits(v)
    elif isinstance(value, list):
        for v in value:
            old_limits(v)


old_limits(data)
old = path.replace("constraints.oanim", "torque.oanim")
with zipfile.ZipFile(old, "w") as z:
    z.writestr("document.json", json.dumps(data))
assert oanim.ops.wm.open_mainfile(filepath=old, confirmed="1") == {'FINISHED'}
rig = next(l for l in oanim.data.layers if l.type == "BONE")
assert abs(rig.bone_constraints("tail")[-1]["weight"] - 2.0) < 1e-6, "Angle's Weight becomes the bone's"
assert close(next(b for b in rig.bones if b["name"] == "tail")["scale"], (1.5, 1.5)), "one number: both ways"
limit = next(c for c in rig.bone_constraints("perp") if c["type"] == "LIMIT_ANGLE")
assert abs(limit["min_angle"] + 0.2) < 1e-5 and abs(limit["max_angle"] - 0.3) < 1e-5,     ("old angles become the same turns", limit)

print("Bone constraint tests passed")
