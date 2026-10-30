"""The Stroke Eraser, Vector Trimmer and Trim Overshoots (data/line_cut): lines cut where brushed and out to crossings.

The new ends' caps and the Ctrl and eraser-end shortcuts with a real press need the window: checked live on a private
copy.

Run with:  OpenAnimate --background --python tests/test_line_cut.py
"""

import oanim

wm = oanim.wm
ops = oanim.ops
doc = oanim.data.document
pixel = 2.0 / doc.height


def samples(path, count=60, seconds=0.5, pressure=1.0):
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


def fresh():
    ops.layer.new()
    return oanim.data.layers.active


def draw_line(*ends):
    ops.wm.tool_draw(mode="FREEHAND")
    wm.set_draw(use_pressure=False)
    stroke(line(*ends))


def extents(layer):
    """Each line's outline ends, as x sorted: (x0, x1) for lines along x."""
    spans = []
    for c in range(layer.curve_count):
        xs = [layer.points[p][0] for p in layer.curve_points(c)]
        spans.append((min(xs), max(xs)))
    return sorted(spans)


# --- The kinds of pen --------------------------------------------------------------------------------------------------
presets = {p["name"]: p for p in wm.draw_presets()}
assert presets["Stroke Eraser"]["mode"] == "STROKE_ERASER" and presets["Stroke Eraser"]["category"] == "Erasers"
assert presets["Whole Line Eraser"]["erase_whole"] and not presets["Stroke Eraser"]["erase_whole"]
assert presets["Vector Trimmer"]["mode"] == "TRIMMER" and presets["Vector Trimmer"]["category"] == "Trim"

# --- The Stroke Eraser cuts a line in two, and only there ---------------------------------------------------------------
layer = fresh()
draw_line(-0.5, 0.0, 0.5, 0.0)
assert layer.shape_count == 1 and layer.curve_count == 1
ops.wm.tool_draw(mode="STROKE_ERASER")
wm.set_draw(fill_size=0.1)
assert stroke(line(0.0, -0.4, 0.0, 0.4)) == {"FINISHED"}
assert layer.curve_count == 2, layer.curve_count
spans = extents(layer)
half = wm.draw_settings()["line_width"] * 0.5
assert len(spans) == 2 and abs(spans[0][0] + 0.5) < 0.01 and abs(spans[1][1] - 0.5) < 0.01, spans
assert abs(spans[0][1] + 0.05 + half) < 0.01 and abs(spans[1][0] - 0.05 - half) < 0.01, (spans, half)
ops.ed.undo()
assert layer.curve_count == 1, "one undo step"
ops.ed.redo()
assert layer.curve_count == 2

# A cut keeps the line's width where it falls: no swelling at the new ends.
layer = fresh()
ops.wm.tool_draw(mode="FREEHAND")
wm.set_draw(use_pressure=True, min_size=0.0, line_width=0.05)
ops.draw.stroke(samples=";".join("%f,0,%f,%f" % (-0.5 + i / 49.0, 0.1 + 0.9 * i / 49.0, i * 0.01) for i in range(50)), pixel=pixel)
ops.wm.tool_draw(mode="STROKE_ERASER")
wm.set_draw(fill_size=0.1)
stroke(line(0.0, -0.4, 0.0, 0.4))
assert layer.curve_count == 2
cut_ends = [r for r, p in zip(layer.radii, layer.points) if abs(p[0]) < 0.1]
assert len(cut_ends) == 2 and all(0.4 < r < 0.7 for r in cut_ends), ("the width there, on a line widening across", cut_ends)
wm.set_draw(min_size=0.15, line_width=0.008)
ops.draw.preset_reset(name="Ink")

# Missing every line is no step.
layer = fresh()
draw_line(-0.5, 0.0, 0.5, 0.0)
ops.wm.tool_draw(mode="STROKE_ERASER")
wm.set_draw(fill_size=0.05)
assert stroke(line(-0.5, 0.5, 0.5, 0.5)) == {"CANCELLED"}
assert layer.curve_count == 1

# Along the end: shorter, still one line.
stroke(line(0.3, 0.0, 0.8, 0.0))
assert layer.curve_count == 1
assert extents(layer)[0][1] < 0.28, extents(layer)
# Right along it: it goes.
stroke(line(-0.8, 0.0, 0.8, 0.0))
assert layer.curve_count == 0 and layer.shape_count == 0 and layer.point_count == 0

# A tap takes a nick.
layer = fresh()
draw_line(-0.5, 0.0, 0.5, 0.0)
ops.wm.tool_draw(mode="STROKE_ERASER")
wm.set_draw(fill_size=0.05)
assert stroke(lambda u: (0.0, 0.0), count=3) == {"FINISHED"}
assert layer.curve_count == 2

# Ctrl with the Stroke Pen erases, and the pen's own width isn't what it erases with.
layer = fresh()
draw_line(-0.5, 0.0, 0.5, 0.0)
assert stroke(line(0.0, -0.3, 0.0, 0.3), erase="1") == {"FINISHED"}
assert layer.curve_count == 2 and layer.shape_count >= 1

# Whole Line takes the line it touches, and leaves others.
layer = fresh()
draw_line(-0.5, 0.0, 0.5, 0.0)
draw_line(-0.5, 0.3, 0.5, 0.3)
ops.draw.preset_load(name="Whole Line Eraser")
wm.set_draw(fill_size=0.05)
assert stroke(line(0.0, -0.1, 0.0, 0.1)) == {"FINISHED"}
assert layer.curve_count == 1 and layer.shape_count == 1 and layer.point_count == 2
assert stroke(line(0.0, -0.1, 0.0, 0.1)) == {"CANCELLED"}

# --- Lines cut, fills stay ----------------------------------------------------------------------------------------------
layer = fresh()
ops.mesh.primitive_box_add(x=0.0, y=0.0, width=0.8, height=0.8, fill="1")
assert layer.shape_style(0)["has_fill"] and layer.shape_style(0)["has_outline"]
ops.draw.preset_load(name="Stroke Eraser")
wm.set_draw(fill_size=0.1)
assert stroke(line(-0.1, 0.4, 0.1, 0.4)) == {"FINISHED"}
assert layer.shape_count == 1 and layer.shape_style(0)["has_fill"], "the fill stays"
assert layer.curve_count == 1 and layer.shape_contains(0, (0.0, 0.0)) and layer.shape_contains(0, (0.0, 0.38))
outline = layer.shape_outline(0, 8)
assert len(outline) >= 1
# Erased again: nothing left there to take.
assert stroke(line(-0.1, 0.4, 0.1, 0.4)) == {"CANCELLED"}

# A fill's edge with no line isn't a line to erase.
layer = fresh()
ops.wm.tool_draw(mode="FILL_PEN")
wm.set_draw(fill_size=0.2)
stroke(line(-0.5, 0.0, 0.5, 0.0))
ops.draw.preset_load(name="Stroke Eraser")
wm.set_draw(fill_size=0.05)
assert stroke(line(0.0, -0.4, 0.0, 0.4)) == {"CANCELLED"}

# Keys stay: a line keyed elsewhere can be cut, its points not rebuilt.
layer = fresh()
draw_line(-0.5, 0.0, 0.5, 0.0)
doc.frame_current = 10
ops.sculpt.stroke(brush="GRAB", path="-0.5,0.0;-0.5,0.1", radius=0.2, strength=1.0, falloff="CONSTANT")
doc.frame_current = 0
keyed = [i for i in range(layer.point_count) if layer.keyframes("points[%d].pos" % i)]
assert keyed, "the sculpt keyed it"
ops.draw.preset_load(name="Stroke Eraser")
assert stroke(line(0.2, -0.3, 0.2, 0.3)) == {"FINISHED"}
assert layer.curve_count == 2
assert any(layer.keyframes("points[%d].pos" % i) for i in range(layer.point_count)), "kept"

# --- The Vector Trimmer -------------------------------------------------------------------------------------------------
# A line crossed by two others: touching the middle takes just the piece between them.
layer = fresh()
draw_line(-0.6, 0.0, 0.6, 0.0)
draw_line(-0.2, -0.3, -0.2, 0.3)
draw_line(0.2, -0.3, 0.2, 0.3)
ops.draw.preset_load(name="Vector Trimmer")
wm.set_draw(fill_size=0.02)
assert stroke(lambda u: (0.0, 0.0), count=3) == {"FINISHED"}
assert layer.curve_count == 4, layer.curve_count
spans = sorted(extents(layer))
horizontal = [s for s in spans if s[1] - s[0] > 0.2]
assert len(horizontal) == 2, spans
assert abs(horizontal[0][1] + 0.2) < 0.01 and abs(horizontal[1][0] - 0.2) < 0.01, horizontal
ops.ed.undo()
assert layer.curve_count == 3

# Touching past the last crossing: from it to the end.
assert stroke(lambda u: (0.5, 0.0), count=3) == {"FINISHED"}
spans = [s for s in extents(layer) if s[1] - s[0] > 0.2]
assert abs(spans[0][1] - 0.2) < 0.01, spans
ops.ed.undo()

# A line crossing nothing goes whole, as a tap on a closed one does.
layer = fresh()
draw_line(-0.5, 0.0, 0.5, 0.0)
ops.draw.preset_load(name="Vector Trimmer")
assert stroke(lambda u: (0.0, 0.0), count=3) == {"FINISHED"}
assert layer.curve_count == 0

# A T: an end stopping on the line counts as a crossing, though it doesn't cross.
layer = fresh()
draw_line(-0.5, 0.0, 0.5, 0.0)
draw_line(0.0, 0.002, 0.0, 0.4)
ops.draw.preset_load(name="Vector Trimmer")
assert stroke(lambda u: (0.3, 0.0), count=3) == {"FINISHED"}
spans = sorted(extents(layer))
longest = max(spans, key=lambda s: s[1] - s[0])
assert abs(longest[1]) < 0.01, spans

# Not across layers, unless asked.
under = fresh()
draw_line(-0.2, -0.3, -0.2, 0.3)
over = fresh()
draw_line(-0.6, 0.0, 0.6, 0.0)
under.selected = True
over.selected = True
oanim.data.layers.active = over
ops.draw.preset_load(name="Vector Trimmer")
wm.set_draw(erase_all_layers=True)
assert stroke(lambda u: (0.3, 0.0), count=3) == {"FINISHED"}
spans = extents(over)
assert len(spans) == 1 and abs(spans[0][1] + 0.2) < 0.01, ("to the other layer's line", spans)
assert under.curve_count == 1, "not touched"
wm.set_draw(erase_all_layers=False)
under.selected = False

# --- Trim Overshoots --------------------------------------------------------------------------------------------------
layer = fresh()
draw_line(-0.2, -0.5, -0.2, 0.5)
draw_line(0.2, -0.5, 0.2, 0.5)
wm.set_draw(trim=True)
draw_line(-0.23, 0.0, 0.23, 0.0)
assert layer.curve_count == 3
horizontal = [s for s in extents(layer) if s[1] - s[0] > 0.2]
assert len(horizontal) == 1 and abs(horizontal[0][0] + 0.2) < 0.01 and abs(horizontal[0][1] - 0.2) < 0.01, horizontal
# Past the limit it was meant: it stays.
draw_line(-0.8, 0.1, 0.8, 0.1)
assert [s for s in extents(layer) if s[1] - s[0] > 1.0], extents(layer)
wm.set_draw(trim=False)

ops.wm.tool_draw(mode="FREEHAND")
print("line cut: ok")
