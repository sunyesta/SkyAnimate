"""A 2 Axis Controller's fixes (the Bone tab's Fixes panel): poses labelled as they are kept (each action's frame), and
Snap to Fix Pose putting the knob (and any other action bone) where a pose keeps it, as an ordinary keyed move.

Run with:  OpenAnimate --background --python tests/test_controller_fixes.py
"""

import oanim

ops = oanim.ops


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


class Rig:
    """A knob "joy" playing ordinary actions Right (40 frames) and Up (20 frames), each moving the face's point."""

    def __init__(self):
        ops.wm.read_homefile(confirmed="1")
        self.doc = oanim.data.document
        self.doc.auto_key = True
        self.layers = oanim.data.layers
        ops.layer.new_bone()
        self.rig = self.layers.active
        ops.armature.bone_add(name="joy", head_x="-3.0", head_y="3.0", tail_x="-2.5", tail_y="3.0")
        self.rig.bone_set("joy", strength=0.0)
        ops.layer.new()
        self.face = self.layers.active
        self.face.add_curve([(0.0, 0.0), (1.0, 0.0)])
        ops.layer.move_to_group(group=str(self.rig.id))
        self.layers.active = self.rig
        self.ordinary("turn_r", 40, (2.0, 0.0))
        self.ordinary("look_up", 20, (0.0, 2.0))
        self.rig.select_bone("joy")
        assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
        for side, action in (("RIGHT", "turn_r"), ("UP", "look_up")):
            assert ops.armature.controller_side(side=side, action=action) == {'FINISHED'}, (side, action)

    def ordinary(self, name, frames, moved):
        assert ops.anim.action_new(name=name) == {'FINISHED'} and self.doc.action == name
        self.doc.frame_current = frames
        self.face.set_point(0, moved)
        self.doc.frame_current = frames + 1
        self.mainline()

    def bone(self, name):
        return next(b for b in self.rig.bones if b["name"] == name)

    def settle(self):
        self.doc.frame_current = self.doc.frame_current + 1
        self.doc.frame_current = self.doc.frame_current - 1

    def mainline(self):
        self.doc.action = ""
        self.doc.frame_current = 0

    def knob(self, x, y):
        self.rig.select_bone("joy")
        self.rig.bone_set("joy", knob=(x, y))
        self.settle()

    def fix(self, name):
        return next(f for f in self.rig.fix_actions if f["name"] == name)

    def knob_keys(self):
        return [(k["frame"], tuple(k["value"])) for k in self.rig.keyframes('bones["joy"].position') if k["frame"]]

    def two_poses(self):
        """A fix with poses at (30, 10) and (20, 15), keyed apart; back on the mainline at frame 50 after."""
        self.doc.frame_current = 50
        self.knob(30.0, 10.0)
        assert ops.anim.fix_action_new() == {'FINISHED'}
        name = self.doc.action
        self.face.set_point(1, (1.0, 0.5))
        self.settle()
        self.doc.action = ""
        self.doc.frame_current = 50
        self.knob(20.0, 15.0)
        assert ops.anim.fix_action_new() == {'FINISHED'} and self.doc.action == name
        self.face.set_point(1, (1.0, -0.5))
        self.settle()
        self.doc.action = ""
        self.doc.frame_current = 50
        assert len(self.fix(name)["poses"]) == 2, self.fix(name)
        return name


# --- Poses are labelled as they are kept: each action's frame, not a share of it ---------------------------------------
r = Rig()
name = r.two_poses()
labels = [p["label"] for p in r.fix(name)["poses"]]
assert labels == ["look_up 10 · turn_r 30", "look_up 15 · turn_r 20"], labels
assert [p["at"] for p in r.fix(name)["poses"]] == [[10.0, 30.0], [15.0, 20.0]], "and kept so (its drivers by name)"

# --- Snap to Fix Pose puts the knob where the pose keeps it, keyed as a move by hand -----------------------------------
r.doc.frame_current = 70
r.knob(5.0, 2.0)
assert ops.anim.fix_pose_snap(name=name, pose="1", layer=str(r.rig.id)) == {'FINISHED'}
r.settle()
assert close(r.bone("joy")["knob"], (30.0, 10.0)), r.bone("joy")
assert (70, (30.0, 10.0)) in r.knob_keys(), ("auto-key keys it", r.knob_keys())
f = r.fix(name)
assert close(f["strength"], 1.0) and f["nearest_pose"] == 0, ("the fix plays its pose there", f)
assert ops.anim.fix_pose_snap(name=name, pose="2") == {'FINISHED'}, "the bone layer the active layer is in by default"
r.settle()
assert close(r.bone("joy")["knob"], (20.0, 15.0)) and r.fix(name)["nearest_pose"] == 1, r.bone("joy")
# One undo step each.
ops.ed.undo()
r.settle()
assert close(r.bone("joy")["knob"], (30.0, 10.0)), ("undo takes it back to pose 1", r.bone("joy"))

# --- Where it isn't a move -----------------------------------------------------------------------------------------------
r.doc.frame_current = 0
assert ops.anim.fix_pose_snap(name=name, pose="1") == {'CANCELLED'}, "frame 0 is the setup pose"
assert close(r.bone("joy")["knob"], (0.0, 0.0)), r.bone("joy")
r.doc.frame_current = 70
assert ops.anim.fix_pose_snap(name=name, pose="3") == {'CANCELLED'}, "it has two poses"
assert ops.anim.fix_pose_snap(name=name, pose="0") == {'CANCELLED'}, "poses count from 1"
assert ops.anim.fix_pose_snap(name="nope", pose="1") == {'CANCELLED'}
assert ops.anim.fix_pose_snap(name="turn_r", pose="1") == {'CANCELLED'}, "an action that isn't a fix"
# Inside the fix its action bones are held at its poses: nothing to snap there.
assert ops.anim.action_set(name=name, frame=str(r.fix(name)["poses"][0]["frame"])) == {'FINISHED'}
before = r.knob_keys()
assert ops.anim.fix_pose_snap(name=name, pose="2") == {'CANCELLED'}, "not inside the fix"
assert r.knob_keys() == before
r.doc.action = ""

# --- Muting a fix from the panel is the Actions panel's Mute ------------------------------------------------------------
r.doc.frame_current = 70
assert ops.anim.fix_pose_snap(name=name, pose="1") == {'FINISHED'}
r.settle()
assert not close(tuple(r.face.shown()[0][1]), (1.0, 0.0)), ("the fix plays", r.face.shown()[0][1])
assert ops.anim.action_play_set(name=name, mute="1", layer=str(r.rig.id)) == {'FINISHED'}
r.settle()
assert close(tuple(r.face.shown()[0][1]), (1.0, 0.0)), ("muted, it doesn't", r.face.shown()[0][1])

# --- Removing the picked pose; the last one goes with its fix -----------------------------------------------------------
assert ops.anim.fix_pose_remove(name=name, pose="1") == {'FINISHED'}
assert [p["at"] for p in r.fix(name)["poses"]] == [[15.0, 20.0]], r.fix(name)
assert ops.anim.action_delete(name=name, layer=str(r.rig.id)) == {'FINISHED'}
assert not r.rig.fix_actions

# --- A label with a part frame shows one decimal ------------------------------------------------------------------------
r = Rig()
r.doc.frame_current = 50
r.knob(12.5, 7.0)
assert ops.anim.fix_action_new() == {'FINISHED'}
assert r.rig.fix_actions[0]["poses"][0]["label"] == "look_up 7 · turn_r 12.5", r.rig.fix_actions[0]["poses"]

print("test_controller_fixes: ok")
