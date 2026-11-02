"""Switch layers stage 4: what the asset shelf does to a switch's children. Duplicate copies a child
to change into another drawing (after it, picked and keyed at this frame), without its timeline keys
(from its setup, or with This Frame from how it looks now), keeping its action bone actions' keys; Duplicate
with Keys keeps them all. Add to Switch adds an empty drawing. A hover preview shows a child in the
viewport without keying it, while exports keep the pick.

Run with:  OpenAnimate --background --python tests/test_switch_shelf.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_switch_shelf_"))
doc.frame_current = 0
doc.auto_key = True


def sync():
    doc.frame_current = doc.frame_current


def goto(frame):
    doc.frame_current = frame


def keyed(layer, path):
    return [k["frame"] for k in layer.keyframes(path)]


def drawn():
    return [n for n in oanim.data.draw_order if n in names()]


def names():
    return [c.name for c in sw.children]


def near(a, b):
    return all(abs(x - y) < 1e-4 for x, y in zip(a, b))


for name, colour in (("O", (1.0, 0.0, 0.0, 1.0)), ("A", (0.0, 1.0, 0.0, 1.0))):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)], closed=True, fill=True)
    l.set_shape_style(0, fill=colour)
    l.selected = True
assert oanim.ops.layer.wrap_in_switch() == {'FINISHED'}
sw = layers.active
sw.name = "Mouth"
# O the default (Wrap in Switch made the top layer, A, the default).
assert oanim.ops.layer.switch_set(layer=sw.name, child="O") == {'FINISHED'}
o = next(c for c in sw.children if c.name == "O")
setup = o.points[0][:2]
# O's corner moves on frame 10 (a point key).
goto(10)
o.set_point(0, (-0.8, -0.8))
goto(11)  # records the edit as a key on frame 10
goto(10)
# An undo step after it, as the app makes one for the drag (undo below goes back to here).
assert oanim.ops.layer.switch_set(layer=sw.name, child="O", hold="1") == {'FINISHED'}
assert keyed(o, "points[0].pos") == [0, 10], keyed(o, "points[0].pos")
moved = o.points[0][:2]

# --- Duplicate: after the original, picked and keyed here, without timeline keys --------------------
goto(10)
layers.active = sw
assert oanim.ops.layer.switch_duplicate(child="O") == {'FINISHED'}
copy = layers.active
assert copy.name != "O" and copy.name.startswith("O"), copy.name
assert names().index(copy.name) == names().index("O") + 1, names()
assert sw.switch_active == copy.name and 10 in keyed(sw, "switch"), (sw.switch_active, keyed(sw, "switch"))
assert keyed(copy, "points[0].pos") == [], "no timeline keys"
assert near(copy.points[0][:2], setup), ("from the original's setup", copy.points[0], setup)
# The original keeps its keys; undo takes the copy away in one step.
assert keyed(o, "points[0].pos") == [0, 10]
oanim.ops.ed.undo()
assert len(sw.children) == 2 and sw.switch_active == "O", (names(), sw.switch_active)

# --- Start From This Frame: how it looks now becomes its setup -------------------------------------
goto(10)
assert oanim.ops.layer.switch_duplicate(child="O", start_from="THIS_FRAME") == {'FINISHED'}
now_copy = layers.active
assert keyed(now_copy, "points[0].pos") == [] and near(now_copy.points[0][:2], moved), (now_copy.points[0], moved, keyed(now_copy, "points[0].pos"))
goto(0)
assert near(now_copy.points[0][:2], moved), ("its setup is the moved pose", now_copy.points[0])
oanim.ops.ed.undo()

# --- Duplicate with Keys -------------------------------------------------------------------------
goto(10)
assert oanim.ops.layer.switch_duplicate(child="O", keys="1") == {'FINISHED'}
keyed_copy = layers.active
assert keyed(keyed_copy, "points[0].pos") == [0, 10]
oanim.ops.ed.undo()

# --- On frame 0 a duplicate is only added ----------------------------------------------------------
goto(0)
before = keyed(sw, "switch")
assert oanim.ops.layer.switch_duplicate(child="A") == {'FINISHED'}
assert keyed(sw, "switch") == before and sw.switch_default == "O", (keyed(sw, "switch"), sw.switch_default)
oanim.ops.ed.undo()

# --- Add to Switch: an empty drawing, last, picked and keyed -----------------------------------------
goto(20)
layers.active = sw
assert oanim.ops.layer.switch_add_child() == {'FINISHED'}
added = layers.active
assert names()[-1] == added.name and sw.switch_active == added.name and 20 in keyed(sw, "switch")
assert added.points == []
oanim.ops.ed.undo()

# --- Hover preview: the viewport shows it, nothing keyed, exports keep the pick ----------------------
goto(30)
layers.active = sw
sync()
assert sw.switch_active == "O" and drawn() == ["O"], (sw.switch_active, drawn())
keys_before = keyed(sw, "switch")
a = next(c for c in sw.children if c.name == "A")
oanim.wm.switch_preview(sw, a)
assert drawn() == ["A"], drawn()
assert sw.switch_active == "O" and keyed(sw, "switch") == keys_before
svg = folder / "preview.svg"
assert oanim.ops.export.svg(filepath=str(svg)) == {'FINISHED'}
assert "#ff0000" in svg.read_text() and "#00ff00" not in svg.read_text(), "the export shows the pick"
oanim.wm.switch_preview()
assert drawn() == ["O"]


# --- Set as Default: frame 0's pick, from any frame, no key here ------------------------------------
goto(30)
keys_before = keyed(sw, "switch")
assert oanim.ops.layer.switch_set(layer=sw.name, child="A", default="1") == {'FINISHED'}
assert sw.switch_default == "A" and keyed(sw, "switch") == keys_before, (sw.switch_default, keyed(sw, "switch"))

print("switch shelf OK")
