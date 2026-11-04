"""Motion paths: the Edit brush's selection and what it does to it. A click selects a key or a dot between keys (Shift
adds or takes out, a double click goes to its frame, a click on nothing lets go); Alt click adds a key; G R S on the
selected keys (one key: its handles; several: the keys about their centre); V sets their handle type; X deletes them;
Even Spacing and Alt S work on the dots and keys selected.

Run with:  OpenAnimate --background --python tests/test_motion_path_edit.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def key(frame):
    return next(k for k in box.keyframes("position") if k["frame"] == frame)


def frames():
    return [k["frame"] for k in box.keyframes("position") if k["frame"] > 0]


def selected():
    return [k["frame"] for k in box.keyframes("position") if k["selected"] and k["frame"] > 0]


def dots():
    return [d["frame"] for d in oanim.wm.motion_path_dots()]


def where(frame):
    doc.frame_current = frame
    return (box.location_x, box.location_y)


# --- A layer's origin keyed in a straight line: (2, 0) on 10, (4, 0) on 20, (6, 0) on 30 -----------------------------
doc.auto_key = True
ops.layer.new()
box = layers.active
box.add_curve([(0.0, 0.0), (0.5, 0.0)])
doc.motion_paths = True
args = dict(layer=str(box.id))
for frame, x in ((10, 2.0), (20, 4.0), (30, 6.0)):
    assert ops.anim.motion_path_move(frame=str(frame), x=str(x), y="0", **args) == {'FINISHED'}
assert ops.wm.tool_motion_path() == {'FINISHED'}
assert ops.anim.motion_path_brush_set(brush="EDIT") == {'FINISHED'}


def select(frame, **kw):
    assert ops.anim.motion_path_select(frame=str(frame), **args, **kw) == {'FINISHED'}


# --- Selecting: a key, or a dot between keys --------------------------------------------------------------------------
select(20)
assert selected() == [20] and dots() == [], (selected(), dots())
select(15)
assert selected() == [] and dots() == [15], "a dot between keys: that dot, the key let go"
select(17, extend="1")
assert dots() == [15, 17], "Shift adds"
select(15, extend="1")
assert dots() == [17], "Shift on a selected one takes it out"
select(20, extend="1")
assert selected() == [20] and dots() == [17], "keys and dots together"
select(20, extend="1")
assert selected() == [] and dots() == [17]
select(20, extend="1")
doc.frame_current = 1
select(20, go="1")
assert doc.frame_current == 20 and selected() == [20] and dots() == [17], "a double click on it: only the frame"
select(25, go="1")
assert doc.frame_current == 25 and selected() == [] and dots() == [25], "on something new: that, and its frame"
assert ops.anim.motion_path_select(none="1") == {'FINISHED'}
assert selected() == [] and dots() == [], "a click on nothing lets go"

# --- With nothing selected, the shortcuts don't start ----------------------------------------------------------------
for idname in ("motion_path_keys_grab", "motion_path_keys_rotate", "motion_path_keys_scale",
               "motion_path_handles_menu", "motion_path_keys_delete", "motion_path_scale_spacing_modal"):
    assert getattr(ops.anim, idname)() == {'CANCELLED'}, idname
assert ops.anim.motion_path_even_spacing() == {'CANCELLED'}, "no dots selected"

# --- Even Spacing and Alt S on the selected dots ----------------------------------------------------------------------
assert ops.anim.motion_path_slide(frame="15", x="3.6", y="0", **args) == {'FINISHED'}
assert close(where(15), (3.6, 0.0))
select(15)
assert ops.anim.motion_path_even_spacing() == {'FINISHED'}
assert close([where(f)[0] for f in range(10, 21)], [2.0 + 0.2 * i for i in range(11)]), "the selected dot's stretch"
assert ops.anim.motion_path_scale_spacing_modal() == {'FINISHED'}, "Alt S starts with a dot selected"
# Half as far from 15 within 3 frames: 14 from 2.8 to 2.9, 13 from 2.6 to 2.8; 12, 3 away, stays.
assert ops.anim.motion_path_scale_spacing(factor="0.5", proportional="0", size="3", **args) == {'FINISHED'}
assert close(where(14)[0], 2.9) and close(where(13)[0], 2.8) and close(where(12)[0], 2.4), (
    where(14), where(13), where(12))
assert close(where(20), (4.0, 0.0)), "the keys stay"
ops.ed.undo()
assert close(where(14)[0], 2.8)

# --- Alt click: a key where the dot is, the path as it was --------------------------------------------------------
was = where(25)
select(20)
assert ops.anim.motion_path_key_add(frame="25", **args) == {'FINISHED'}
assert 25 in frames() and close(key(25)["value"], was), key(25)
assert selected() == [25] and dots() == [], "the new key selected"
assert ops.anim.motion_path_key_add(frame="25", **args) == {'CANCELLED'}, "a key there already"
ops.ed.undo()
assert 25 not in frames()

# --- One key: G moves it, R and S its handles about it ----------------------------------------------------------
select(20)
for idname in ("motion_path_keys_grab", "motion_path_keys_rotate", "motion_path_keys_scale",
               "motion_path_handles_menu"):
    assert getattr(ops.anim, idname)() == {'FINISHED'}, idname
assert ops.anim.motion_path_keys_transform(kind="G", x="0", y="1") == {'FINISHED'}
assert close(key(20)["value"], (4.0, 1.0)) and close(key(10)["value"], (2.0, 0.0)), key(20)
ops.ed.undo()
assert close(key(20)["value"], (4.0, 0.0)), "one undo step"
# Undo brings back the selection the step before had, as Blender's does.
select(20)
assert ops.anim.motion_path_keys_transform(kind="R", angle=str(math.pi / 2)) == {'FINISHED'}
k = key(20)
assert close(k["value"], (4.0, 0.0)), "the key stays"
assert close(k["path_out"], (0.0, 2.0 / 3.0)) and close(k["path_in"], (0.0, -2.0 / 3.0)), k
assert k["path_handles"] == "ALIGNED", "still in line"
ops.ed.undo()
select(20)
assert ops.anim.motion_path_keys_transform(kind="S", factor="2") == {'FINISHED'}
k = key(20)
assert close(k["path_out"], (4.0 / 3.0, 0.0)) and close(k["path_in"], (-4.0 / 3.0, 0.0)), k
ops.ed.undo()

# --- Several keys: about their centre, (4, 0) ---------------------------------------------------------------------
select(10)
select(30, extend="1")
assert ops.anim.motion_path_keys_transform(kind="S", factor="2") == {'FINISHED'}
assert close(key(10)["value"], (0.0, 0.0)) and close(key(30)["value"], (8.0, 0.0)), (key(10), key(30))
assert close(key(20)["value"], (4.0, 0.0)), "not selected: stays"
ops.ed.undo()
select(10)
select(30, extend="1")
assert ops.anim.motion_path_keys_transform(kind="R", angle=str(math.pi)) == {'FINISHED'}
assert close(key(10)["value"], (6.0, 0.0)) and close(key(30)["value"], (2.0, 0.0)), (key(10), key(30))
ops.ed.undo()
assert close(key(10)["value"], (2.0, 0.0))

# --- V: the handle types ----------------------------------------------------------------------------------------
select(20)
assert ops.anim.motion_path_handle(frame="20", side="OUT", x="4", y="1", **args) == {'FINISHED'}
assert ops.anim.motion_path_handles_type(mode="BROKEN") == {'FINISHED'}
assert key(20)["path_handles"] == "BROKEN"
assert ops.anim.motion_path_handle(frame="20", side="IN", x="3.5", y="0.5", **args) == {'FINISHED'}
assert close(key(20)["path_in"], (-0.5, 0.5)) and close(key(20)["path_out"], (0.0, 1.0)), key(20)
# Aligned: in line along the way from the in handle's end to the out's, each keeping its length.
assert ops.anim.motion_path_handles_type(mode="ALIGNED") == {'FINISHED'}
k = key(20)
h = math.sqrt(0.5)
assert k["path_handles"] == "ALIGNED" and close(k["path_out"], (h, h)) and close(k["path_in"], (-0.5, -0.5)), k
assert ops.anim.motion_path_handles_type(mode="AUTO") == {'FINISHED'}
k = key(20)
assert k["path_handles"] == "AUTO" and close(k["path_out"], (2.0 / 3.0, 0.0)), k
ops.ed.undo()
assert key(20)["path_handles"] == "ALIGNED", "one undo step each"
select(20)
assert ops.anim.motion_path_handles_type(mode="SIDEWAYS") == {'CANCELLED'}

# --- X: the selected keys deleted -----------------------------------------------------------------------------------
select(20)
assert ops.anim.motion_path_keys_delete() == {'FINISHED'}
assert frames() == [10, 30], frames()
ops.ed.undo()
assert frames() == [10, 20, 30]
select(30)
assert oanim.wm.event_simulate("X", area="Viewport")
assert frames() == [10, 20], "through the keymap"
