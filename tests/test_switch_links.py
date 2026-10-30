"""Switches linked by name: a Heads switch with a Mouth switch in each view. Same-named switches at the
same place inside a switch's drawings link by themselves; one set of keys (by name) picks the mouth in
every view; a view without the keyed name keeps showing the last name it had, and says so; aliases
let a drawing answer to other names; renaming a drawing renames its partners; a lead deleted hands the
keys on; Unlink gives each its own keys showing what it showed; saved with the file.

Run with:  OpenAnimate --background --python tests/test_switch_links.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_switch_links_"))
doc.frame_current = 0
doc.auto_key = True
base = layers.active  # a top-level layer: each view is made beside it


def goto(frame):
    doc.frame_current = frame


def find(name, parent=None):
    for l in layers.all():
        if l.name == name and (parent is None or any(c.id == l.id for c in parent.children)):
            return l
    raise KeyError(name)


def view(name, mouths):
    """A view group holding a Mouth switch of `mouths`."""
    layers.active = base
    oanim.ops.layer.new_group()
    group = layers.active
    group.name = name
    oanim.ops.layer.new_switch()
    mouth = layers.active
    mouth.name = "Mouth"
    assert oanim.ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
    for m in mouths:
        layers.active = mouth
        oanim.ops.layer.new()
        l = layers.active
        l.name = m
        l.add_curve([(-0.2, -0.1), (0.2, -0.1), (0.2, 0.1), (-0.2, 0.1)], closed=True, fill=True)
        assert oanim.ops.layer.move_to_group(group=str(mouth.id)) == {'FINISHED'}
    return group, mouth


front, front_mouth = view("Front", ["Happy", "Sad"])
side, side_mouth = view("Side", ["Happy", "Sad", "Special"])
for l in layers.all():
    l.selected = l.id in (front.id, side.id)
layers.active = front
assert oanim.ops.layer.wrap_in_switch() == {'FINISHED'}
heads = layers.active
heads.name = "Heads"
goto(1)  # recording links same-named switches
goto(0)

# --- Linked by themselves: same name, same place in each view ----------------------------------------
assert sorted(front_mouth.switch_linked) == ["Mouth", "Mouth"], front_mouth.switch_linked
assert len(side_mouth.switch_linked) == 2
assert oanim.ops.layer.switch_set(layer=str(front_mouth.id), child="Happy") == {'FINISHED'}
assert front_mouth.switch_default == "Happy" and side_mouth.switch_default == "Happy"

# The head turns to the side on frame 20.
goto(20)
assert oanim.ops.layer.switch_set(layer=str(heads.id), child="Side") == {'FINISHED'}

# --- A pick in one view picks the same name in the other -----------------------------------------
goto(10)
assert oanim.ops.layer.switch_set(layer=str(front_mouth.id), child="Sad") == {'FINISHED'}
assert front_mouth.switch_active == "Sad" and side_mouth.switch_active == "Sad"
goto(25)
assert heads.switch_active == "Side" and side_mouth.switch_active == "Sad", (heads.switch_active, side_mouth.switch_active)

# --- A name a view lacks: that view keeps the last name it had, and says so ---------------------------
goto(30)
assert oanim.ops.layer.switch_set(layer=str(side_mouth.id), child="Special") == {'FINISHED'}
assert side_mouth.switch_active == "Special"
assert front_mouth.switch_active == "Sad" and front_mouth.switch_missing == "Special", (front_mouth.switch_active, front_mouth.switch_missing)
assert side_mouth.switch_missing == ""
assert "Special" in front_mouth.switch_link_names

# --- An alias: Front's Sad answers to Special too ----------------------------------------------------
sad = find("Sad", front_mouth)
assert oanim.ops.layer.switch_aliases_set(layer=str(sad.id), aliases="Special") == {'FINISHED'}
assert sad.switch_aliases == ["Special"]
goto(31)
goto(30)
assert front_mouth.switch_active == "Sad" and front_mouth.switch_missing == "", front_mouth.switch_missing
assert oanim.ops.layer.switch_aliases_set(layer=str(sad.id), aliases="") == {'FINISHED'}

# --- Renaming a drawing renames its partners; the keys follow ----------------------------------------
goto(10)
happy = find("Happy", front_mouth)
happy.name = "Joy"
goto(11)  # recording passes the rename on
goto(10)
assert [c.name for c in side_mouth.children].count("Joy") == 1, [c.name for c in side_mouth.children]
goto(0)
assert front_mouth.switch_active == "Joy" and side_mouth.switch_active == "Joy"
# Rename Here Only: the partner keeps its name; this one no longer answers to the old one.
assert oanim.ops.layer.rename_here(layer=str(happy.id), name="Grin") == {'FINISHED'}
assert "Joy" in [c.name for c in side_mouth.children]
goto(1)
goto(0)
assert front_mouth.switch_missing == "Joy", front_mouth.switch_missing
happy.name = "Joy"
goto(1)
goto(0)

# --- Saved with the file -------------------------------------------------------------------------------
path = folder / "heads.oanim"
assert oanim.ops.wm.save_as_mainfile(filepath=str(path)) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=str(path)) == {'FINISHED'}
doc = oanim.data.document
doc.auto_key = True
heads = find("Heads")
front, side = find("Front"), find("Side")
front_mouth, side_mouth = find("Mouth", front), find("Mouth", side)
assert len(front_mouth.switch_linked) == 2
for frame, want in ((0, "Joy"), (10, "Sad"), (30, "Special")):
    goto(frame)
    assert side_mouth.switch_active == want, (frame, side_mouth.switch_active)

# --- Duplicating a view: its Mouth joins the link --------------------------------------------------
goto(0)
layers.active = front
assert oanim.ops.layer.switch_duplicate(child=str(front.id)) == {'FINISHED'}
goto(1)
goto(0)
assert len(front_mouth.switch_linked) == 3, front_mouth.switch_linked
oanim.ops.ed.undo()
goto(1)
goto(0)

# --- The lead deleted: another member keeps the keys ----------------------------------------------
lead_name = front_mouth.switch_linked[0]
goto(10)
before = side_mouth.switch_active
# Delete a copy of the lead's view would leave the group; delete the lead's own view on a scratch copy.
assert oanim.ops.wm.save_as_mainfile(filepath=str(folder / "before_delete.oanim")) == {'FINISHED'}
lead_view = front if any(c.id == front_mouth.id for c in front.children) else side
lead = front_mouth if front_mouth.switch_linked and front_mouth.name == lead_name else side_mouth
lead_view = front if lead.id == front_mouth.id else side
other = side_mouth if lead.id == front_mouth.id else front_mouth
layers.active = lead_view
assert oanim.ops.layer.delete() == {'FINISHED'}
goto(11)
goto(10)
assert other.switch_linked == [], "one left: no link"
assert other.switch_active == before, (other.switch_active, before)
assert [k["frame"] for k in other.keyframes("switch")], "it got the keys"
assert oanim.ops.wm.open_mainfile(filepath=str(folder / "before_delete.oanim")) == {'FINISHED'}
doc = oanim.data.document
doc.auto_key = True
front, side = find("Front"), find("Side")
front_mouth, side_mouth = find("Mouth", front), find("Mouth", side)

# --- Unlink: each keeps what it showed, on its own keys; not linked again by name ------------------------
shown = {}
for frame in (0, 10, 25, 30):
    goto(frame)
    shown[frame] = (front_mouth.switch_active, side_mouth.switch_active)
assert oanim.ops.layer.switch_unlink(layer=str(front_mouth.id)) == {'FINISHED'}
goto(1)
goto(0)
assert front_mouth.switch_linked == [] and side_mouth.switch_linked == []
for frame, (f, s) in shown.items():
    goto(frame)
    assert (front_mouth.switch_active, side_mouth.switch_active) == (f, s), (frame, front_mouth.switch_active, side_mouth.switch_active, f, s)
goto(40)
assert oanim.ops.layer.switch_set(layer=str(front_mouth.id), child="Joy") == {'FINISHED'}
assert side_mouth.switch_active != "Joy" or shown[30][1] == "Joy", "no longer linked"

print("switch links OK")
