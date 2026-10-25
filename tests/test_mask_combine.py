"""A mask step's Combine: how its layers make its shape. Union (the default) is all of them together,
Difference the first with the others cut out of it, Intersection only where all of them are. Picking
(the masks' test on the CPU) and End Lines at Edge follow it, it is said in the summary, undone,
driven and saved with the file.

Run with:  OpenAnimate --background --python tests/test_mask_combine.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_mask_combine_"))


def find(name):
    return next(l for l in layers.all() if l.name == name)


def square(name, x0, x1, hidden=True):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(x0, x0), (x1, x0), (x1, x1), (x0, x1)], closed=True, fill=True)
    l.visible = not hidden
    return l


def picks(x, y):
    """Whether Alt-click picks art at (x, y): masks hide what they hide."""
    layers.active = find("Layer 1")
    if oanim.ops.layer.select_at(x=str(x), y=str(y)) != {'FINISHED'}:
        return False
    return layers.active.name == "art"


A_ONLY, BOTH, B_ONLY, NEITHER = (-0.5, -0.5), (0.5, 0.5), (1.5, 1.5), (2.5, 2.5)


def shown():
    return tuple(picks(*p) for p in (A_ONLY, BOTH, B_ONLY, NEITHER))


square("a", -1.0, 1.0)
square("b", 0.0, 2.0)
art = square("art", -3.0, 3.0, hidden=False)
layers.active = art
assert oanim.ops.mask.add(sources="a|b", mode="ADD") == {'FINISHED'}
step = art.masks[0]["steps"][0]
assert step["combine"] == "UNION", "Union by default"
assert shown() == (True, True, True, False), shown()
assert art.mask_summary == "Shows: inside a + b", art.mask_summary

# Difference: a with b cut out of it.
layers.active = art
assert oanim.ops.mask.set(index="0", step="0", combine="DIFFERENCE") == {'FINISHED'}
assert art.masks[0]["steps"][0]["combine"] == "DIFFERENCE"
assert shown() == (True, False, False, False), shown()
assert art.mask_summary == "Shows: inside a - b", art.mask_summary

# The order counts for Difference: b first keeps b's part.
layers.active = art
assert oanim.ops.mask.set(index="0", step="0", sources="b|a") == {'FINISHED'}
assert shown() == (False, False, True, False), shown()

# Intersection: only where both are.
layers.active = art
assert oanim.ops.mask.set(index="0", step="0", combine="INTERSECTION") == {'FINISHED'}
assert shown() == (False, True, False, False), shown()
assert art.mask_summary == "Shows: inside b & a", art.mask_summary

# With Invert, the shape inside out; with - Shape on everything, cut away.
layers.active = art
assert oanim.ops.mask.set(index="0", step="0", invert="1") == {'FINISHED'}
assert shown() == (True, False, True, True), shown()
layers.active = art
assert oanim.ops.mask.set(index="0", step="0", invert="0", mode="SUBTRACT") == {'FINISHED'}
assert art.masks[0]["start"] == "EVERYTHING"
assert shown() == (True, False, True, True), shown()

# A missing layer is left out; one layer alone is its own shape, whatever the mode.
layers.active = art
assert oanim.ops.mask.set(index="0", step="0", mode="ADD", sources="a") == {'FINISHED'}
assert art.masks[0]["start"] == "NOTHING"
assert shown() == (True, True, False, False), shown()
layers.active = art
assert oanim.ops.mask.set(index="0", step="0", sources="a|b") == {'FINISHED'}

# A name it doesn't know is refused.
layers.active = art
assert oanim.ops.mask.set(index="0", step="0", combine="XOR") == {'CANCELLED'}
assert art.masks[0]["steps"][0]["combine"] == "INTERSECTION"

# Undone like any setting.
layers.active = art
assert oanim.ops.mask.set(index="0", step="0", combine="UNION") == {'FINISHED'}
assert find("art").masks[0]["steps"][0]["combine"] == "UNION"
oanim.ops.ed.undo()
art = find("art")
assert art.masks[0]["steps"][0]["combine"] == "INTERSECTION"
assert shown() == (False, True, False, False), shown()

# End Lines at Edge ends the lines where the combined shape's edge is.
layers.active = art
assert oanim.ops.mask.set(index="0", end_lines="1") == {'FINISHED'}
art = find("art")
assert art.line_shows(*BOTH) and not art.line_shows(*A_ONLY) and not art.line_shows(*B_ONLY)
layers.active = art
assert oanim.ops.mask.set(index="0", step="0", combine="DIFFERENCE") == {'FINISHED'}
art = find("art")
assert art.line_shows(*A_ONLY) and not art.line_shows(*BOTH) and not art.line_shows(*B_ONLY)
layers.active = art
assert oanim.ops.mask.set(index="0", end_lines="0") == {'FINISHED'}

# Saved with the file; a file from before it opens as Union.
path = str(folder / "combine.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
art = find("art")
assert art.masks[0]["steps"][0]["combine"] == "DIFFERENCE"
assert shown() == (True, False, False, False), shown()

# A second step combines on its own: + Shape b|a as Intersection on top of a - b shows a too.
layers.active = art
assert oanim.ops.mask.add(card="0", sources="b|a", mode="ADD", combine="INTERSECTION") == {'FINISHED'}
art = find("art")
assert [s["combine"] for s in art.masks[0]["steps"]] == ["DIFFERENCE", "INTERSECTION"]
assert shown() == (True, True, False, False), shown()

# Drivable: masks[i].steps[j].combine, 0 Union, 1 Difference, 2 Intersection.
layers.active = art
assert oanim.ops.anim.driver_add(layer=str(art.id), path="masks[0].steps[1].combine", expression="1") == {'FINISHED'}
doc.frame_current = 1
doc.frame_current = 0
art = find("art")
assert art.masks[0]["steps"][1]["combine"] == "DIFFERENCE", art.masks[0]["steps"][1]

print("mask_combine: ok")
