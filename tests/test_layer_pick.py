"""Select Layer (the tool, and Alt-click from any tool): layer.select_at picks the topmost drawing layer
shown at a point (Draw Order included), only where its masks let it show; media, hidden layers, hidden
guides and layers outside Local View are never picked; Shift adds or takes out, Ctrl takes out, and
Pick: Group takes the outermost group.

Run with:  OpenAnimate --background --python tests/test_layer_pick.py
"""

import os

import oanim

HERE = os.path.dirname(os.path.abspath(__file__))
STILL = os.path.join(HERE, "data", "media", "still.png")

layers = oanim.data.layers


def find(name):
    return next(l for l in layers.all() if l.name == name)


def square(name, x0, y0, x1, y1, fill=True):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], closed=True, fill=fill)
    return l


def pick(x, y, **args):
    return oanim.ops.layer.select_at(x=str(x), y=str(y), **{k: str(v) for k, v in args.items()})


def picked():
    return sorted(l.name for l in layers.all() if l.select or l == layers.active)


# Bottom to top: back (big), front (small, over back's middle).
back = square("back", -1.0, -1.0, 1.0, 1.0)
front = square("front", -0.2, -0.2, 0.2, 0.2)

layers.active = back
assert pick(0.0, 0.0) == {'FINISHED'} and layers.active.name == "front", "the topmost shown there"
assert pick(0.0, 0.0) == {'CANCELLED'}, "picked already: nothing changes (no undo step)"
assert pick(0.6, 0.6) == {'FINISHED'} and layers.active.name == "back"
assert picked() == ["back"], "a plain pick takes it alone"
assert oanim.wm.undo_history()[0][-1] == "Select back"
assert pick(5.0, 5.0) == {'CANCELLED'} and layers.active.name == "back", "nothing there: the selection stays"

# Shift (TOGGLE) adds, and takes out a picked one; Ctrl (REMOVE) takes out.
assert pick(0.0, 0.0, mode="TOGGLE") == {'FINISHED'}
assert picked() == ["back", "front"] and layers.active.name == "front"
assert pick(0.0, 0.0, mode="TOGGLE") == {'FINISHED'} and picked() == ["back"], "Shift again takes it out"
assert pick(0.0, 0.0, mode="TOGGLE") == {'FINISHED'}
assert pick(0.6, 0.6, mode="REMOVE") == {'FINISHED'} and picked() == ["front"]
assert pick(0.6, 0.6, mode="SIDEWAYS") == {'CANCELLED'}

# A named layer (the viewport's click-again-for-the-one-below, and bones) and undo.
assert oanim.ops.layer.select_at(layer="back") == {'FINISHED'} and picked() == ["back"]
oanim.ops.ed.undo()
assert picked() == ["front"]

# Lines count near them (tolerance); an outline-only shape's inside doesn't.
ring = square("ring", 2.0, 2.0, 3.0, 3.0, fill=False)
layers.active = back
assert pick(2.5, 2.5) == {'CANCELLED'}, "inside an unfilled outline is empty"
assert pick(2.0, 2.0, tolerance="0.01") == {'FINISHED'} and layers.active.name == "ring"

# Hidden layers aren't picked: the one below is.
front.visible = False
assert pick(0.0, 0.0) == {'FINISHED'} and layers.active.name == "back"
front.visible = True

# Media layers are never picked, even on top.
assert oanim.ops.media.add(filepath=STILL) == {'FINISHED'}
media = layers.active
assert pick(0.0, 0.0) == {'FINISHED'} and layers.active.name == "front", "the media layer on top is skipped"
oanim.ops.layer.select_at(layer=media.name)
oanim.ops.layer.delete()

# Where a mask hides a layer, it isn't picked: the one below is.
layers.active = front
hole = square("hole", -0.1, -0.1, 0.1, 0.1)
hole.visible = False
layers.active = front
assert oanim.ops.mask.add(sources="hole", mode="SUBTRACT") == {'FINISHED'}
assert pick(0.0, 0.0) == {'FINISHED'} and layers.active.name == "back", "front is cut away there"
assert pick(0.15, 0.15) == {'FINISHED'} and layers.active.name == "front", "and shows around the hole"
layers.active = front
oanim.ops.mask.remove()

# The mask's Shape: Strokes Only shows the layer along the mask's line, not inside it; Fills Only
# inside, not along the line outside the fill.


def top_at(x, y):
    """The layer picked at (x, y), front left active after (the masks below are front's)."""
    layers.active = back
    pick(x, y)
    name = layers.active.name
    layers.active = front
    return name


hole.set_shape_style(0, line_width=0.04)
layers.active = front
assert oanim.ops.mask.add(sources="hole", mode="ADD", shape="STROKES_ONLY") == {'FINISHED'}
# (A curve through the square's corners bulges: its line crosses y = 0 near x = 0.13.)
assert top_at(0.0, 0.0) == "back", "Strokes Only: nothing inside"
assert top_at(0.13, 0.0) == "front", "Strokes Only: along the line"
assert front.mask_summary == "Shows: on hole's lines", front.mask_summary
assert top_at(0.1, 0.1) == "front", "Strokes Only: through a corner"
assert oanim.ops.mask.set(index="0", shape="FILLS_ONLY") == {'FINISHED'}
assert top_at(0.0, 0.0) == "front", "Fills Only: inside"
assert top_at(0.145, 0.0) == "back", "Fills Only: not the line's outer half"
# Inside the Stroke: the fill ends at the line's inner edge (line 0.04 wide: 0.02 in from the outline).
assert top_at(0.125, 0.0) == "front", "Fills Only reaches the line's middle"
assert oanim.ops.mask.set(index="0", inside_stroke="1") == {'FINISHED'}
assert top_at(0.125, 0.0) == "back", "Inside the Stroke: not within the line"
assert top_at(0.0, 0.0) == "front", "Inside the Stroke: inside still masks"
assert oanim.ops.mask.set(index="0", inside_stroke="0") == {'FINISHED'}
oanim.ops.mask.remove()
# Fill Every Shape: an outline-only shape masks with its inside too.
outline = square("outline", -0.1, -0.1, 0.1, 0.1, fill=False)
outline.visible = False
layers.active = front
assert oanim.ops.mask.add(sources="outline", mode="ADD") == {'FINISHED'}
assert top_at(0.0, 0.0) == "back", "an outline's inside doesn't mask"
assert oanim.ops.mask.set(index="0", filled="1") == {'FINISHED'}
assert top_at(0.0, 0.0) == "front", "Fill Every Shape: it does"
assert oanim.ops.mask.set(index="0", shape="STROKES_ONLY") == {'FINISHED'}
assert top_at(0.0, 0.0) == "back", "Strokes Only has no inside to fill"
oanim.ops.mask.remove()

# A mask layer lends its shapes as drawn: its own masks don't cut the shape it lends (they only mask
# how it shows itself).
layers.active = outline
assert oanim.ops.mask.add(sources="hole", mode="SUBTRACT") == {'FINISHED'}, "outline's own mask: minus hole"
layers.active = front
assert oanim.ops.mask.add(sources="outline", mode="ADD", filled="1") == {'FINISHED'}
assert top_at(0.0, 0.0) == "front", "outline's own mask doesn't cut what it lends front"
oanim.ops.mask.remove()
layers.active = outline
oanim.ops.mask.remove()

# Guides (Show in Renders off) are picked while guides show, not while they're hidden.
front.render_visible = False
layers.active = back
assert pick(0.0, 0.0) == {'FINISHED'} and layers.active.name == "front"
oanim.wm.set_overlay("guides", False)
assert pick(0.0, 0.0) == {'FINISHED'} and layers.active.name == "back"
oanim.wm.set_overlay("guides", True)
front.render_visible = True

# Local View: only its layers.
layers.active = back
for l in layers.all():
    l.select = False
assert oanim.ops.view.local_view() == {'FINISHED'}
layers.active = ring
assert pick(0.0, 0.0) == {'FINISHED'} and layers.active.name == "back", "front isn't in Local View"
assert oanim.ops.view.local_view() == {'FINISHED'}

# Pick: Group takes the outermost group of the layer clicked.
layers.active = front
assert oanim.ops.layer.group() == {'FINISHED'}
group = layers.active
group.name = "character"
layers.active = back
assert pick(0.0, 0.0) == {'FINISHED'} and layers.active.name == "front", "the layer itself by default"
assert pick(0.0, 0.0, group="1") == {'FINISHED'} and layers.active.name == "character"

print("test_layer_pick: OK")
