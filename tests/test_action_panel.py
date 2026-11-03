"""The Armature tab's Actions panel: every action's Frame (0: not playing), Mute and Loop, an action with no bone played
by its own keyable Frame, one with a bone or knob put at a frame by turning or moving it.

Run with:  OpenAnimate --background --python tests/test_action_panel.py
"""

import math
import pathlib
import tempfile

import oanim

ops = oanim.ops
QUARTER = math.pi / 2.0


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


class Rig:
    def __init__(self):
        ops.wm.read_homefile(confirmed="1")
        self.doc = oanim.data.document
        self.doc.auto_key = True
        self.layers = oanim.data.layers
        ops.layer.new_bone()
        self.rig = self.layers.active
        ops.armature.bone_add(name="dial", head_x="5.0", head_y="0.0", tail_x="6.0", tail_y="0.0")
        self.rig.bone_set("dial", strength=0.0)
        ops.layer.new()
        self.face = self.layers.active
        self.face.add_curve([(0.0, 0.0), (1.0, 0.0)])
        ops.layer.move_to_group(group=str(self.rig.id))
        self.layers.active = self.rig

    def bone(self, name):
        return next(b for b in self.rig.bones if b["name"] == name)

    def settle(self):
        self.doc.frame_current = self.doc.frame_current + 1
        self.doc.frame_current = self.doc.frame_current - 1

    def where(self):
        return tuple(self.face.shown()[0][0])

    def mainline(self):
        self.doc.action = ""
        self.doc.frame_current = 0

    def action(self, name):
        return next(a for a in self.rig.actions if a["name"] == name)

    def keys(self, path):
        try:
            return [(k["frame"], k["value"]) for k in self.rig.keyframes(path)]
        except KeyError:
            return []  # no such channel

    def wave(self, frames=10, moved=(1.0, 1.0)):
        """An action with no bone: the face's first point goes to `moved` by `frames`."""
        assert ops.anim.action_new(name="wave") == {'FINISHED'} and self.doc.action == "wave"
        self.doc.frame_current = frames
        self.face.set_point(0, moved)
        self.doc.frame_current = frames + 1
        self.mainline()

    def dial_action(self, frames=20, angle=QUARTER, moved=(0.0, 2.0)):
        self.rig.select_bone("dial")
        assert ops.anim.action_new(name="dial", action_bone="1") == {'FINISHED'} and self.doc.action == "dial"
        self.doc.frame_current = frames
        self.rig.bone_set("dial", angle=self.bone("dial")["setup_angle"] + angle)
        self.face.set_point(0, moved)
        self.doc.frame_current = frames + 1
        self.mainline()

    def play(self, name, **values):
        args = {k: str(v) for k, v in values.items()}
        return ops.anim.action_play_set(name=name, **args)


# --- An action with no bone plays at its own Frame -----------------------------------------------------------------
r = Rig()
r.wave()
a = r.action("wave")
assert a["frame_by"] == "OWN" and a["holder"] == "" and a["frame"] == 0.0 and a["length"] == 10.0, a
r.doc.frame_current = 30
r.settle()
assert close(r.where(), (0.0, 0.0)), ("at 0 it doesn't play", r.where())
assert r.play("wave", frame=10) == {'FINISHED'}
r.settle()
assert close(r.action("wave")["frame"], 10.0)
assert close(r.where(), (1.0, 1.0)), ("its last frame plays", r.where())
r.play("wave", frame=5)
r.settle()
x, y = r.where()
assert 0.05 < x < 0.95 and close(x, y), ("between frames it blends", r.where())
# Keyed, as any keyable value: auto-key keys its Frame on this frame, from 0 on the setup frame.
assert r.keys('actions["wave"].frame') == [(0, 0.0), (30, 5.0)], r.keys('actions["wave"].frame')
r.doc.frame_current = 15
r.settle()
assert close(r.action("wave")["frame"], 2.5) and 0.0 < r.where()[0] < x, (r.action("wave"), r.where())
# Nothing plays on frame 0, so its Frame isn't set there.
r.doc.frame_current = 0
assert r.play("wave", frame=3) == {'CANCELLED'}

# --- Past its end it holds the last frame, or with Loop starts over ----------------------------------------------------
r = Rig()
r.wave()
r.doc.frame_current = 30
r.play("wave", frame=15)
r.settle()
assert close(r.where(), (1.0, 1.0)), ("held at its end", r.where())
assert not r.action("wave")["loops"] and r.action("wave")["loop"] == "AUTO"
r.play("wave", frame=5)
r.settle()
at5 = r.where()
r.play("wave", loop="ON", frame=15)
r.settle()
assert r.action("wave")["loops"] and r.action("wave")["loop"] == "ON"
assert close(r.where(), at5), ("15 of a 10-frame loop is its frame 5", r.where(), at5)
r.play("wave", frame=20)
r.settle()
assert close(r.where(), (1.0, 1.0)), ("a whole number of rounds ends on its last frame, not its first", r.where())

# --- Mute: it doesn't play, whatever its frame, and comes back unmuted -------------------------------------------------
r = Rig()
r.wave()
r.doc.frame_current = 30
r.play("wave", frame=10)
r.settle()
assert r.play("wave", mute=1) == {'FINISHED'}
r.settle()
assert r.action("wave")["mute"] and close(r.where(), (0.0, 0.0)), r.where()
r.play("wave", mute=0)
r.settle()
assert close(r.where(), (1.0, 1.0)), r.where()

# --- An action bone action: its Frame is its bone's, setting it turns the bone, the keys on the bone ---------------
r = Rig()
r.dial_action()
a = r.action("dial")
assert a["frame_by"] == "BONE" and a["holder"] == "dial" and a["length"] == 20.0, a
r.doc.frame_current = 30
r.rig.select_bone("dial")
r.rig.bone_set("dial", angle=r.bone("dial")["setup_angle"] + QUARTER / 2.0)
r.settle()
shown = r.action("dial")["frame"]
assert 1.0 < shown < 19.0, ("the bone's turn picks its frame", shown)
r.mainline()
r.doc.frame_current = 40
assert r.play("dial", frame=20) == {'FINISHED'}
r.settle()
assert close(r.bone("dial")["angle"] - r.bone("dial")["setup_angle"], QUARTER), r.bone("dial")
assert close(r.action("dial")["frame"], 20.0) and close(r.where(), (0.0, 2.0)), (r.action("dial"), r.where())
assert any(f == 40 for f, _ in r.keys('bones["dial"].angle')), ("the bone holds the key", r.keys('bones["dial"].angle'))
assert not r.keys('actions["dial"].frame'), "no Frame keys of its own"
# Muted, the bone turns but plays nothing; the panel still shows where its bone has it.
r.play("dial", mute=1)
r.settle()
assert close(r.where(), (0.0, 0.0)) and close(r.action("dial")["frame"], 20.0), (r.where(), r.action("dial"))
r.play("dial", mute=0)

# --- A 360 action bone action loops by itself, and Loop off holds it at its end ---------------------------------------
r = Rig()
r.dial_action(frames=40, angle=2.0 * math.pi, moved=(0.0, 0.0))
assert r.action("dial")["loops"] and r.action("dial")["loop"] == "AUTO", r.action("dial")
r.doc.frame_current = 10
r.rig.select_bone("dial")
r.rig.bone_set("dial", angle=r.bone("dial")["setup_angle"] + 2.0 * math.pi + QUARTER)
r.settle()
assert close(r.action("dial")["frame"], 10.0, 0.1), ("a turn and a quarter: a quarter of it", r.action("dial"))
r.play("dial", loop="OFF")
r.settle()
assert not r.action("dial")["loops"] and close(r.action("dial")["frame"], 40.0, 0.1), r.action("dial")

# --- A controller's action: its Frame is where the knob has it, setting it moves the knob ---------------------------
r = Rig()
r.dial_action()
r.wave()
ops.armature.select_none()
ops.armature.bone_add(name="joy", head_x="-3.0", head_y="3.0", tail_x="-2.5", tail_y="3.0")
r.rig.select_bone("joy")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="UP", action="dial") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="wave") == {'FINISHED'}
a = r.action("dial")
assert a["frame_by"] == "KNOB" and a["holder"] == "joy", a
assert r.action("wave")["frame_by"] == "KNOB", "an action in a side is the knob's too"
r.doc.frame_current = 10
r.rig.select_bone("joy")
r.rig.bone_set("joy", knob=(7.0, 0.0))
r.settle()
assert r.play("dial", frame=12) == {'FINISHED'}
r.settle()
assert close(r.bone("joy")["knob"], (7.0, 12.0)), ("along its side, the other axis kept", r.bone("joy")["knob"])
assert close(r.action("dial")["frame"], 12.0), r.action("dial")
r.play("dial", frame=500)
r.settle()
assert close(r.bone("joy")["knob"], (7.0, 20.0)), ("no further than the box's edge", r.bone("joy")["knob"])

# --- Order, rename, delete, and saving ---------------------------------------------------------------------------------
r = Rig()
r.wave()
assert ops.anim.action_new(name="blink") == {'FINISHED'}
r.mainline()
assert [a["name"] for a in r.rig.actions] == ["wave", "blink"]
assert ops.anim.action_move(name="blink", direction="UP") == {'FINISHED'}
assert [a["name"] for a in r.rig.actions] == ["blink", "wave"]
assert ops.anim.action_move(name="blink", direction="UP") == {'CANCELLED'}, "already first"
r.doc.frame_current = 30
r.play("wave", frame=5, loop="ON")
r.play("blink", mute=1)
assert ops.anim.action_rename(name="wave", to="hello") == {'FINISHED'}
a = r.action("hello")
assert close(a["frame"], 5.0) and a["loop"] == "ON", ("its Frame and Loop go with it", a)
assert r.keys('actions["hello"].frame'), "its Frame keys too"
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_action_panel_")) / "action_panel.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
r.doc = oanim.data.document
r.rig = next(l for l in oanim.data.layers if l.name == r.rig.name)
r.doc.frame_current = 30
a, b = r.action("hello"), r.action("blink")
assert close(a["frame"], 5.0) and a["loop"] == "ON" and not a["mute"] and b["mute"], (a, b)
assert r.keys('actions["hello"].frame') == [(0, 0.0), (30, 5.0)], r.keys('actions["hello"].frame')
assert ops.anim.action_delete(name="hello", layer=str(r.rig.id)) == {'FINISHED'}
assert [a["name"] for a in r.rig.actions] == ["blink"]
assert not r.keys('actions["hello"].frame'), "its Frame went with it"
assert ops.anim.action_new(name="hello") == {'FINISHED'}
r.mainline()
assert r.action("hello")["frame"] == 0.0 and r.action("hello")["loop"] == "AUTO", "a new one starts at rest"

# --- Clear Frames puts every own Frame back to 0 ----------------------------------------------------------------------
r = Rig()
r.wave()
r.doc.frame_current = 30
r.play("wave", frame=7)
assert ops.anim.action_frames_clear() == {'FINISHED'}
r.settle()
assert r.action("wave")["frame"] == 0.0 and close(r.where(), (0.0, 0.0)), (r.action("wave"), r.where())

print("action panel: ok")
