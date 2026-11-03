"""A 2 Axis Controller's spots (the Bone tab's Spots panel): named places for its knob. Going to one is a keyed move
(Shift: held there), Ctrl lands the knob on the nearest spot, fix pose, rest or crossing of keys, a hover shows a pose
without moving anything, and spots work with fixes, Add Corners, files and Symmetrize.

Run with:  OpenAnimate --background --python tests/test_controller_spots.py
"""

import pathlib
import tempfile

import _oanim
import oanim

ops = oanim.ops


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


class Rig:
    """A knob "joy" playing ordinary actions Right (turn_r, 40 frames) and Up (look_up, 20 frames), each moving the
    face's first point."""

    def __init__(self, name="joy"):
        ops.wm.read_homefile(confirmed="1")
        self.name = name
        self.doc = oanim.data.document
        self.doc.auto_key = True
        self.layers = oanim.data.layers
        ops.layer.new_bone()
        self.rig = self.layers.active
        ops.armature.bone_add(name=name, head_x="-3.0", head_y="3.0", tail_x="-2.5", tail_y="3.0")
        self.rig.bone_set(name, strength=0.0)
        ops.layer.new()
        self.face = self.layers.active
        self.face.add_curve([(0.0, 0.0), (1.0, 0.0)])
        ops.layer.move_to_group(group=str(self.rig.id))
        self.layers.active = self.rig
        self.ordinary("turn_r", 40, (2.0, 0.0))
        self.ordinary("look_up", 20, (0.0, 2.0))
        self.rig.select_bone(name)
        assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
        for side, action in (("RIGHT", "turn_r"), ("UP", "look_up")):
            assert ops.armature.controller_side(side=side, action=action) == {'FINISHED'}, (side, action)

    def ordinary(self, name, frames, moved):
        assert ops.anim.action_new(name=name) == {'FINISHED'} and self.doc.action == name
        self.doc.frame_current = frames
        self.face.set_point(0, moved)
        self.doc.frame_current = frames + 1
        self.doc.action = ""
        self.doc.frame_current = 0

    def bone(self, name=None):
        return next(b for b in self.rig.bones if b["name"] == (name or self.name))

    def spots(self, name=None):
        return [(n, tuple(at)) for n, at in self.bone(name)["controller_spots"]]

    def settle(self):
        self.doc.frame_current = self.doc.frame_current + 1
        self.doc.frame_current = self.doc.frame_current - 1

    def knob(self, x, y, snap=False):
        self.rig.select_bone(self.name)
        if snap:
            self.rig.bone_set(self.name, knob=(x, y), knob_snap=True)
        else:
            self.rig.bone_set(self.name, knob=(x, y))
        self.settle()
        return tuple(self.bone()["knob"])

    def keys(self):
        return {k["frame"]: (tuple(k["value"]), k["interp"]) for k in self.rig.keyframes('bones["%s"].position' % self.name)
                if k["frame"]}

    def where(self):
        return tuple(self.face.shown()[0][0])

    def fix(self):
        assert len(self.rig.fix_actions) == 1, self.rig.fix_actions
        return self.rig.fix_actions[0]


# --- Adding, naming, ordering and removing ----------------------------------------------------------------------------
r = Rig()
r.doc.frame_current = 50
r.knob(30.0, 10.0)
assert ops.armature.controller_spot_add() == {'FINISHED'}
assert r.spots() == [("Spot 1", (30.0, 10.0))], ("where the knob is", r.spots())
assert ops.armature.controller_spot_add(name="Smile", x="5", y="0") == {'FINISHED'}
assert ops.armature.controller_spot_add(name="Smile", x="0", y="15") == {'FINISHED'}
assert [n for n, _ in r.spots()] == ["Spot 1", "Smile", "Smile 2"], r.spots()
assert ops.armature.controller_spot_rename(name="Smile 2", to="Smile") == {'CANCELLED'}, "names are the spot's own"
assert ops.armature.controller_spot_rename(name="Smile 2", to="Up") == {'FINISHED'}
assert ops.armature.controller_spot_order(name="Up", direction="UP") == {'FINISHED'}
assert [n for n, _ in r.spots()] == ["Spot 1", "Up", "Smile"], r.spots()
assert ops.armature.controller_spot_order(name="Spot 1", direction="UP") == {'CANCELLED'}, "already first"
assert ops.armature.controller_spot_remove(name="Spot 1") == {'FINISHED'}
assert ops.armature.controller_spot_remove(name="Spot 1") == {'CANCELLED'}
assert r.spots() == [("Up", (0.0, 15.0)), ("Smile", (5.0, 0.0))], r.spots()
assert not r.keys().get(50) or r.keys()[50][0] == (30.0, 10.0), "adding a spot keys nothing"
ops.ed.undo()
assert [n for n, _ in r.spots()] == ["Spot 1", "Up", "Smile"], ("one undo step each", r.spots())
assert ops.armature.controller_spot_remove(name="Spot 1") == {'FINISHED'}
assert ops.armature.controller_spot_add(bone="nope") == {'CANCELLED'}, "a controller by name, or the active bone"

# --- Saved with the file ----------------------------------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp()) / "spots.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
r.doc = oanim.data.document
r.rig = next(l for l in oanim.data.layers if l.name == r.rig.name)
r.face = next(l for l in r.rig.children if l.name == r.face.name)
assert r.spots() == [("Up", (0.0, 15.0)), ("Smile", (5.0, 0.0))], ("back as saved", r.spots())
oanim.data.layers.active = r.rig
r.doc.auto_key = True

# --- Going to a spot is a keyed move; Shift keys it held there --------------------------------------------------------
r.doc.frame_current = 70
r.knob(0.0, 0.0)
assert ops.armature.controller_spot_go(name="Smile", bone="joy") == {'FINISHED'}
r.settle()
assert close(r.bone()["knob"], (5.0, 0.0)), r.bone()
assert r.keys()[70][0] == (5.0, 0.0), ("auto-key keys it", r.keys())
assert close(r.where(), (0.25, 0.0)), ("its actions play there", r.where())
r.doc.auto_key = False
r.doc.frame_current = 80
assert ops.armature.controller_spot_go(name="Up", hold="1") == {'FINISHED'}
r.settle()
assert r.keys()[80] == ((0.0, 15.0), "STEP"), ("held: keyed even without auto-key, and it holds", r.keys())
r.doc.auto_key = True
r.doc.frame_current = 0
assert ops.armature.controller_spot_go(name="Smile") == {'CANCELLED'}, "frame 0 is the setup pose"
r.doc.frame_current = 70
assert ops.armature.controller_spot_go(name="Nope") == {'CANCELLED'}
ops.ed.undo()  # the refused ones made no step: this takes back the held key
r.settle()
assert 80 not in r.keys(), r.keys()

# --- Ctrl lands on the nearest spot, fix pose, rest or crossing of keys --------------------------------------------------
r.doc.frame_current = 90
r.knob(30.0, 10.0)
assert ops.anim.fix_action_new() == {'FINISHED'}, "a fix pose at (30, 10)"
r.doc.action = ""
r.doc.frame_current = 90
assert close(r.knob(28.0, 11.0, snap=True), (30.0, 10.0)), "onto the fix pose"
assert close(r.knob(6.0, 1.0, snap=True), (5.0, 0.0)), "onto the spot"
assert close(r.knob(37.0, 2.0, snap=True), (40.0, 0.0)), "where keys cross: turn_r's end, look_up at rest"
assert close(r.knob(2.0, 13.0, snap=True), (0.0, 15.0)), "the spot Up, nearer than look_up's end (0, 20)"
assert close(r.knob(1.0, 1.0, snap=True), (0.0, 0.0)), "rest"

# --- A hover shows the pose there and moves nothing ------------------------------------------------------------------------
r.doc.frame_current = 90
r.knob(0.0, 0.0)
keys_before, knob_before, where_before = r.keys(), r.bone()["knob"], r.where()
assert _oanim.controller_preview_points(r.face.id) is None, "no preview yet"
_oanim.controller_preview(r.rig.id, "joy", 5.0, 0.0)
shown = _oanim.controller_preview_points(r.face.id)
assert close(tuple(shown[0]), (0.25, 0.0)), ("the drawing as with the knob on Smile", shown)
r.settle()
assert r.keys() == keys_before and r.bone()["knob"] == knob_before and r.where() == where_before, "nothing moved"
r.doc.frame_current = 91
r.doc.frame_current = 90
assert r.keys() == keys_before, "nor got keyed as the frame changes"
_oanim.controller_preview(r.rig.id, "", 0.0, 0.0)
assert _oanim.controller_preview_points(r.face.id) is None, "cleared"

# --- Move Here: a fix pose on the spot goes with it only if asked --------------------------------------------------------
assert ops.armature.controller_spot_add(name="Fixed", x="30", y="10") == {'FINISHED'}
r.doc.frame_current = 90
r.knob(25.0, 12.0)
assert ops.armature.controller_spot_move(name="Fixed", fix="KEEP") == {'FINISHED'}
assert dict(r.spots())["Fixed"] == (25.0, 12.0) and r.fix()["poses"][0]["at"] == [10.0, 30.0], "the fix pose stays"
assert ops.armature.controller_spot_move(name="Smile", fix="FOLLOW") == {'FINISHED'}, "no fix pose on Smile: just moves"
assert ops.armature.controller_spot_add(name="Fixed2", x="30", y="10") == {'FINISHED'}
r.knob(25.0, 12.0)
assert ops.armature.controller_spot_move(name="Fixed2", fix="FOLLOW") == {'FINISHED'}
assert r.fix()["poses"][0]["at"] == [12.0, 25.0], ("the fix pose moves with it", r.fix())
# Somewhere one of its actions rests, a fix pose can't be: it stays.
r.knob(25.0, 0.0)
assert ops.armature.controller_spot_move(name="Fixed2", fix="FOLLOW") == {'FINISHED'}
assert dict(r.spots())["Fixed2"] == (25.0, 0.0) and r.fix()["poses"][0]["at"] == [12.0, 25.0], r.fix()

# --- Fix This Pose Here --------------------------------------------------------------------------------------------------
assert ops.armature.controller_spot_add(name="Corner", x="20", y="15") == {'FINISHED'}
r.doc.frame_current = 100
assert ops.armature.controller_spot_fix(name="Corner") == {'FINISHED'}
assert r.doc.action == r.fix()["name"], "the fix opens on the pose"
assert [p["at"] for p in r.fix()["poses"]] == [[12.0, 25.0], [15.0, 20.0]], r.fix()["poses"]
r.doc.action = ""
r.doc.frame_current = 100
assert close(r.bone()["knob"], (20.0, 15.0)) and r.keys()[100][0] == (20.0, 15.0), ("and the knob is there", r.keys())
assert ops.armature.controller_spot_fix(name="Up") == {'CANCELLED'}, "a spot on one side only: nothing to fix"

# --- Add Corners -------------------------------------------------------------------------------------------------------
r = Rig()
assert ops.armature.controller_spots_corners() == {'FINISHED'}
assert r.spots() == [("Rest", (0.0, 0.0)), ("turn_r", (40.0, 0.0)), ("look_up", (0.0, 20.0)),
                     ("turn_r + look_up", (40.0, 20.0))], r.spots()
assert ops.armature.controller_spots_corners() == {'CANCELLED'}, "each has one already"

# --- Symmetrize: the other side's spots, Left and Right swapped --------------------------------------------------------
r = Rig(name="joy_L")
assert ops.armature.controller_spot_add(name="Wink_L", x="30", y="10") == {'FINISHED'}
assert ops.armature.symmetrize() == {'FINISHED'}
assert r.spots("joy_R") == [("Wink_R", (-30.0, 10.0))], r.spots("joy_R")

print("test_controller_spots: ok")
