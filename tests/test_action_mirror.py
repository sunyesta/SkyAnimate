"""Bone Symmetrize's Mirror Action Bone Actions: an action's mirror image as a new action, the original left as it is. Turning the
new action bone one way must draw the mirror image of turning the old one the other way, layer by layer.

Run with:  OpenAnimate --background --python tests/test_action_mirror.py
"""

import math

import oanim

ops, doc, layers, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm
doc.auto_key = True

TURN = 1.2
EAR_L = [(0.6, 0.0), (0.9, 0.3), (1.2, 0.0)]
EAR_R = [(-x, y) for x, y in EAR_L]
FACE = [(0.5, -0.5), (0.5, 0.5), (0.0, 0.8), (-0.5, 0.5), (-0.5, -0.5), (0.0, -0.8)]
SIDE = [(0.0, 0.0), (0.2, 0.1), (0.5, 0.0), (0.1, -0.2)]
BACK = [(0.3, -0.2), (-0.3, -0.2), (0.0, 0.4)]


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def layer(name):
    return [l for l in layers.all() if l.name == name][0]


def rig_layer():
    return [l for l in layers.all() if l.type == 'BONE'][0]


def actions(rig):
    return [a['name'] for a in rig.actions]


def drawing(name, points, rig, closed=False):
    ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve(points, closed=closed)
    ops.layer.move_to_group(group=str(rig.id))
    return l


def build():
    """A rig with an action bone action that does a bit of everything."""
    ops.wm.read_homefile(confirmed="1")
    global doc, layers
    doc, layers = oanim.data.document, oanim.data.layers
    doc.auto_key = True
    ops.layer.new_bone()
    rig = layers.active
    rig.name = "Rig"
    ops.armature.bone_add(name="Turn_L", head_x="0.5", head_y="1.5", tail_x="1.0", tail_y="1.5")
    ops.armature.select_none()
    ops.armature.bone_add(name="Spine_X", head_x="0.0", head_y="-1.0", tail_x="0.0", tail_y="-0.5")
    ops.armature.bone_add(name="Arm_L", head_x="0.6", head_y="-1.0", tail_x="1.0", tail_y="-0.7")
    rig.select_bone("Spine_X")
    ops.armature.bone_add(name="Arm_R", head_x="-0.6", head_y="-1.0", tail_x="-1.0", tail_y="-0.7")
    # It only drives its action: nothing is weighted to it, as an action bone off to the side of a rig.
    rig.bone_set("Turn_L", strength=0.0)
    ear_l = drawing("Ear_L", EAR_L, rig)
    ear_r = drawing("Ear_R", EAR_R, rig)
    face = drawing("Face_X", FACE, rig, closed=True)
    back = drawing("Back", BACK, rig, closed=True)
    # Partners whose setup handles are very different: one a stub.
    tab_l = drawing("Tab_L", [(0.6, 1.0), (0.9, 1.2), (1.2, 1.0)], rig)
    tab_r = drawing("Tab_R", [(-0.6, 1.0), (-0.9, 1.2), (-1.2, 1.0)], rig)
    tab_l.move_handle(0, 1, "OUT", (1.1, 1.3))
    tab_r.move_handle(0, 1, "OUT", (-0.901, 1.201))
    # A point with no partner, on the face's upper right line.
    face.split_segment(0, 1, 0.5)
    # A switch of a symmetric drawing and one drawn side on.
    ops.layer.new_switch()
    switch = layers.active
    switch.name = "Noses"
    ops.layer.move_to_group(group=str(rig.id))
    front = drawing("Nose_X", [(-0.1, 0.0), (0.0, 0.2), (0.1, 0.0)], rig)
    ops.layer.move_to_group(group=str(switch.id))
    side = drawing("Side_X", SIDE, rig)
    ops.layer.move_to_group(group=str(switch.id))

    layers.active = rig
    rig.select_bone("Turn_L")
    setup = {b["name"]: b["setup_angle"] for b in rig.bones}
    assert ops.anim.action_new(name="Turn_L", action_bone="1") == {'FINISHED'} and doc.action == "Turn_L"
    doc.frame_current = 10
    rig.bone_set("Turn_L", angle=setup["Turn_L"] + TURN)
    rig.bone_set("Arm_L", angle=setup["Arm_L"] + 0.4, position=(0.7, -0.9))
    rig.bone_set("Arm_R", angle=setup["Arm_R"] - 0.1)
    rig.bone_set("Spine_X", angle=setup["Spine_X"] + 0.2, position=(0.1, -0.9))
    ear_l.set_point(1, (1.0, 0.5))
    ear_r.set_point(1, (-0.8, 0.2))
    face.set_point(0, (0.7, -0.4))
    face.set_point(3, (-0.2, 0.6))
    face.set_point(5, (0.05, -0.9))
    layer("Side_X").set_point(2, (0.6, 0.1))
    tab_l.move_handle(0, 1, "OUT", (1.1, 1.6))
    assert ops.layer.draw_order_set(layer=str(back.id), mode="FRONT", target=str(ear_r.id)) == {'FINISHED'}
    assert ops.layer.switch_set(layer=switch.name, child="Side_X") == {'FINISHED'}
    doc.frame_current = 11
    doc.frame_current = 0
    doc.action = ""
    layers.active = rig
    assert ops.armature.select_none() == {'FINISHED'}, "an undo step on the mainline to come back to"
    rig.select_bone("Turn_L")
    return rig


def bezier(l):
    points, curves = l.shown()
    out = []
    for cps, closed in curves:
        for a, b in list(zip(cps, cps[1:])) + ([(cps[-1], cps[0])] if closed else []):
            p0, p1, p2, p3 = points[a[0]], a[2], b[1], points[b[0]]
            for i in range(9):
                t = i / 8
                u = 1 - t
                out.append(tuple(u * u * u * p0[k] + 3 * u * u * t * p1[k] + 3 * u * t * t * p2[k] + t * t * t * p3[k] for k in (0, 1)))
    return out


def apart(a, b):
    near = lambda p, q: min(math.dist(p, x) for x in q)
    return max(max(near(p, b) for p in a), max(near(p, a) for p in b))


def posed(rig, left, right):
    names = {b["name"]: b for b in rig.bones}
    doc.frame_current = 20
    rig.bone_set("Turn_L", angle=names["Turn_L"]["setup_angle"] + left)
    rig.bone_set("Turn_R", angle=names["Turn_R"]["setup_angle"] + right)
    doc.frame_current = 21
    doc.frame_current = 20
    return {l.name: bezier(l) for l in layers.all() if l.type == 'VECTOR'}


# --- Off, as it comes: bones only ----------------------------------------------------------------------------------
rig = build()
assert ops.armature.symmetrize() == {'FINISHED'}
assert "Turn_R" in [b["name"] for b in rig.bones] and actions(rig) == ["Turn_L"]
args = wm.last_operation()[1]
assert args["action_bone_actions"] == "0", args

# Turned on in the ViewportActionPanel, it runs again in place of the first run.
assert wm.redo_last(action_bone_actions=True) == "FINISHED"
rig = rig_layer()
assert actions(rig) == ["Turn_L", "Turn_R"], actions(rig)
assert "Mirrored Turn_L into Turn_R" in wm.banner_report()[3], wm.banner_report()
assert doc.action == ""

# --- What the new action holds -------------------------------------------------------------------------------------
doc.action = "Turn_R"
angle = {k["frame"]: k["value"] for k in rig.keyframes('bones["Turn_R"].angle')}
assert close(angle[10], -TURN), ("the other way", angle)
arm_r = {k["frame"]: k["value"] for k in rig.keyframes('bones["Arm_R"].angle')}
arm_l = {k["frame"]: k["value"] for k in rig.keyframes('bones["Arm_L"].angle')}
assert close(arm_r[10], -0.4) and close(arm_l[10], 0.1), ("each side takes the other's", arm_r, arm_l)
spine = {k["frame"]: k["value"] for k in rig.keyframes('bones["Spine_X"].angle')}
assert close(spine[10], -0.2), ("a centred bone turns the other way inside itself", spine)
ear_keys = {k["frame"]: k["value"] for k in layer("Ear_R").keyframes("points[1].pos")}
assert close(ear_keys[10], (-0.1, 0.2)), ("Ear_L's change, reflected", ear_keys)
ear_keys = {k["frame"]: k["value"] for k in layer("Ear_L").keyframes("points[1].pos")}
assert close(ear_keys[10], (-0.1, -0.1)), ("and Ear_R's", ear_keys)
rule = [k for k in layer("Back").keyframes("draw_order")]
assert rule[-1]["frame"] == 10, rule
doc.frame_current = 10
assert layer("Back").draw_order == ("FRONT", "Ear_L"), layer("Back").draw_order
doc.frame_current = 0

# A handle comes across the same shape, whatever its partner's setup handle was (a stub here).
doc.frame_current = 10
_, _, tab_out = layer("Tab_R").curve_handles(0)[1]
point = layer("Tab_R").points[1]
assert close((tab_out[0] - point[0], tab_out[1] - point[1]), (-0.2, 0.4)), (tab_out, point)
doc.frame_current = 0

# The original is as it was.
doc.action = "Turn_L"
assert close({k["frame"]: k["value"] for k in rig.keyframes('bones["Turn_L"].angle')}[10], TURN)
doc.frame_current = 10
assert layer("Back").draw_order == ("FRONT", "Ear_R")
doc.frame_current = 0
doc.action = ""

# --- A drawing that can't pair up in a switch gets a flipped copy, picked by the new action ----------------------------
assert len([l for l in layers.all() if l.name == "Side_X Flipped"]) == 1
flipped = layer("Side_X Flipped")
assert close(flipped.points[2][:2], (-0.5, 0.0)), flipped.points
doc.action = "Turn_R"
doc.frame_current = 10
assert layer("Noses").switch_active == "Side_X Flipped", layer("Noses").switch_active
doc.frame_current = 0
doc.action = "Turn_L"
doc.frame_current = 10
assert layer("Noses").switch_active == "Side_X", "the original action goes on picking the original"
doc.frame_current = 0
doc.action = ""

# --- The invariant: each way, every drawing is the mirror image of its other side --------------------------------------
PARTNERS = {"Ear_L": "Ear_R", "Ear_R": "Ear_L", "Face_X": "Face_X", "Back": "Back", "Nose_X": "Nose_X", "Side_X": "Side_X Flipped"}
for share in (0.25, 0.5, 1.0):
    a = posed(rig, TURN * share, 0.0)
    b = posed(rig, 0.0, -TURN * share)
    for name, other in PARTNERS.items():
        d = apart([(-x, y) for x, y in a[name]], b[other])
        # The face has a point on one side only: one segment on the other side can't take on the bend it makes.
        assert d < (0.05 if name == "Face_X" else 1e-3), (share, name, other, d)
doc.frame_current = 0

# The point with no partner stays on its line in the mirrored action: on the mirror image of the line it has no
# partner on, not left behind at its setup place.
face = layer("Face_X")
extra = face.point_count - 1
doc.action = "Turn_L"
doc.frame_current = 10
line_there = [(-x, y) for x, y in bezier(face)]
doc.frame_current = 0
at_rest = tuple(face.shown()[0][extra])
doc.action = "Turn_R"
doc.frame_current = 10
moved = tuple(face.shown()[0][extra])
doc.frame_current = 0
doc.action = ""
assert min(math.dist(moved, p) for p in line_there) < 0.02, (moved, "not on the mirrored line")
assert not close(moved, at_rest, 0.05), ("it went with its line", moved, at_rest)

# --- Run again: it replaces the action, and the flipped copy stays one copy ----------------------------------------------
doc.action = "Turn_R"
doc.frame_current = 10
rig.bone_set("Arm_R", angle=9.0)
doc.frame_current = 11
doc.frame_current = 0
doc.action = ""
assert ops.armature.symmetrize(action_bone_actions="1") == {'FINISHED'}
rig = rig_layer()
assert "(replaced it)" in wm.banner_report()[3], wm.banner_report()
assert actions(rig) == ["Turn_L", "Turn_R"], actions(rig)
assert len([l for l in layers.all() if l.name == "Side_X Flipped"]) == 1
doc.action = "Turn_R"
assert close({k["frame"]: k["value"] for k in rig.keyframes('bones["Arm_R"].angle')}[10], -0.4)
doc.action = ""

# --- Direction: both picked and both there, it says which is copied ------------------------------------------------------
rig.select_bone("")
doc.action = "Turn_R"
doc.frame_current = 10
layer("Ear_R").set_point(1, (-1.4, 0.6))
doc.frame_current = 11
doc.frame_current = 0
doc.action = ""
assert ops.armature.symmetrize(action_bone_actions="1", direction="R_TO_L") == {'FINISHED'}
rig = rig_layer()
assert "Mirrored Turn_R into Turn_L" in wm.banner_report()[3], wm.banner_report()
doc.action = "Turn_L"
keys = {k["frame"]: k["value"] for k in layer("Ear_L").keyframes("points[1].pos")}
assert close(keys[10], (0.5, 0.3)), keys
# The other way round, the original drawing is what the switch picks, and it isn't remade.
doc.frame_current = 10
assert layer("Noses").switch_active == "Side_X", layer("Noses").switch_active
doc.frame_current = 0
doc.action = ""
assert close(layer("Side_X").points[2][:2], SIDE[2])

# --- One undo step, and it leaves no copy behind ---------------------------------------------------------------------
rig = build()
assert ops.armature.symmetrize(action_bone_actions="1") == {'FINISHED'}
assert actions(rig_layer()) == ["Turn_L", "Turn_R"]
ops.ed.undo()
assert actions(rig_layer()) == ["Turn_L"]
assert not [l for l in layers.all() if l.name == "Side_X Flipped"]

# Editing an action on frame 0: it goes back to it.
doc.action = "Turn_L"
rig = rig_layer()
layers.active = rig
rig.select_bone("Turn_L")
assert ops.armature.symmetrize(action_bone_actions="1") == {'FINISHED'}
assert doc.action == "Turn_L"
doc.action = ""

# Only on frame 0, as bone Symmetrize always was.
doc.frame_current = 5
assert ops.armature.symmetrize(action_bone_actions="1") == {'CANCELLED'}
doc.frame_current = 0

# Bones with no action bone action: the option has nothing to do, and says so.
rig = rig_layer()
layers.active = rig
rig.select_bone("Arm_L")
assert ops.armature.symmetrize(action_bone_actions="1") == {'FINISHED'}
assert "No action bone actions" in wm.banner_report()[3], wm.banner_report()
