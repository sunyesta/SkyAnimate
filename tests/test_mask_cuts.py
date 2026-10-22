"""Cuts: what of the masked layer a mask row cuts. LINES masks (or ends) only its lines, leaving its
fills as if the row weren't there, so two layers can cut each other's lines where they join without the
overlap losing both fills; FILLS only its fills. Picking goes by the fills. mask.join_lines sets that
pair of rows up in one step; mask.duplicate copies a row.

Run with:  OpenAnimate --background --python tests/test_mask_cuts.py
"""

import pathlib
import tempfile

import oanim

layers = oanim.data.layers
doc = oanim.data.document
doc.frame_current = 0


def find(name):
    return next(l for l in layers.all() if l.name == name)


def square(name, x0, y0, x1, y1):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], closed=True, fill=True)
    return l


def pick(x, y):
    return oanim.ops.layer.select_at(x=str(x), y=str(y))


# Head and ear overlap in (0.5..1, 0..1); each cuts the other's lines there.
head = square("head", -1.0, -1.0, 1.0, 1.0)
ear = square("ear", 0.5, 0.0, 2.0, 1.0)
layers.active = ear
assert oanim.ops.mask.add(sources="head", mode="SUBTRACT", shape="FILLS_ONLY", end_lines="1") == {'FINISHED'}
layers.active = head
assert oanim.ops.mask.add(sources="ear", mode="SUBTRACT", shape="FILLS_ONLY", end_lines="1") == {'FINISHED'}
assert head.masks[0]["cuts"] == "ALL" and head.masks[0]["lines_only"] is False

# Cutting all of both, both fills go in the overlap: picking there finds neither.
layers.active = find("head")
assert pick(1.5, 0.5) == {'FINISHED'} and layers.active.name == "ear"
assert pick(0.75, 0.5) == {'CANCELLED'}, "the hole: neither layer shows there"

# Cuts LINES on head's row: head's fill shows in the overlap again (its lines still end there).
assert oanim.ops.mask.set(layer=str(head.id), index="0", cuts="LINES") == {'FINISHED'}
assert head.masks[0]["cuts"] == "LINES" and head.masks[0]["lines_only"] is True
assert pick(0.75, 0.5) == {'FINISHED'} and layers.active.name == "head", "no hole"
assert not head.line_shows(0.75, 0.5), "head's lines still end at ear's edge"
assert head.mask_summary == "Shows: lines: everything but ear, lines ending at the edge", head.mask_summary
assert oanim.ops.mask.set(layer=str(head.id), index="0", cuts="EARS") == {'CANCELLED'}
# lines_only= as before.
assert oanim.ops.mask.set(layer=str(head.id), index="0", lines_only="0") == {'FINISHED'}
assert head.masks[0]["cuts"] == "ALL"

# Cuts FILLS: the fill is cut, the lines aren't (nor ended: End Lines at Edge does nothing then).
assert oanim.ops.mask.set(layer=str(head.id), index="0", cuts="FILLS") == {'FINISHED'}
layers.active = find("ear")
assert pick(0.75, 0.5) == {'CANCELLED'}, "head's fill is cut there"
assert head.line_shows(0.75, 0.5), "its lines aren't ended"
assert head.mask_summary == "Shows: fills: everything but ear", head.mask_summary
assert oanim.ops.mask.set(layer=str(head.id), index="0", cuts="LINES") == {'FINISHED'}

# Only the rows that cut just lines drop out for fills: other rows of the same layer still cut them.
hole = square("hole", -0.5, -0.5, 0.0, 0.0)
hole.visible = False
layers.active = head
assert oanim.ops.mask.add(sources="hole", mode="SUBTRACT") == {'FINISHED'}
layers.active = find("ear")
assert pick(-0.25, -0.25) == {'CANCELLED'}, "the other row still cuts head's fill"
assert pick(-0.75, -0.75) == {'FINISHED'} and layers.active.name == "head", "outside the hole it shows"
layers.active = find("ear")
assert pick(0.75, 0.5) == {'FINISHED'} and layers.active.name == "head"

# Duplicate: a copy just below.
assert oanim.ops.mask.duplicate(layer=str(head.id), index="0") == {'FINISHED'}
assert [m["layers"] for m in head.masks] == [["ear"], ["ear"], ["hole"]]
assert head.masks[1]["cuts"] == "LINES"
assert oanim.ops.mask.remove(layer=str(head.id), index="1") == {'FINISHED'}

# Saved with the file; a driver can switch it.
path = str(pathlib.Path(tempfile.mkdtemp()) / "cuts.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
head = find("head")
assert head.masks[0]["cuts"] == "LINES" and head.masks[1]["cuts"] == "ALL"
assert oanim.ops.anim.driver_add(layer=str(head.id), path="masks[0].cuts", expression="0") == {'FINISHED'}
doc.frame_current = 1
layers.active = find("ear")
assert head.masks[0]["cuts"] == "ALL", "driven to All"
assert pick(0.75, 0.5) == {'CANCELLED'}, "the hole again"
assert oanim.ops.anim.driver_remove(layer=str(head.id), path="masks[0].cuts") == {'FINISHED'}
doc.frame_current = 0

# Join Lines: from nothing, two picked layers get the pair of rows; the lower one's row cuts only
# lines, so its fill covers the overlap. Running it again reuses the rows.
assert oanim.ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
cheek = square("cheek", -1.0, -1.0, 1.0, 1.0)
nose = square("nose", 0.5, 0.0, 2.0, 1.0)
assert oanim.ops.mask.join_lines(layers="nose") == {'CANCELLED'}, "takes two"
assert oanim.ops.mask.join_lines(layers="nose|cheek") == {'FINISHED'}
cheek, nose = find("cheek"), find("nose")
lower, upper = (cheek, nose) if cheek.masks[0]["cuts"] == "LINES" else (nose, cheek)
for l, other in ((cheek, nose), (nose, cheek)):
    (m,) = l.masks
    assert (m["layers"], m["mode"], m["shape"], m["inside_stroke"], m["end_lines"], m["ends"]) == \
        ([other.name], "SUBTRACT", "FILLS_ONLY", True, True, "BRUSH"), m
assert upper.masks[0]["cuts"] == "ALL"
layers.active = upper
assert pick(0.75, 0.5) == {'FINISHED'} and layers.active.name == lower.name, "the lower fill covers the overlap"
assert not cheek.line_shows(0.75, 0.5) and not nose.line_shows(0.75, 0.5), "both lines end at the join"
assert oanim.ops.mask.join_lines(layers="cheek|nose") == {'FINISHED'}
assert len(find("cheek").masks) == 1 and len(find("nose").masks) == 1, "rows reused"

# The summary says how far a row reaches with Extend.
assert oanim.ops.mask.set(layer=str(find("cheek").id), index="0", extend="UP") == {'FINISHED'}
assert find("cheek").mask_summary == "Shows: lines: everything but nose and above, lines ending at the edge",     find("cheek").mask_summary

print("mask_cuts: ok")
