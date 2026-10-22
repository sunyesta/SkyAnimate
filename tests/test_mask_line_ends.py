"""End Lines at Edge: a mask row can end the masked layer's lines where they cross its shape (worked
out along them before a brush lays its dabs) instead of slicing them; Ends says how the new ends look
(BRUSH: the brush's taper and fade, FLAT: none). Fills are masked as always. layer.line_shows(x, y)
asks the same test the renderer uses.

Run with:  OpenAnimate --background --python tests/test_mask_line_ends.py
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


art = square("art", -3.0, -3.0, 3.0, 3.0)
head = square("head", -1.0, -1.0, 1.0, 1.0)
head.visible = False
layers.active = art

# No row with it on: every line shows, wherever.
assert art.line_shows(0.0, 0.0) and art.line_shows(2.5, 0.0)
assert oanim.ops.mask.add(sources="head", mode="SUBTRACT") == {'FINISHED'}
assert art.masks[0]["end_lines"] is False and art.masks[0]["ends"] == "BRUSH"
assert art.line_shows(0.0, 0.0), "a row without End Lines at Edge slices lines (pixels), it doesn't end them"

# On: Subtract head, so the lines end inside it and show outside.
assert oanim.ops.mask.set(index="0", end_lines="1") == {'FINISHED'}
assert art.masks[0]["end_lines"] is True
assert not art.line_shows(0.0, 0.0), "inside the cut-away shape: no line"
assert art.line_shows(2.5, 0.0), "outside it: the line shows"
assert art.mask_summary == "Shows: everything but head, lines ending at the edge", art.mask_summary

# Invert, and Add / Subtract of the outside read as their steps do.
assert oanim.ops.mask.set(index="0", invert="1") == {'FINISHED'}
assert art.line_shows(0.0, 0.0) and not art.line_shows(2.5, 0.0), "Invert: only inside head"
assert oanim.ops.mask.set(index="0", invert="1", mode="SUBTRACT") == {'FINISHED'}
assert art.line_shows(0.0, 0.0) and not art.line_shows(2.5, 0.0), "Subtract outside: only inside head"

# Strokes Only: just along head's line (its fill doesn't count).
head.set_shape_style(0, line_width=0.1)
assert oanim.ops.mask.set(index="0", mode="ADD", invert="0", shape="STROKES_ONLY") == {'FINISHED'}
assert not art.line_shows(0.0, 0.0), "Strokes Only: not inside"
assert art.line_shows(-1.0, -1.0), "Strokes Only: through the line's corner point"

# Fills Only with Inside the Stroke: the fill ends at the line's inner edge (0.05 in).
assert oanim.ops.mask.set(index="0", shape="FILLS_ONLY") == {'FINISHED'}
assert art.line_shows(0.0, 0.0)
corner_inside = (-0.98, -0.98)
assert art.line_shows(*corner_inside), "Fills Only: up to the middle of the line"
assert oanim.ops.mask.set(index="0", inside_stroke="1") == {'FINISHED'}
assert not art.line_shows(*corner_inside), "Inside the Stroke: not within the line"
assert art.line_shows(0.0, 0.0)

# Ends: BRUSH or FLAT; anything else is refused.
assert oanim.ops.mask.set(index="0", ends="FLAT") == {'FINISHED'} and art.masks[0]["ends"] == "FLAT"
assert oanim.ops.mask.set(index="0", ends="ROUND") == {'CANCELLED'}
assert art.masks[0]["ends"] == "FLAT"

# Reach: line widths past the edge (Brush ends), -2 to 2, 0 by default. It moves the ends, not where
# the edge is, so line_shows doesn't change.
assert art.masks[0]["reach"] == 0.0
assert oanim.ops.mask.set(index="0", ends="BRUSH", reach="0.5") == {'FINISHED'}
assert art.masks[0]["reach"] == 0.5
assert oanim.ops.mask.set(index="0", reach="7") == {'FINISHED'} and art.masks[0]["reach"] == 2.0, "clamped"
assert oanim.ops.mask.set(index="0", reach="-0.25") == {'FINISHED'} and art.masks[0]["reach"] == -0.25
assert not art.line_shows(*corner_inside) and art.line_shows(0.0, 0.0)

# Off in the viewport but on in renders: the render's test still cuts.
assert oanim.ops.mask.set(index="0", shape="AS_DRAWN", inside_stroke="0", mode="SUBTRACT", enabled="0") == {'FINISHED'}
assert art.line_shows(0.0, 0.0), "the row is off in the viewport"
assert not art.line_shows(0.0, 0.0, render=True), "but on in renders"
assert oanim.ops.mask.set(index="0", enabled="1") == {'FINISHED'}

# A group's row ends the lines of every layer inside it.
oanim.ops.layer.new_group()
group = layers.active
inner = square("inner", -3.0, -3.0, 3.0, 3.0)
assert oanim.ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
layers.active = group
assert oanim.ops.mask.add(sources="head", mode="SUBTRACT", end_lines="1") == {'FINISHED'}
inner = find("inner")
assert not inner.line_shows(0.0, 0.0) and inner.line_shows(2.5, 0.0), "the group's row ends its children's lines"

# A card's steps make one shape, and its lines end at that shape's edge: - head, + patch brings them
# back inside patch, ending them where the combined shape's edge is.
patch = square("patch", -0.5, -0.5, 0.5, 0.5)
patch.visible = False
oanim.ops.layer.new()
lone = layers.active
lone.name = "lone"
lone.add_curve([(-3.0, -3.0), (3.0, -3.0), (3.0, 3.0), (-3.0, 3.0)], closed=True, fill=True)
assert oanim.ops.mask.add(layer=str(lone.id), sources="head", mode="SUBTRACT", end_lines="1") == {'FINISHED'}
assert not lone.line_shows(0.0, 0.0), "head ends the lines"
assert oanim.ops.mask.add(layer=str(lone.id), card="0", sources="patch", mode="ADD") == {'FINISHED'}
assert len(lone.masks) == 1 and [s["layers"] for s in lone.masks[0]["steps"]] == [["head"], ["patch"]]
assert lone.line_shows(0.0, 0.0), "+ patch brings them back"
assert not lone.line_shows(0.75, 0.0), "only where it adds"
assert lone.mask_summary == "Shows: everything but head, plus patch, lines ending at the edge", lone.mask_summary
# The step as - Shape: hidden there too.
assert oanim.ops.mask.set(layer=str(lone.id), index="0", step="1", mode="SUBTRACT") == {'FINISHED'}
assert not lone.line_shows(0.0, 0.0)
assert oanim.ops.mask.set(layer=str(lone.id), index="0", step="1", mode="ADD") == {'FINISHED'}
# Another card that doesn't end lines slices them by pixels: their ends don't change.
assert oanim.ops.mask.add(layer=str(lone.id), sources="patch", mode="SUBTRACT") == {'FINISHED'}
assert lone.line_shows(0.0, 0.0) and not lone.line_shows(0.75, 0.0)
assert oanim.ops.mask.remove(layer=str(lone.id), index="1") == {'FINISHED'}
assert oanim.ops.mask.set(layer=str(lone.id), index="0", end_lines="0") == {'FINISHED'}
assert lone.line_shows(0.0, 0.0) and lone.line_shows(0.75, 0.0), "no card ends lines now"
assert oanim.ops.mask.remove(layer=str(lone.id)) == {'FINISHED'}

# Saved with the file, and a driver can switch it.
path = str(pathlib.Path(tempfile.mkdtemp()) / "line_ends.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
art = find("art")
assert (art.masks[0]["end_lines"], art.masks[0]["ends"]) == (True, "BRUSH"), art.masks[0]
assert art.masks[0]["reach"] == -0.25, art.masks[0]
assert not art.line_shows(0.0, 0.0)

# Reach is keyed like any setting: a key at 10 with another value, and it changes between them.
path_reach = "masks[0].reach"
assert oanim.ops.anim.keyframe_toggle(layer=str(art.id), paths=path_reach) == {'FINISHED'}
doc.frame_current = 10
assert oanim.ops.mask.set(layer=str(art.id), index="0", reach="1") == {'FINISHED'}
doc.frame_current = 0
assert art.masks[0]["reach"] == -0.25, art.masks[0]["reach"]
doc.frame_current = 10
assert art.masks[0]["reach"] == 1.0, art.masks[0]["reach"]
doc.frame_current = 5
assert -0.25 < art.masks[0]["reach"] < 1.0, art.masks[0]["reach"]
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
art = find("art")
doc.frame_current = 10
assert art.masks[0]["reach"] == 1.0, "keys saved"
assert oanim.ops.anim.driver_add(layer=str(art.id), path=path_reach, expression="0.75") == {'FINISHED'}
doc.frame_current = 3
assert art.masks[0]["reach"] == 0.75, "driven"
assert oanim.ops.anim.driver_remove(layer=str(art.id), path=path_reach) == {'FINISHED'}
doc.frame_current = 0
assert oanim.ops.anim.driver_add(layer=str(art.id), path="masks[0].end_lines", expression="0") == {'FINISHED'}
doc.frame_current = 1
assert art.masks[0]["end_lines"] is False and art.line_shows(0.0, 0.0), "driven off"

print("mask_line_ends: ok")
