"""The Actions panel with the rest of the app: undo, actions playing actions, fixes, bone renames, Symmetrize, files,
duplicates and dynamics. Each case in a new document.

Run with:  OpenAnimate --background --python tests/test_action_panel_edges.py
"""

import math
import pathlib
import tempfile

import _oanim
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
        ops.armature.select_none()
        ops.armature.bone_add(name="arm", head_x="0.0", head_y="-3.0", tail_x="1.0", tail_y="-3.0")
        self.rig.bone_set("arm", strength=0.0)
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
            return []

    def own(self, name, frames=10, moved=(1.0, 1.0)):
        assert ops.anim.action_new(name=name) == {'FINISHED'} and self.doc.action == name
        self.doc.frame_current = frames
        self.face.set_point(0, moved)
        self.doc.frame_current = frames + 1
        self.mainline()

    def dial_action(self, name="dial", frames=20, angle=QUARTER, moved=(0.0, 2.0)):
        self.rig.select_bone(name)
        assert ops.anim.action_new(name=name, action_bone="1") == {'FINISHED'} and self.doc.action == name
        self.doc.frame_current = frames
        self.rig.bone_set(name, angle=self.bone(name)["setup_angle"] + angle)
        if moved is not None:
            self.face.set_point(0, moved)
        self.doc.frame_current = frames + 1
        self.mainline()

    def play(self, name, **values):
        return ops.anim.action_play_set(name=name, **{k: str(v) for k, v in values.items()})


# --- Undo and redo a Frame --------------------------------------------------------------------------------------------
def undo_redo_frame():
    r = Rig()
    r.own("wave")
    r.doc.frame_current = 30
    r.play("wave", frame=10)
    r.settle()
    assert close(r.where(), (1.0, 1.0)), r.where()
    ops.ed.undo()
    r.settle()
    assert close(r.where(), (0.0, 0.0)) and r.action("wave")["frame"] == 0.0, ("undone", r.where(), r.action("wave"))
    ops.ed.redo()
    r.settle()
    assert close(r.where(), (1.0, 1.0)), ("redone", r.where())


undo_redo_frame()


# --- Undo Mute and Loop -----------------------------------------------------------------------------------------------
def undo_mute_and_loop():
    r = Rig()
    r.own("wave")
    r.doc.frame_current = 30
    r.play("wave", frame=10)
    r.play("wave", mute=1)
    r.play("wave", loop="ON")
    ops.ed.undo()
    assert r.action("wave")["loop"] == "AUTO" and r.action("wave")["mute"]
    ops.ed.undo()
    r.settle()
    assert not r.action("wave")["mute"] and close(r.where(), (1.0, 1.0)), (r.action("wave"), r.where())


undo_mute_and_loop()


# --- An action keying another's Frame plays it ------------------------------------------------------------------------
def nested_own_actions():
    """Action `outer` keys `inner`'s Frame: playing outer plays inner."""
    r = Rig()
    r.own("inner")
    assert ops.anim.action_new(name="outer") == {'FINISHED'}
    r.doc.frame_current = 10
    assert r.play("inner", frame=10) == {'FINISHED'}, "an action can key another's Frame"
    r.doc.frame_current = 11
    r.mainline()
    assert r.keys('actions["inner"].frame') == [], ("its keys are outer's, not the mainline's", r.keys('actions["inner"].frame'))
    r.doc.frame_current = 30
    r.play("outer", frame=10)
    r.settle()
    assert close(r.where(), (1.0, 1.0)), ("outer at its end plays inner at its end", r.where(), r.rig.actions)
    r.play("outer", frame=5)
    r.settle()
    x = r.where()[0]
    assert 0.05 < x < 0.95, ("outer half way plays inner part way", r.where())
    # Stable: worked out again and again, the same.
    for _ in range(3):
        r.settle()
    assert close(r.where()[0], x), ("no drift", r.where(), x)


nested_own_actions()


# --- Renaming a bone keeps its action's Mute and Loop -----------------------------------------------------------------
def bone_rename_keeps_settings():
    r = Rig()
    r.dial_action()
    r.doc.frame_current = 10
    r.play("dial", mute=1, loop="ON")
    r.rig.select_bone("dial")
    _oanim.layer_bone_set(r.rig.id, "dial", {"name": "knobby"})
    a = r.action("knobby")
    assert a["mute"] and a["loop"] == "ON", a


bone_rename_keeps_settings()


# --- Symmetrize: the other side loops as this one does ----------------------------------------------------------------
def mirror_keeps_loop():
    r = Rig()
    ops.armature.select_none()
    ops.armature.bone_add(name="eye_L", head_x="2.0", head_y="2.0", tail_x="3.0", tail_y="2.0")
    r.rig.bone_set("eye_L", strength=0.0)
    r.dial_action("eye_L")
    r.doc.frame_current = 10
    r.play("eye_L", loop="ON")
    r.doc.frame_current = 0
    r.rig.select_bone("eye_L")
    res = ops.armature.symmetrize(action_bone_actions="1")
    names = [a["name"] for a in r.rig.actions]
    assert "eye_R" in names, (res, names)
    assert r.action("eye_R")["loop"] == "ON", r.action("eye_R")


mirror_keeps_loop()


# --- Muting a fix, or one of its action bones -------------------------------------------------------------------------
def muted_fix_and_driver():
    r = Rig()
    r.dial_action("dial", moved=(0.0, 2.0))
    r.dial_action("arm", moved=(2.0, 0.0))
    r.doc.frame_current = 30
    r.play("dial", frame=20)
    r.play("arm", frame=20)
    r.settle()
    together = r.where()
    assert ops.anim.fix_action_new() == {'FINISHED'}, "a fix for both"
    fix = r.doc.action
    r.face.set_point(0, (together[0] + 1.0, together[1] + 1.0))
    r.doc.frame_current = r.doc.frame_current + 1
    r.doc.action = ""
    r.doc.frame_current = 30
    r.settle()
    fixed = r.where()
    assert not close(fixed, together), ("the fix plays", fixed, together)
    assert r.action(fix)["frame_by"] == "FIX" and close(r.action(fix)["frame"], 1.0), r.action(fix)
    r.play(fix, mute=1)
    r.settle()
    assert close(r.where(), together), ("a muted fix doesn't play", r.where(), together)
    r.play(fix, mute=0)
    r.play("arm", mute=1)
    r.settle()
    assert close(r.where(), (0.0, 2.0)), ("a muted driver: neither it nor its fix", r.where())
    assert r.play(fix, frame=3) == {'CANCELLED'}, "a fix's frame isn't set"


muted_fix_and_driver()


# --- Loop on an action bone action that isn't a whole turn ------------------------------------------------------------
def loop_on_plain_bone():
    r = Rig()
    r.dial_action(angle=QUARTER, frames=20, moved=(0.0, 2.0))
    r.doc.frame_current = 30
    r.rig.select_bone("dial")
    r.rig.bone_set("dial", angle=r.bone("dial")["setup_angle"] + QUARTER * 1.5)
    r.settle()
    assert close(r.action("dial")["frame"], 20.0, 0.05), ("held at the end", r.action("dial"))
    r.play("dial", loop="ON")
    r.settle()
    assert close(r.action("dial")["frame"], 10.0, 0.3), ("half way into its second round", r.action("dial"))
    r.play("dial", frame=30)
    r.settle()
    assert close(r.action("dial")["frame"], 10.0, 0.3), ("frame 30 of a 20-frame loop", r.action("dial"), r.bone("dial"))


loop_on_plain_bone()


# --- Saved while an action is edited ----------------------------------------------------------------------------------
def save_while_editing_action():
    r = Rig()
    r.own("wave")
    r.doc.frame_current = 30
    r.play("wave", frame=4)
    r.doc.frame_current = 40
    r.play("wave", frame=10)
    assert ops.anim.action_set(name="wave") == {'FINISHED'}
    path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_ap_")) / "editing.oanim")
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
    r.doc = oanim.data.document
    r.rig = next(l for l in oanim.data.layers.all() if l.type == "BONE")
    r.face = next(l for l in oanim.data.layers.all() if l.type == "VECTOR")
    assert r.doc.action == "wave", r.doc.action
    r.doc.action = ""
    assert r.keys('actions["wave"].frame') == [(0, 0.0), (30, 4.0), (40, 10.0)], r.keys('actions["wave"].frame')
    r.doc.frame_current = 40
    r.settle()
    assert close(r.where(), (1.0, 1.0)), r.where()


save_while_editing_action()


# --- A duplicated bone layer has its own Frames -----------------------------------------------------------------------
def duplicate_rig():
    r = Rig()
    r.own("wave")
    r.doc.frame_current = 30
    r.play("wave", frame=10, loop="ON")
    r.layers.active = r.rig
    assert ops.layer.duplicate() == {'FINISHED'}
    copy = r.layers.active
    assert copy.id != r.rig.id
    a = next(x for x in copy.actions if x["name"] == "wave")
    assert close(a["frame"], 10.0) and a["loop"] == "ON", a
    r.play("wave", frame=3, layer=r.rig.id)
    assert close(next(x for x in copy.actions if x["name"] == "wave")["frame"], 10.0), "copies are apart"


duplicate_rig()


# --- Dynamics follow a bone an action turns at its Frame --------------------------------------------------------------
def own_action_turns_bone_with_dynamics():
    """An own action turning a bone: dynamics hanging from it follow (action_bone_actions_add_at)."""
    r = Rig()
    ops.armature.select_none()
    ops.armature.bone_add(name="strand", head_x="1.0", head_y="-3.0", tail_x="2.0", tail_y="-3.0", parent="arm")
    r.rig.select_bone("strand")
    assert ops.armature.constraint_add(type="DYNAMICS") == {'FINISHED'}
    assert ops.anim.action_new(name="swing") == {'FINISHED'}
    r.doc.frame_current = 10
    r.rig.bone_set("arm", angle=r.bone("arm")["setup_angle"] + 1.0)
    r.doc.frame_current = 11
    r.mainline()
    r.doc.frame_current = 1
    r.play("swing", frame=0.1)
    r.doc.frame_current = 8
    r.play("swing", frame=10)
    r.doc.frame_current = 40
    r.settle()
    live = r.bone("strand")["tail"]
    keyed = next(b for b in r.rig.bones_keyed if b["name"] == "strand")["tail"]
    arm = r.bone("arm")
    assert close(arm["angle"] - arm["setup_angle"], 1.0, 0.01), ("the own action turns arm", arm)
    off = abs(live[0] - keyed[0]) + abs(live[1] - keyed[1])
    assert off < 0.05, ("settled onto the turned arm", live, keyed)
    swing = []
    for f in range(1, 20):
        r.doc.frame_current = f
        lv = r.bone("strand")["tail"]
        kd = next(b for b in r.rig.bones_keyed if b["name"] == "strand")["tail"]
        swing.append(round(abs(lv[0] - kd[0]) + abs(lv[1] - kd[1]), 3))
    assert max(swing) > 0.02, ("it swings as the arm turns", swing)


own_action_turns_bone_with_dynamics()


# --- Wrong arguments are refused --------------------------------------------------------------------------------------
def bad_python_args():
    r = Rig()
    r.own("wave")
    r.doc.frame_current = 5
    assert r.play("nope", frame=3) == {'CANCELLED'}
    assert r.play("wave", loop="SOMETIMES") == {'CANCELLED'}
    assert r.play("wave", frame=-4) == {'FINISHED'} and r.action("wave")["frame"] == 0.0, r.action("wave")
    r.play("wave", frame=2)
    assert r.play("wave", frame="abc") == {'CANCELLED'} and r.action("wave")["frame"] == 2.0, r.action("wave")
    assert r.play("wave", frame="") == {'CANCELLED'} and r.play("wave", frame="nan") == {'CANCELLED'}
    assert ops.anim.action_move(name="wave", direction="DOWN") == {'CANCELLED'}


bad_python_args()

print("action panel edges: ok")
