"""Switch layers: a group that shows one of its children, or none. Its keys hold the child's layer
id (so renaming and reordering never break them) and only hold; frame 0 is the default. Who wins:
a key made by hand after frame 0, until a Rig key; then an action bone action whose bone has turned past
one of its switch keys (the first listed); then the default. Copies key their copied children; a
child that is gone shows the default. Files keep it all; exports draw only the child shown.

Run with:  OpenAnimate --background --python tests/test_switch_layer.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_switch_"))
doc.frame_current = 0
doc.auto_key = True


def sync():
    doc.frame_current = doc.frame_current


def goto(frame):
    doc.frame_current = frame


def keyed(layer):
    keys = layer.keyframes("switch")
    assert all(k["interp"] == "STEP" for k in keys), "switch keys only hold"
    return [k["frame"] for k in keys]


def drawn():
    return [n for n in oanim.data.draw_order if n in ("A", "B", "C")]


oanim.ops.layer.new_bone()
rig = layers.active
oanim.ops.armature.bone_add(name="dial", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
assert oanim.ops.layer.new_switch() == {'FINISHED'}
sw = layers.active
assert sw.type == "SWITCH" and sw.name.startswith("Switch"), (sw.type, sw.name)
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
assert sw.switch_active is None and sw.switch_default is None, "an empty switch shows nothing"

children = {}
for name, colour in (("A", (1.0, 0.0, 0.0, 1.0)), ("B", (0.0, 1.0, 0.0, 1.0)), ("C", (0.0, 0.0, 1.0, 1.0))):
    layers.active = sw
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)], closed=True, fill=True)
    l.set_shape_style(0, fill=colour)
    assert oanim.ops.layer.move_to_group(group=str(sw.id)) == {'FINISHED'}
    children[name] = l
# The switch itself active: the viewport shows its pick (a child active would show for editing).
layers.active = sw
sync()

# The first layer put in becomes the default; only it is drawn.
assert sw.switch_default == "A" and sw.switch_active == "A", (sw.switch_default, sw.switch_active)
assert drawn() == ["A"], drawn()
# Its children keep their own visibility: switched out is not hidden.
assert children["B"].visible and children["C"].visible

# --- Picking: frame 0 sets the default; other frames key ------------------------------------------
assert oanim.ops.layer.switch_set(layer=sw.name, child="B") == {'FINISHED'}
assert sw.switch_default == "B" and keyed(sw) == [], "frame 0: the default, no key"
goto(10)
assert oanim.ops.layer.switch_set(layer=sw.name, child="C") == {'FINISHED'}
assert keyed(sw) == [0, 10], keyed(sw)
for frame, want in ((0, "B"), (5, "B"), (10, "C"), (40, "C")):
    goto(frame)
    assert sw.switch_active == want and drawn() == [want], (frame, sw.switch_active, drawn())
# None is a value of its own.
goto(20)
assert oanim.ops.layer.switch_set(layer=sw.name, child="") == {'FINISHED'}
assert sw.switch_active is None and drawn() == [], drawn()
goto(15)
assert sw.switch_active == "C"
# Picking a child of the switch picks for the switch holding it (the active layer is a child).
layers.active = children["A"]
goto(30)
assert oanim.ops.layer.switch_set(child="A") == {'FINISHED'}
assert sw.switch_active == "A" and keyed(sw) == [0, 10, 20, 30]
assert oanim.ops.layer.switch_set(child="nobody") == {'CANCELLED'}

# Renaming or reordering children never breaks the keys: they hold ids.
children["C"].name = "C2"
goto(12)
assert sw.switch_active == "C2"
children["C"].name = "C"
layers.active = children["C"]
oanim.ops.layer.move_up()
goto(12)
assert sw.switch_active == "C"

# --- Action bone actions pick, hand keys win, Rig hands back ---------------------------------------------
# An action bone action on the dial keys the switch to C at its frame 10.
goto(0)
assert oanim.ops.anim.action_new(name="dial", action_bone="1") == {'FINISHED'} and doc.action == "dial"
goto(10)
rig.bone_set("dial", angle=1.0)
assert oanim.ops.layer.switch_set(layer=sw.name, child="C") == {'FINISHED'}
goto(0)
assert oanim.ops.anim.action_set(name="") == {'FINISHED'}


def dial(angle, frame):
    goto(frame)
    rig.bone_set("dial", angle=angle)
    sync()


# On frame 5 no hand key holds (frame 0 is the default, not a hand key), so the rig decides: the dial
# past the action's key at 10 picks C; short of it, the action has no say.
dial(0.5, 5)
assert sw.switch_active == "B", ("before the action's key: the default", sw.switch_active)
dial(1.0, 5)
assert sw.switch_active == "C", ("the bone past the key: the rig's pick", sw.switch_active)
dial(3.0, 5)
assert sw.switch_active == "C", "past the end it holds"
# A hand key wins over the rig from its frame on (frame 10: the hand's C; 20: None; 30: A).
dial(1.0, 30)
assert sw.switch_active == "A", ("the hand key wins", sw.switch_active)
dial(1.0, 20)
assert sw.switch_active is None, ("a hand None wins too", sw.switch_active)
# A Rig key hands it back.
goto(25)
assert oanim.ops.layer.switch_set(layer=sw.name, child="RIG") == {'FINISHED'}
dial(1.0, 25)
assert sw.switch_active == "C", ("Rig: the action bone action again", sw.switch_active)
dial(0.0, 25)
assert sw.switch_active == "B", ("Rig, the dial at rest: the default", sw.switch_active)
# The action bone action's pick is never data: the frame-0 default stays B, and no key appeared.
assert sw.switch_default == "B"
assert keyed(sw) == [0, 10, 20, 25, 30], keyed(sw)
# Frame 0 is the rig at rest: the default.
goto(0)
assert sw.switch_active == "B"

# --- Undo ------------------------------------------------------------------------------------------
goto(40)
before = keyed(sw)
oanim.ops.layer.switch_set(layer=sw.name, child="B")
assert sw.switch_active == "B"
oanim.ops.ed.undo()
goto(40)
assert keyed(sw) == before and sw.switch_active == "A", (keyed(sw), sw.switch_active)

# --- Export draws only the child shown -------------------------------------------------------------
goto(15)
assert sw.switch_active == "C"
svg = folder / "frame.svg"
assert oanim.ops.export.svg(filepath=str(svg)) == {'FINISHED'}
text = svg.read_text()
assert "#0000ff" in text and "#ff0000" not in text and "#00ff00" not in text, "only C"

# --- Save and open ---------------------------------------------------------------------------------
path = folder / "switch.oanim"
assert oanim.ops.wm.save_as_mainfile(filepath=str(path)) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=str(path)) == {'FINISHED'}
doc = oanim.data.document
doc.auto_key = True
sw = next(l for l in layers.all() if l.type == "SWITCH")
assert keyed(sw) == [0, 10, 20, 25, 30], keyed(sw)
for frame, want in ((0, "B"), (12, "C"), (22, None), (30, "A")):
    goto(frame)
    assert sw.switch_active == want, (frame, sw.switch_active)
rig = next(l for l in layers.all() if l.type == "BONE")
dial(1.0, 25)
assert sw.switch_active == "C", "the action bone action came back with the file"
dial(0.0, 25)

# --- Copies key their copied children; a child that is gone shows the default ----------------------
layers.active = sw
assert oanim.ops.layer.duplicate() == {'FINISHED'}
copy = layers.active
assert copy.id != sw.id and copy.type == "SWITCH"
goto(12)
assert copy.switch_active == "C" and copy.switch_default == "B", (copy.switch_active, copy.switch_default)
# The copy shows its own child, not the original's: both Cs are drawn.
original_ids = {l.id for l in sw.children}
names = oanim.data.draw_order
assert len([n for n in names if n.startswith("C")]) == 2, names

layers.active = next(l for l in layers.all() if l.name == "C" and l.id in original_ids)
assert oanim.ops.layer.delete() == {'FINISHED'}
goto(12)
assert sw.switch_active == "B", ("C gone: the default", sw.switch_active)
assert oanim.ops.layer.switch_set(layer=sw.name, child="C") == {'CANCELLED'}


# --- With one of its drawings active, the timeline still shows the switch's keys ----------------------
layers.active = children["A"]
at = [r["label"] for r in oanim.wm.timeline_row_details(True)].index(sw.name)
assert oanim.wm.timeline_row_fold(at, True)
shown = {r["label"]: r for r in oanim.wm.timeline_row_details(True)}
assert shown[sw.name]["open"], list(shown)
assert shown[sw.name]["frames"] == [k["frame"] for k in sw.keyframes("switch")], (shown[sw.name], "its own row")

# === Stage 2: picking =================================================================================
assert oanim.ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
doc = oanim.data.document
doc.auto_key = True
goto(0)
made = {}
for name, colour in (("A", (1.0, 0.0, 0.0, 1.0)), ("B", (0.0, 1.0, 0.0, 1.0)), ("C", (0.0, 0.0, 1.0, 1.0))):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)], closed=True, fill=True)
    l.set_shape_style(0, fill=colour)
    made[name] = l
for l in made.values():
    l.selected = True

# Wrap in Switch: the picked layers into a new switch, the top one (C) the default.
assert oanim.ops.layer.wrap_in_switch() == {'FINISHED'}
sw = layers.active
sync()
assert sw.type == "SWITCH" and [c.name for c in sw.children] == ["C", "B", "A"], [c.name for c in sw.children]
assert sw.switch_default == "C" and drawn() == ["C"], (sw.switch_default, drawn())

# ] and [: the next and previous child in the Layers editor's order (wrapping), keyed at this frame.
goto(5)
assert oanim.ops.layer.switch_next() == {'FINISHED'} and sw.switch_active == "B"
assert oanim.ops.layer.switch_next() == {'FINISHED'} and sw.switch_active == "A"
assert oanim.ops.layer.switch_next() == {'FINISHED'} and sw.switch_active == "C", "wraps round"
assert oanim.ops.layer.switch_previous() == {'FINISHED'} and sw.switch_active == "A"
assert keyed(sw) == [0, 5], keyed(sw)

# Alt: the whole hold the frame is in (from frame 5's key on), no new key.
goto(9)
assert oanim.ops.layer.switch_set(layer=sw.name, child="B", hold="1") == {'FINISHED'}
assert keyed(sw) == [0, 5] and sw.switch_active == "B"
goto(5)
assert sw.switch_active == "B", "the key at 5 now shows B"
goto(2)
assert oanim.ops.layer.switch_set(layer=sw.name, child="A", hold="1") == {'FINISHED'}
assert sw.switch_default == "A" and keyed(sw) == [0, 5], "the hold from frame 0 is the default's"

# Replace Keys With: every key showing B shows C instead.
goto(30)
oanim.ops.layer.switch_set(layer=sw.name, child="B")
assert oanim.ops.layer.switch_replace_keys(layer=sw.name, **{"from": "B"}, child="C") == {'FINISHED'}
for frame, want in ((5, "C"), (30, "C"), (0, "A")):
    goto(frame)
    assert sw.switch_active == want, (frame, sw.switch_active)
assert oanim.ops.layer.switch_replace_keys(layer=sw.name, **{"from": "B"}, child="A") == {'CANCELLED'}, "none left"

# Editing a hidden child shows it in the viewport, unkeyed; renders and exports keep the pick.
goto(30)
assert sw.switch_active == "C"
layers.active = made["B"]
sync()
assert drawn() == ["B"], ("the child being edited shows in the viewport", drawn())
assert sw.switch_active == "C" and keyed(sw) == [0, 5, 30], "nothing keyed, the pick unchanged"
svg = folder / "editing.svg"
assert oanim.ops.export.svg(filepath=str(svg)) == {'FINISHED'}
text = svg.read_text()
assert "#0000ff" in text and "#00ff00" not in text, "the export shows the pick, C"
# Picking while editing a child moves the active layer to the pick, so the viewport shows it.
assert oanim.ops.layer.switch_next() == {'FINISHED'}
assert sw.switch_active == "A" and layers.active.name == "A" and drawn() == ["A"], (sw.switch_active, layers.active.name, drawn())
layers.active = sw
sync()
assert drawn() == ["A"]

# A child that is gone: its keys show the default until Replace Keys With points them elsewhere.
goto(40)
oanim.ops.layer.switch_set(layer=sw.name, child="B")
gone_id = made["B"].id
layers.active = made["B"]
assert oanim.ops.layer.delete() == {'FINISHED'}
goto(40)
assert sw.switch_active == "A", ("B gone: the default", sw.switch_active)
assert oanim.ops.layer.switch_replace_keys(layer=sw.name, **{"from": str(gone_id)}, child="C") == {'FINISHED'}
goto(40)
assert sw.switch_active == "C"


# --- A hidden child shows for editing only on the frame it was selected: scrubbing shows the picks --------
layers.active = sw
goto(30)
pick = sw.switch_active
hidden = next(c for c in sw.children if c.name != pick)
layers.active = hidden
sync()
assert drawn() == [hidden.name], drawn()
goto(31)
assert drawn() == [sw.switch_active], ("scrubbing shows the keyed pick", drawn(), sw.switch_active)
goto(30)
assert drawn() == [pick], ("and back on its frame too", drawn())
layers.active = sw

# --- Picks follow Auto Keying ------------------------------------------------------------------------
doc.auto_key = False
goto(50)
keys = keyed(sw)
assert oanim.ops.layer.switch_set(layer=sw.name, child="A") == {'FINISHED'}
assert sw.switch_active == "A" and keyed(sw) == keys, ("shown, not keyed", sw.switch_active, keyed(sw))
goto(51)
goto(50)
assert sw.switch_active == "C", ("gone when the frame changes", sw.switch_active)
# Keyed by hand (the key button, or I).
assert oanim.ops.layer.switch_set(layer=sw.name, child="A") == {'FINISHED'}
assert oanim.ops.anim.keyframe_toggle(layer=str(sw.id), paths="switch") == {'FINISHED'}
assert 50 in keyed(sw), keyed(sw)
goto(51)
goto(50)
assert sw.switch_active == "A"
# I with one of its drawings active keys the switch's pick too.
goto(60)
assert oanim.ops.layer.switch_set(layer=sw.name, child="C") == {'FINISHED'}
layers.active = next(c for c in sw.children if c.name == "C")
assert oanim.ops.anim.keyframe_insert() == {'FINISHED'}
assert 60 in keyed(sw), keyed(sw)
doc.auto_key = True

# Convert to Switch (the Layers editor's right-click menu): a group becomes a switch where it is, its children
# the drawings, the top one the default; not offered for anything but a group.
goto(0)
# At the top, not inside a switch's drawing (one not shown shows nothing of what is in it).
layers.active = [l for l in layers.all() if l.parent is None and l.type != "SWITCH"][0]
oanim.ops.layer.new_group()
holder = layers.active
assert holder.parent is None
holder.name = "Holder"
for name in ("Low", "High"):
    layers.active = holder
    oanim.ops.layer.new()
    layers.active.name = name
    assert oanim.ops.layer.move_to_group(group=str(holder.id)) == {'FINISHED'}
layers.active = holder
order = [c.name for c in holder.children]
assert oanim.ops.layer.convert_to_switch() == {'FINISHED'}
sync()
assert holder.type == "SWITCH" and holder.name == "Holder" and [c.name for c in holder.children] == order
assert holder.switch_default == order[0] and holder.switch_active == order[0], (holder.switch_default, holder.switch_active, order)
assert oanim.ops.layer.switch_set(layer=str(holder.id), child=order[1]) == {'FINISHED'}
assert holder.switch_default == order[1]
oanim.ops.ed.undo()
oanim.ops.ed.undo()
assert holder.type == "GROUP", "one undo step"
layers.active = holder.children[0]
assert not oanim.ops.layer.convert_to_switch.poll(), "only a group converts"
layers.active = holder
assert oanim.ops.layer.convert_to_switch.poll()

print("switch layer OK")
