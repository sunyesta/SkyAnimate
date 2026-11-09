"""2 Axis Controllers with the rest of the rig tools: each case in a new document, a rig with two bones linked to a knob.

Run with:  OpenAnimate --background --python tests/test_controller_edges.py
"""

import math

import oanim

ops = oanim.ops
QUARTER = math.pi / 2.0


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


class Rig:
    def __init__(self, dials=("turn_r", "turn_l")):
        ops.wm.read_homefile(confirmed="1")
        self.doc = oanim.data.document
        self.doc.auto_key = True
        self.layers = oanim.data.layers
        ops.layer.new_bone()
        self.rig = self.layers.active
        ops.armature.bone_add(name="head", head_x="0.0", head_y="-1.0", tail_x="0.0", tail_y="-0.5")
        for i, name in enumerate(dials):
            ops.armature.select_none()
            ops.armature.bone_add(name=name, head_x="5.0", head_y=str(float(i)), tail_x="6.0", tail_y=str(float(i)))
            self.rig.bone_set(name, strength=0.0)
        ops.layer.new()
        self.face = self.layers.active
        self.face.add_curve([(0.0, 0.0), (1.0, 0.0)])
        ops.layer.move_to_group(group=str(self.rig.id))
        self.layers.active = self.rig

    def bone(self, name):
        return next(b for b in self.rig.bones if b["name"] == name)

    def turn_of(self, name):
        b = self.bone(name)
        return b["angle"] - b["setup_angle"]

    def settle(self):
        self.doc.frame_current = self.doc.frame_current + 1
        self.doc.frame_current = self.doc.frame_current - 1

    def where(self):
        return tuple(self.face.shown()[0][0])

    def mainline(self):
        self.doc.action = ""
        self.doc.frame_current = 0

    def keys(self, path):
        return [(k["frame"], k["value"]) for k in self.rig.keyframes(path)]

    def dial_action(self, name, frames, angle, moved):
        """Action bone action `name`: its bone turns `angle` by `frames`, taking the face's first point to `moved`."""
        self.rig.select_bone(name)
        assert ops.anim.action_new(name=name, action_bone="1") == {'FINISHED'} and self.doc.action == name
        self.doc.frame_current = frames
        self.rig.bone_set(name, angle=self.bone(name)["setup_angle"] + angle)
        self.face.set_point(0, moved)
        self.doc.frame_current = frames + 1
        self.mainline()

    def controller(self, name="joy", right="turn_r", left="turn_l"):
        ops.armature.select_none()
        ops.armature.bone_add(name=name, head_x="-3.0", head_y="3.0", tail_x="-2.5", tail_y="3.0")
        self.rig.select_bone(name)
        assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
        for side, action in (("RIGHT", right), ("LEFT", left)):
            if action:
                assert ops.armature.controller_side(side=side, action=action) == {'FINISHED'}, (side, action)

    def knob(self, x, y=0.0, name="joy"):
        self.rig.select_bone(name)
        self.rig.bone_set(name, knob=(x, y))
        self.settle()

    def standard(self):
        """turn_r turns a quarter over 90 frames, turn_l back a quarter over 60; joy plays them Right and Left."""
        self.dial_action("turn_r", 90, QUARTER, (1.0, 1.0))
        self.dial_action("turn_l", 60, -QUARTER, (-1.0, 1.0))
        self.controller()

    def knob_keyed(self, frame, x):
        self.doc.frame_current = frame
        self.knob(x)
        self.doc.frame_current = frame + 1
        self.doc.frame_current = frame


# --- Auto-Freeze Keys keys the knob where it is, not the linked bones -------------------------------------------------
r = Rig()
r.standard()
r.doc.auto_freeze_keys = True
r.knob_keyed(10, 45.0)
r.settle()
assert close(r.bone("joy")["knob"], (45.0, 0.0)), ("the knob's key stays", r.keys('bones["joy"].position'))
assert not r.keys('bones["turn_r"].angle') and not r.keys('bones["turn_l"].angle'), "a linked bone is the knob's"
assert not r.keys('bones["joy"].angle') and not r.keys('bones["joy"].scale'), "a controller has nothing to turn or scale"
assert not any(f == 10 for f, _ in r.keys('bones["head"].angle')), "only the channel edited: no other bone keyed"

# --- Key Together on a rig with a controller: the knob's keys stay (a linked dial gets no fillers to lose) -----------
r = Rig()
r.standard()
r.doc.auto_freeze_keys = False
r.knob_keyed(10, 45.0)
r.doc.frame_current = 14
r.rig.bone_set("head", angle=r.bone("head")["angle"] + 0.3)
r.doc.frame_current = 15
r.mainline()
frames_before = r.rig.key_frames
knob_before = [r.keys('bones["joy"].position')]
assert ops.anim.key_together_set(layer=str(r.rig.id), on="1") == {'FINISHED'}
r.settle()
r.doc.frame_current = 5
r.doc.frame_current = 0
assert r.rig.key_frames == frames_before, ("folding deletes no keys", frames_before, r.rig.key_frames)
r.doc.frame_current = 10
assert close(r.bone("joy")["knob"], (45.0, 0.0)), ("the knob holds on 10", r.bone("joy")["knob"])
assert not r.keys('bones["turn_r"].angle') and not r.keys('bones["turn_l"].angle'), "a linked dial is the knob's"

# --- Apply Pose as Rest Pose leaves the box where it rests ------------------------------------------------------------
r = Rig()
r.standard()
r.knob_keyed(10, 45.0)
setup = r.bone("joy")["setup_position"]
r.rig.select_bone("head")
r.rig.bone_set("head", angle=r.bone("head")["setup_angle"] + 0.5)
r.settle()
assert ops.armature.apply_pose_as_rest() == {'FINISHED'}
assert close(r.bone("joy")["setup_position"], setup), ("the knob's frames aren't a place", r.bone("joy")["setup_position"])
r.doc.frame_current = 10
r.settle()
assert close(r.bone("joy")["knob"], (45.0, 0.0)), r.bone("joy")
# A box on a parent whose rest moves keeps where it rests, and no key holding that move is read as frames.
r = Rig()
r.standard()
r.rig.select_bone("joy")
r.rig.select_bone("head", extend=True)
assert ops.armature.parent_set(type="OFFSET") == {'FINISHED'}
r.doc.frame_current = 10
r.rig.select_bone("head")
r.rig.bone_set("head", angle=r.bone("head")["setup_angle"] + 0.5)
r.settle()
assert ops.armature.apply_pose_as_rest() == {'FINISHED'}
r.doc.frame_current = 10
r.settle()
assert close(r.bone("joy")["knob"], (0.0, 0.0)), ("no knob key from the move", r.bone("joy"), r.keys('bones["joy"].position'))

# --- Copy Position with Follow Move: a controller rests ---------------------------------------------------------------
r = Rig()
r.standard()
ops.armature.select_none()
ops.armature.bone_add(name="follower", head_x="2.0", head_y="2.0", tail_x="2.5", tail_y="2.0")
r.rig.select_bone("follower")
assert ops.armature.constraint_add(type="COPY_POSITION", bone="follower") == {'FINISHED'}
r.rig.bone_constraint_set("follower", 0, target="joy", follow_move=True)
r.doc.frame_current = 10
head = r.bone("follower")["head"]
r.knob(45.0)
assert close(r.bone("follower")["head"], head), ("nothing follows a knob's frames", head, r.bone("follower")["head"])

# --- A linked bone converted: its actions leave the other controller, its keys come along -----------------------------
r = Rig()
r.standard()
r.knob_keyed(10, 45.0)
shown = r.where()
r.doc.frame_current = 0
r.rig.select_bone("turn_r")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert r.bone("joy")["controller_actions"]["RIGHT"] == "", ("an action has one controller", r.bone("joy"))
assert r.bone("turn_r")["controller_actions"]["RIGHT"] == "turn_r"
assert any(f == 10 and close(v, (45.0, 0.0)) for f, v in r.keys('bones["turn_r"].position')), r.keys('bones["turn_r"].position')
r.doc.frame_current = 10
r.settle()
assert close(r.where(), shown), ("nothing stops playing", r.where(), shown)

# --- A knob set from Python on frame 0 is refused, the box left alone -------------------------------------------------
r = Rig()
r.standard()
setup = r.bone("joy")["setup_position"]
r.rig.select_bone("joy")
try:
    r.rig.bone_set("joy", knob=(30.0, 0.0))
    refused = False
except ValueError:
    refused = True
assert refused, "frame 0 is the rig at rest"
r.settle()
assert close(r.bone("joy")["setup_position"], setup), r.bone("joy")

# --- A connected bone converted still moves its knob ------------------------------------------------------------------
r = Rig(dials=("turn_r",))
r.dial_action("turn_r", 90, QUARTER, (1.0, 1.0))
ops.armature.select_none()
ops.armature.bone_add(name="root", head_x="-3.0", head_y="3.0", tail_x="-2.5", tail_y="3.0")
ops.armature.bone_add(name="joy", head_x="-2.5", head_y="3.0", tail_x="-2.0", tail_y="3.0")
r.rig.select_bone("joy")
r.rig.select_bone("root", extend=True)
assert ops.armature.parent_set(type="CONNECTED") == {'FINISHED'}
r.rig.select_bone("joy")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="turn_r") == {'FINISHED'}
r.doc.frame_current = 10
r.rig.select_bone("joy")
head = r.bone("joy")["head"]
assert r.rig.drag_bone_ends([(0.1, 0.0)])
r.settle()
assert r.bone("joy")["knob"][0] > 1.0, ("dragging moves a connected controller's knob", r.bone("joy")["knob"])
assert close(r.bone("joy")["head"], head), "and the box stays on its parent's tip"

# --- A bone that turns out and back: the knob's frame plays, not the first frame with that turn -----------------------
r = Rig(dials=("wave",))
r.dial_action("wave", 30, 1.0, (1.0, 0.0))
r.doc.action = "wave"
r.doc.frame_current = 60
r.rig.bone_set("wave", angle=r.bone("wave")["setup_angle"] + 0.5)
r.face.set_point(0, (5.0, 0.0))
r.doc.frame_current = 61
r.mainline()
r.doc.action = "wave"
r.doc.frame_current = 60
at_60 = r.where()
r.mainline()
r.controller(right="wave", left=None)
r.doc.frame_current = 10
r.knob(60.0)
assert close(r.where(), at_60), ("the knob at 60 shows frame 60", r.where(), at_60)
assert close(r.bone("joy")["knob_playing"], (60.0, 0.0)), r.bone("joy")
# Turned by hand it picks the first frame with that turn, as any action bone does.
r.rig.select_bone("wave")
r.rig.bone_set("wave", angle=r.bone("wave")["setup_angle"] + 0.4)
r.settle()
assert r.bone("joy")["knob"][0] < 30.0 and close(r.turn_of("wave"), 0.4), r.bone("joy")

# --- A knob moved by an action another controller plays turns its bones ---------------------------------------------
r = Rig()
r.standard()
assert ops.anim.action_new(name="Puppet") == {'FINISHED'}
r.doc.frame_current = 20
r.knob(90.0)
r.doc.frame_current = 21
r.mainline()
ops.armature.select_none()
ops.armature.bone_add(name="master", head_x="-3.0", head_y="-3.0", tail_x="-2.5", tail_y="-3.0")
r.rig.select_bone("master")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="Puppet") == {'FINISHED'}
r.doc.frame_current = 10
r.knob(20.0, 0.0, "master")
assert close(r.bone("joy")["knob"], (90.0, 0.0), 0.05), r.bone("joy")
assert close(r.turn_of("turn_r"), QUARTER, 1e-3), ("the master's knob turns joy's bone", r.turn_of("turn_r"))
assert close(r.where(), (1.0, 1.0)), r.where()
r.knob(0.0, 0.0, "master")
assert close(r.turn_of("turn_r"), 0.0), r.turn_of("turn_r")

# --- Symmetrize with Mirror Action Bone Actions fills the other controller's sides -------------------------------------
r = Rig(dials=("look_L",))
r.dial_action("look_L", 20, 1.0, (0.5, 0.0))
r.controller(name="stick_L", right="look_L", left=None)
r.rig.select_bone("look_L")
r.rig.select_bone("stick_L", extend=True)
assert ops.armature.symmetrize(action_bone_actions="1") == {'FINISHED'}
other = r.bone("stick_R")
assert other["type"] == "CONTROLLER_2D", other
assert other["controller_actions"]["LEFT"] == "look_R", ("the mirrored action in the mirrored side", other)
assert r.bone("look_R")["controller"] == "stick_R"

# --- Keep Poses moving the box on frame 0 leaves its knob's keys -------------------------------------------------------
r = Rig()
r.standard()
r.knob_keyed(10, 45.0)
r.doc.frame_current = 0
r.rig.select_bone("joy")
assert ops.armature.translate(move_x="1.0", move_y="0.0", keep_poses="1") == {'FINISHED'}
assert close(r.bone("joy")["setup_position"], (-2.0, 3.0)), r.bone("joy")
r.doc.frame_current = 10
r.settle()
assert close(r.bone("joy")["knob"], (45.0, 0.0)), ("frames stay frames", r.bone("joy")["knob"])
assert close(r.turn_of("turn_r"), QUARTER / 2.0), r.turn_of("turn_r")

# --- Back to Regular: what it did to linked bones is theirs, no moves of its own left ----------------------------------
r = Rig()
r.standard()
r.knob_keyed(10, 45.0)
r.doc.frame_current = 0
r.rig.select_bone("joy")
assert ops.armature.bone_convert(type="REGULAR") == {'FINISHED'}
assert not r.keys('bones["joy"].position'), ("its keys were the bones'", r.keys('bones["joy"].position'))
assert [f for f, _ in r.keys('bones["turn_r"].angle')] == [0, 10]
r.doc.frame_current = 10
r.settle()
assert close(r.turn_of("turn_r"), QUARTER / 2.0), r.turn_of("turn_r")

print("controller edges ok")
