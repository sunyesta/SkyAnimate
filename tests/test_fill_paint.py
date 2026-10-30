"""The Fill Pen and Fill Eraser (data/fill_paint): painted areas, merging, cutting, and lines left alone.

The live preview and Ctrl with a real press need the window: checked live on a private copy.

Run with:  OpenAnimate --background --python tests/test_fill_paint.py
"""

import math

import oanim

wm = oanim.wm
ops = oanim.ops
doc = oanim.data.document
pixel = 2.0 / doc.height


def samples(path, count=80, seconds=0.5, pressure=1.0):
    rows = []
    for i in range(count):
        u = i / (count - 1)
        x, y = path(u)
        rows.append("%f,%f,%f,%f" % (x, y, pressure, u * seconds))
    return ";".join(rows)


def stroke(path, **kwargs):
    extra = {k: kwargs.pop(k) for k in list(kwargs) if k == "erase"}
    return ops.draw.stroke(samples=samples(path, **kwargs), pixel=pixel, **extra)


def line(ax, ay, bx, by):
    return lambda u: (ax + (bx - ax) * u, ay + (by - ay) * u)


def covers(layer, shape, x, y):
    return layer.shape_contains(shape, (x, y))


def fresh():
    ops.layer.new()
    return oanim.data.layers.active


# --- The kinds of pen, and each remembering its own -------------------------------------------------------------------
presets = {p["name"]: p for p in wm.draw_presets()}
assert presets["Fill Pen"]["mode"] == "FILL_PEN" and presets["Fill Pen"]["category"] == "Pens"
assert presets["Fill Eraser"]["mode"] == "FILL_ERASER" and presets["Fill Eraser"]["category"] == "Erasers"

ops.wm.tool_draw(mode="FILL_PEN")
assert wm.draw_settings()["mode"] == "FILL_PEN" and wm.draw_settings()["preset"] == "Fill Pen"
line_width = wm.draw_settings()["line_width"]
ops.draw.preset_load(name="Fill Marker")
assert wm.draw_settings()["preset"] == "Fill Marker" and wm.draw_settings()["line_width"] == line_width, "not the lines' width"
ops.wm.tool_draw(mode="FREEHAND")
assert wm.draw_settings()["preset"] == "Ink"
ops.wm.tool_draw(mode="FILL_ERASER")
assert wm.draw_settings()["preset"] == "Fill Eraser"
ops.wm.tool_draw(mode="FILL_PEN")
assert wm.draw_settings()["preset"] == "Fill Marker", "it comes back to the one it last used"
ops.draw.preset_load(name="Fill Pen")

# --- The Fill Pen paints a filled area, with no outline ---------------------------------------------------------------
layer = fresh()
wm.set_draw(fill_size=0.1)
assert stroke(line(-0.5, 0.0, 0.5, 0.0), pressure=1.0) == {"FINISHED"}
assert layer.shape_count == 1 and layer.curve_count == 1
look = layer.shape_style(0)
assert look["has_fill"] and not look["has_outline"], look
assert covers(layer, 0, 0.0, 0.0) and covers(layer, 0, 0.0, 0.045)
assert not covers(layer, 0, 0.0, 0.07), "as wide as the brush"
assert covers(layer, 0, 0.53, 0.0), "round at its end"
assert 3 <= layer.point_count <= 16, ("few points", layer.point_count)
ops.ed.undo()
assert layer.shape_count == 0, "one undo step"
ops.ed.redo()
assert layer.shape_count == 1

# A loop stays one area, a hole in it staying a hole.
layer = fresh()
wm.set_draw(fill_size=0.06)
circle = lambda u: (0.3 * math.cos(u * 2.0 * math.pi), 0.3 * math.sin(u * 2.0 * math.pi))
stroke(circle, count=200, seconds=1.0)
assert layer.shape_count == 1 and layer.curve_count == 2, (layer.shape_count, layer.curve_count)
assert covers(layer, 0, 0.3, 0.0) and covers(layer, 0, -0.3, 0.0)
assert not covers(layer, 0, 0.0, 0.0), "the hole"

# Overlapping the same colour, it merges; a stroke elsewhere stays its own shape.
layer = fresh()
wm.set_draw(fill_size=0.1)
stroke(line(-0.5, 0.0, 0.0, 0.0))
stroke(line(0.5, 0.5, 0.8, 0.5))
assert layer.shape_count == 2
stroke(line(-0.1, 0.0, 0.4, 0.0))
assert layer.shape_count == 2, ("merged", layer.shape_count)
merged = [i for i in range(layer.shape_count) if covers(layer, i, -0.4, 0.0)][0]
assert covers(layer, merged, 0.35, 0.0), "one area"
# Bridging two: three become one area.
stroke(line(0.35, 0.0, 0.6, 0.5))
assert layer.shape_count == 1, layer.shape_count
assert covers(layer, 0, -0.4, 0.0) and covers(layer, 0, 0.75, 0.5)

# Another colour goes on top, unmerged.
layer = fresh()
stroke(line(-0.5, 0.0, 0.5, 0.0))
wm.set_draw(fill_color=(0.2, 0.4, 0.8, 1.0))
stroke(line(0.0, -0.3, 0.0, 0.3))
assert layer.shape_count == 2
assert layer.shape_style(1)["fill"] != layer.shape_style(0)["fill"]
# And painting the first colour over that doesn't merge under it.
wm.set_draw(fill_color=(0.95, 0.85, 0.7, 1.0))
stroke(line(-0.2, 0.0, 0.2, 0.0))
assert layer.shape_count == 3, layer.shape_count

# Merging keeps its place: a line drawn over a colour stays over it.
layer = fresh()
stroke(line(-0.5, 0.0, 0.0, 0.0))
ops.draw.preset_load(name="Ink")
ops.wm.tool_draw(mode="FREEHAND")
stroke(line(-0.6, 0.1, 0.6, 0.1))
ops.wm.tool_draw(mode="FILL_PEN")
assert layer.shape_count == 2 and layer.shape_style(1)["has_outline"]
stroke(line(-0.1, 0.0, 0.4, 0.0))
assert layer.shape_count == 2, layer.shape_count
assert layer.shape_style(1)["has_outline"], "the line is still on top"
assert layer.shape_style(0)["has_fill"] and covers(layer, 0, 0.35, 0.0)

# --- The Fill Eraser cuts fills ------------------------------------------------------------------------------------
layer = fresh()
wm.set_draw(fill_size=0.2)
stroke(line(-0.5, 0.0, 0.5, 0.0))
ops.wm.tool_draw(mode="FILL_ERASER")
wm.set_draw(fill_size=0.1)
assert stroke(line(0.0, -0.4, 0.0, 0.4)) == {"FINISHED"}
assert layer.shape_count == 2, ("in two", layer.shape_count)
assert not any(covers(layer, i, 0.0, 0.0) for i in range(2)), "the cut"
assert any(covers(layer, i, -0.3, 0.0) for i in range(2)) and any(covers(layer, i, 0.3, 0.0) for i in range(2))
ops.ed.undo()
assert layer.shape_count == 1 and covers(layer, 0, 0.0, 0.0)
ops.ed.redo()

# Cutting a hole, and cutting it all away.
layer = fresh()
ops.wm.tool_draw(mode="FILL_PEN")
wm.set_draw(fill_size=0.3)
stroke(line(-0.5, 0.0, 0.5, 0.0))
ops.wm.tool_draw(mode="FILL_ERASER")
wm.set_draw(fill_size=0.05)
stroke(line(0.0, 0.0, 0.05, 0.0))
assert layer.shape_count == 1 and layer.curve_count == 2
assert not covers(layer, 0, 0.02, 0.0) and covers(layer, 0, 0.3, 0.0)
wm.set_draw(fill_size=0.8)
stroke(line(-0.7, 0.0, 0.7, 0.0))
assert layer.shape_count == 0 and layer.point_count == 0, (layer.shape_count, layer.point_count)
# Missing everything is no step at all.
assert stroke(line(-0.7, 0.0, 0.7, 0.0)) == {"CANCELLED"}

# Lines are left alone: a filled shape keeps its outline whole, with the cut only in its fill.
layer = fresh()
ops.mesh.primitive_box_add(x=0.0, y=0.0, width=0.8, height=0.8, fill="1")
assert layer.shape_count == 1 and layer.shape_style(0)["has_outline"] and layer.shape_style(0)["has_fill"]
points_before = layer.point_count
ops.wm.tool_draw(mode="FILL_ERASER")
wm.set_draw(fill_size=0.2)
stroke(line(-0.6, 0.0, 0.0, 0.0))
assert layer.shape_count == 2, layer.shape_count
outline = [i for i in range(2) if layer.shape_style(i)["has_outline"]]
fills = [i for i in range(2) if layer.shape_style(i)["has_fill"]]
assert len(outline) == 1 and len(fills) == 1 and outline != fills, (outline, fills)
assert not layer.shape_style(outline[0])["has_fill"]
assert fills[0] < outline[0], "the line over what is left of the fill"
assert not covers(layer, fills[0], -0.2, 0.0) and covers(layer, fills[0], 0.2, 0.0)
assert layer.curve_count == 2

# Ctrl with the Fill Pen cuts.
layer = fresh()
ops.wm.tool_draw(mode="FILL_PEN")
wm.set_draw(fill_size=0.2)
stroke(line(-0.5, 0.0, 0.5, 0.0))
assert stroke(line(0.0, -0.4, 0.0, 0.4), erase="1") == {"FINISHED"}
assert layer.shape_count == 2, layer.shape_count

# Selected Only.
layer = fresh()
stroke(line(-0.5, 0.0, -0.2, 0.0))
stroke(line(0.2, 0.0, 0.5, 0.0))
assert layer.shape_count == 2
ops.wm.tool_draw(mode="FILL_ERASER")
wm.set_draw(erase_selected=True, fill_size=0.3)
assert stroke(line(-0.7, 0.0, 0.7, 0.0)) == {"CANCELLED"}, "nothing selected"
assert layer.shape_count == 2
left = [i for i in range(2) if covers(layer, i, -0.4, 0.0)][0]
layer.select_points([p for p in range(layer.point_count) if layer.points[p][0] < 0.0], True)
assert stroke(line(-0.7, 0.0, 0.7, 0.0)) == {"FINISHED"}
assert layer.shape_count == 1 and covers(layer, 0, 0.3, 0.0), "only the selected one went"
wm.set_draw(erase_selected=False)

# All Layers: the same place on other drawings, through their own transforms.
below = fresh()
ops.wm.tool_draw(mode="FILL_PEN")
wm.set_draw(fill_size=0.2)
stroke(line(-0.5, 0.0, 0.5, 0.0))
above = fresh()
stroke(line(-0.5, 0.0, 0.5, 0.0))
below.selected = True
above.selected = True
oanim.data.layers.active = above
ops.wm.tool_draw(mode="FILL_ERASER")
wm.set_draw(fill_size=0.1)
stroke(line(0.0, -0.4, 0.0, 0.4))
assert above.shape_count == 2 and below.shape_count == 1, "the active one only, at first"
wm.set_draw(erase_all_layers=True)
stroke(line(0.2, -0.4, 0.2, 0.4))
assert below.shape_count == 2 and above.shape_count == 3, (below.shape_count, above.shape_count)
wm.set_draw(erase_all_layers=False)
below.selected = False

# A shape with keys is left as it is, rather than losing them.
layer = fresh()
ops.wm.tool_draw(mode="FILL_PEN")
wm.set_draw(fill_size=0.2)
stroke(line(-0.5, 0.0, 0.5, 0.0))
doc.frame_current = 10
ops.sculpt.stroke(brush="GRAB", path="-0.5,0.0;-0.5,0.1", radius=0.3, strength=1.0, falloff="CONSTANT")
doc.frame_current = 0
points = layer.point_count
ops.wm.tool_draw(mode="FILL_ERASER")
assert stroke(line(0.0, -0.4, 0.0, 0.4)) == {"CANCELLED"}
assert layer.shape_count == 1 and layer.point_count == points
# And painting over it goes on top, rather than merging.
ops.wm.tool_draw(mode="FILL_PEN")
stroke(line(0.0, 0.0, 0.3, 0.0))
assert layer.shape_count == 2, layer.shape_count

print("fill paint: ok")
