"""2 Axis Controller edge cases: Dynamics springs the knob; in a side's own action a key at rest holds it there; a
linked dial and a moved fix pose go through the side's timing; back to Regular its timing keys go; an independent knob
goes to a spot where it is seen, its box turned; dragging a knob its constraints pull goes from where it is seen; a Knob
Parent hanging from its own controller can't carry it; a knob plays after what moves its constraints' targets.

Run with:  OpenAnimate --background --python tests/test_controller_robust.py
"""

import math

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers
QUARTER = math.pi / 2


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def go(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


class Rig:
    """A controller `eye` (box on `head`) with Right, Left and Down actions moving a pupil, 40 frames each."""

    def __init__(self, eye_at=(0.0, 0.0), extra=()):
        ops.wm.read_homefile()
        doc.auto_key = True
        doc.frame_current = 0
        ops.layer.new_bone()
        self.rig = layers.active
        ops.armature.bone_add(name="head", head_x="0", head_y="-1", tail_x="0", tail_y="-0.5")
        ops.armature.select_none()
        ops.armature.bone_add(name="eye", head_x=str(eye_at[0]), head_y=str(eye_at[1]), tail_x=str(eye_at[0] + 0.1),
                              tail_y=str(eye_at[1]), parent="head")
        for name, head, tail in (("look", (0, 0), (0, 0.1)), ("center", (0, 0.5), (0.1, 0.5))) + tuple(extra):
            ops.armature.select_none()
            ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]),
                                  tail_y=str(tail[1]))
        ops.layer.new()
        self.pupil = layers.active
        ops.layer.move_to_group(group=str(self.rig.id))
        self.pupil.add_curve([(0.0, 0.0), (0.2, 0.0)])
        assert ops.layer.parent_bone_set(bone="eye") == {'FINISHED'}
        for name, to in (("eye_r", (0.3, 0.0)), ("eye_l", (0.1, 0.0)), ("eye_d", (0.2, -0.1))):
            layers.active = self.pupil
            assert ops.anim.action_new(name=name) == {'FINISHED'}
            doc.frame_current = 40
            self.pupil.set_point(1, to)
            go(40)
            doc.action = ""
            go(0)
        layers.active = self.rig
        ops.armature.select_none()
        self.rig.select_bone("eye")
        assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
        assert ops.armature.controller_side(side="RIGHT", action="eye_r") == {'FINISHED'}
        assert ops.armature.controller_side(side="LEFT", action="eye_l") == {'FINISHED'}
        assert ops.armature.controller_side(side="DOWN", action="eye_d") == {'FINISHED'}
        self.size = self.bone("eye")["controller_frame_size"]

    def bone(self, name):
        return next(b for b in self.rig.bones if b["name"] == name)

    def playing(self, name):
        return next(a["frame"] for a in self.rig.actions if a["name"] == name)

    def time_right(self, frame, out):
        """In Right's own action, the knob `out` frames' worth along on `frame`."""
        assert ops.anim.action_set(name="eye_r") == {'FINISHED'}
        doc.frame_current = frame
        self.rig.bone_set("eye", knob=(out, 0.0))
        go(frame)
        assert ops.anim.action_set(name="") == {'FINISHED'}


# --- Dynamics springs the knob, as any bone's head -----------------------------------------------------------------
r = Rig()
assert ops.armature.constraint_add(type="DYNAMICS", bone="eye") == {'FINISHED'}
r.rig.bone_constraint_set("eye", 0, position_spring={"enabled": True}, angle_spring={"enabled": False})
doc.frame_current = 5
r.rig.bone_set("eye", knob=(20.0, 0.0))
seen = []
for f in range(1, 16):
    doc.frame_current = f
    seen.append((r.bone("eye")["knob"][0], r.bone("eye")["knob_shown"][0], r.playing("eye_r")))
assert any(abs(s - k) > 0.5 for k, s, p in seen), ("the knob lags and springs", seen)
assert abs(seen[-1][1] - 20) < 2 and all(close(s, p) for k, s, p in seen), ("and its actions play it", seen)

# --- In a side's own action, a key at rest holds the knob at rest there ---------------------------------------------
r = Rig()
assert ops.anim.action_set(name="eye_r") == {'FINISHED'}
doc.frame_current = 20
r.rig.bone_set("eye", knob=(0.0, 0.0))
go(20)
assert close(r.bone("eye")["knob_shown"], (0, 0)), ("a key at rest holds it there", r.bone("eye")["knob_shown"])
go(30)
assert close(r.bone("eye")["knob_shown"], (10, 0)), ("then evenly on", r.bone("eye")["knob_shown"])
assert ops.anim.action_set(name="") == {'FINISHED'}

# --- A linked dial goes through the timing -------------------------------------------------------------------------
ops.wm.read_homefile()
doc.auto_key = True
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
for name, y in (("dial", 1.0), ("joy", 3.0)):
    ops.armature.select_none()
    ops.armature.bone_add(name=name, head_x="5.0", head_y=str(y), tail_x="6.0", tail_y=str(y))
rig.bone_set("dial", strength=0.0)
ops.layer.new()
face = layers.active
face.add_curve([(0.0, 0.0), (1.0, 0.0)])
ops.layer.move_to_group(group=str(rig.id))
layers.active = rig
dial_bone = lambda: next(b for b in rig.bones if b["name"] == "dial")
joy_bone = lambda: next(b for b in rig.bones if b["name"] == "joy")
rig.select_bone("dial")
assert ops.anim.action_new(name="dial", action_bone="1") == {'FINISHED'}
doc.frame_current = 40
rig.bone_set("dial", angle=dial_bone()["setup_angle"] + QUARTER)
face.set_point(0, (1.0, 1.0))
go(40)
doc.action = ""
doc.frame_current = 0
ops.armature.select_none()
rig.select_bone("joy")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="dial") == {'FINISHED'}
assert ops.anim.action_set(name="dial") == {'FINISHED'}
doc.frame_current = 20
rig.bone_set("joy", knob=(10.0, 0.0))
go(20)
assert ops.anim.action_set(name="") == {'FINISHED'}
doc.frame_current = 7
go(7)
rig.select_bone("dial")
rig.bone_set("dial", angle=dial_bone()["setup_angle"] + QUARTER / 2)
go(7)
assert close(joy_bone()["knob"], (10, 0), 0.05), ("the dial's frame 20 is 10 out", joy_bone()["knob"])

# --- A fix pose moved with its spot holds the frame its place plays --------------------------------------------------
r = Rig()
r.time_right(20, 10.0)
doc.frame_current = 5
r.rig.bone_set("eye", knob=(10.0, -20.0))
go(5)
assert ops.anim.fix_action_new() == {'FINISHED'}
assert ops.anim.action_set(name="") == {'FINISHED'}
assert ops.armature.controller_spot_add(name="Here", x="10", y="-20") == {'FINISHED'}
go(5)
r.rig.bone_set("eye", knob=(5.0, -20.0))
go(5)
assert ops.armature.controller_spot_move(name="Here", fix="FOLLOW") == {'FINISHED'}
fix = r.rig.fix_actions[0]
at = dict(zip(fix["drivers"], fix["poses"][0]["at"]))
assert close(at["eye_r"], 10, 0.05) and close(at["eye_d"], 20, 0.05), ("5 out on Right is its frame 10", at)

# --- Back to Regular, its timing keys go ------------------------------------------------------------------------------
r = Rig()
r.time_right(20, 10.0)
go(0)
ops.armature.select_none()
r.rig.select_bone("eye")
assert ops.armature.bone_convert(type="REGULAR") == {'FINISHED'}
doc.action = "eye_r"
go(20)
assert r.rig.keyframes('bones["eye"].position') == [], ("no timing left to move it", r.rig.keyframes('bones["eye"].position'))
doc.action = ""

# --- An independent knob goes to a spot where it is seen, its box turned -------------------------------------------
r = Rig(eye_at=(0.0, 1.0))
assert ops.armature.bone_relations_set(bone="eye", knob_independent="1") == {'FINISHED'}
assert ops.armature.controller_spot_add(name="Smile", x="5", y="0") == {'FINISHED'}
doc.frame_current = 20
r.rig.bone_set("head", angle=r.bone("head")["setup_angle"] + 0.6)
go(20)
ops.armature.select_none()
r.rig.select_bone("eye")
assert ops.armature.controller_spot_go(name="Smile") == {'FINISHED'}
go(20)
assert close(r.bone("eye")["knob_shown"], (5, 0)) and close(r.playing("eye_r"), 5), \
    ("at the spot as seen", r.bone("eye")["knob_shown"], r.playing("eye_r"))

# --- Dragging a knob its constraints pull: from where it is seen -----------------------------------------------------
r = Rig()
assert ops.armature.constraint_add(type="LOOK_AT", bone="eye") == {'FINISHED'}
r.rig.bone_constraint_set("eye", 0, target="look")
doc.frame_current = 10
r.rig.bone_set("look", position=(1000.0, 0.0))
r.rig.bone_set("eye", knob=(10.0, 0.0))
go(10)
assert close(r.bone("eye")["knob_shown"], (40, 0), 0.1), r.bone("eye")["knob_shown"]
ops.armature.select_none()
r.rig.select_bone("eye")
assert r.rig.drag_bone_ends([(0.0, 0.0), (-5 * r.size, 0.0)])
go(10)
assert close(r.bone("eye")["knob_shown"], (35, 0), 0.1), ("dragged back, it moves at once", r.bone("eye")["knob_shown"])

# --- A Knob Parent hanging from its own controller can't carry it ---------------------------------------------------
r = Rig(eye_at=(1.0, 1.0))
assert ops.armature.bone_relations_set(bone="eye", knob_independent="1", knob_parent="center") == {'FINISHED'}
go(0)
ops.armature.select_none()
r.rig.select_bone("center")
r.rig.select_bone("eye", extend=True)
assert ops.armature.parent_set(type="OFFSET") == {'FINISHED'}
assert r.bone("center")["parent"] == "eye", r.bone("center")["parent"]
go(10)
center = r.bone("center")
assert close(center["head"], (0.0, 0.5)), ("it hangs from the knob, where it is", center["head"])

# --- A knob plays after what moves its constraints' targets ----------------------------------------------------------
r = Rig(extra=(("nod", (3, 0), (4, 0)),))
assert ops.armature.constraint_add(type="COPY_POSITION", bone="eye") == {'FINISHED'}
r.rig.bone_constraint_set("eye", 0, target="look")
r.rig.select_bone("nod")
assert ops.anim.action_new(name="nod", action_bone="1") == {'FINISHED'}
doc.frame_current = 40
r.rig.bone_set("nod", angle=r.bone("nod")["setup_angle"] + 1.0)
r.rig.bone_set("look", position=(20 * r.size, 0.0))
go(40)
doc.action = ""
go(0)
doc.frame_current = 10
r.rig.bone_set("nod", angle=r.bone("nod")["setup_angle"] + 1.0)
go(5)
doc.frame_current = 10  # once: played in one go, not from where the pass on frame 5 left the target
pupil = next(x for x in layers.all() if x.id == r.pupil.id)
# It follows its knob (20 frames out) and Right plays its frame 20 there: the tip 0.05 further out than at rest.
assert close(tuple(pupil.shown()[0][1]), (20 * r.size + 0.25, 0.0), 1e-3), ("the nod moves the target first", pupil.shown()[0])
assert close(r.playing("eye_r"), 20, 0.05), (r.playing("eye_r"), r.bone("eye")["knob_shown"])

print("Controller robustness tests passed")
