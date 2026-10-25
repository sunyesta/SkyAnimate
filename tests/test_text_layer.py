"""Text layers: New Text makes the letters as shapes (an O's middle a hole), changing the text or its runs
makes them again, bold changes the outlines, the text and its letters save and open, a run can share a
material, undo takes a new text layer away, and Convert to Vectors (whole, by letters, by words) leaves
ordinary vector layers.

Run with:  OpenAnimate --background --python tests/test_text_layer.py
"""

import os
import tempfile

import oanim


def shown_at(x, y):
    return [l.name for l in oanim.layers_at((x, y))]


def outline_box(layer, shape):
    outline, _ = layer.shape_outline(shape, 8)
    xs, ys = [p[0] for p in outline], [p[1] for p in outline]
    return min(xs), min(ys), max(xs), max(ys)


assert oanim.ops.layer.new_text(text="O", size="0.5", x="-0.5", y="0.5") == {'FINISHED'}
text = oanim.data.layers.active
assert text.type == "TEXT" and text.name == "O" and text.text_body == "O"
assert text.shape_count == 1
style = text.shape_style(0)
assert style['fill_rule'] == "NONZERO" and style['has_fill'], style
# The O's middle shows through; its ring doesn't. The layer's origin is the first line's top.
x0, y0, x1, y1 = outline_box(text, 0)
assert y1 < 0.0 and y0 > -0.55, (y0, y1)
mid = ((x0 + x1) / 2 + text.location_x, (y0 + y1) / 2 + text.location_y)
assert "O" not in shown_at(*mid), shown_at(*mid)
assert "O" in shown_at(x0 + text.location_x + 0.01, mid[1])

# The text changed: its letters made again (no shapes for spaces).
text.text_body = "Hi there"
assert text.shape_count == 7, text.shape_count
assert [text.shape_style(i)['has_fill'] for i in range(7)] == [True] * 7
narrow = outline_box(text, 0)

# Bold, through its runs: wider letters.
runs = text.text_runs
assert len(runs) == 1 and runs[0]['text'] == "Hi there" and runs[0]['font'] == "Segoe UI"
runs[0]['weight'] = 700
text.text_runs = runs
bold = outline_box(text, 0)
assert bold[2] - bold[0] > narrow[2] - narrow[0], (narrow, bold)

# Styling part of it splits the runs; the same style again joins them.
oanim.data.layers.active = text
assert oanim.ops.text.style_set(start="3", end="8", italic="1") == {'FINISHED'}
assert [r['text'] for r in text.text_runs] == ["Hi ", "there"], text.text_runs
assert oanim.ops.text.style_set(start="3", end="8", italic="0") == {'FINISHED'}
assert [r['text'] for r in text.text_runs] == ["Hi there"]

# A text box wraps; alignment and spacing are layout settings.
text.text_box_width = 0.3
_, wrapped_y0, _, _ = outline_box(text, text.shape_count - 1)
assert wrapped_y0 < bold[1] - 0.05, (wrapped_y0, bold)
text.text_box_width = 0.0
text.text_align = "CENTER"
centred = outline_box(text, 0)
assert centred[0] < -0.05, centred

# A shared material (made from a box's look): the run takes it, and changes with it.
oanim.ops.layer.new()
box = oanim.data.layers.active
box.name = "Box"
assert box.add_primitive("BOX", [(0.6, -0.9), (0.9, -0.6)], True)
box.select_points(range(box.point_count))
assert oanim.ops.material.new(name="Ink") == {'FINISHED'}
runs = text.text_runs
runs[0]['material'] = "Ink"
text.text_runs = runs
assert text.text_runs[0]['material'] == "Ink" and text.shape_style(0)['material'] == "Ink"
box.set_shape_style(0, fill=(1.0, 0.0, 0.0, 1.0))
oanim.data.layers.active = text
oanim.ops.text.style_set(start="0", end="1", fill="1")
assert [round(c, 3) for c in text.shape_style(0)['fill']] == [1.0, 0.0, 0.0, 1.0], text.shape_style(0)
oanim.data.layers.active = text

# Saved and opened: the text and its letters.
path = os.path.join(tempfile.mkdtemp(), 'text.oanim')
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
text = next(l for l in oanim.data.layers.all() if l.type == "TEXT")
assert text.text_body == "Hi there" and text.shape_count == 7 and text.text_align == "CENTER"
assert text.text_runs[0]['weight'] == 700

# Undo takes a new text layer away.
count = len(list(oanim.data.layers.all()))
assert oanim.ops.layer.new_text(text="Undo me") == {'FINISHED'}
assert len(list(oanim.data.layers.all())) == count + 1
oanim.ops.ed.undo()
assert len(list(oanim.data.layers.all())) == count

# Convert to Vectors: an ordinary vector layer of the letters.
oanim.data.layers.active = text
assert oanim.ops.text.convert_to_vectors() == {'FINISHED'}
assert text.type == "VECTOR" and text.shape_count == 7
oanim.ops.ed.undo()
text = next(l for l in oanim.data.layers.all() if l.type == "TEXT")
oanim.data.layers.active = text
assert oanim.ops.text.convert_to_vectors(split="WORDS") == {'FINISHED'}
group = oanim.data.layers.active
assert group.type == "GROUP" and sorted(c.name for c in group.children) == ["Hi", "there"], \
    [c.name for c in group.children]
oanim.ops.ed.undo()
text = next(l for l in oanim.data.layers.all() if l.type == "TEXT")
oanim.data.layers.active = text
assert oanim.ops.text.convert_to_vectors(split="LETTERS") == {'FINISHED'}
assert len(oanim.data.layers.active.children) == 7

# Fonts: the installed ones, and Flash's PostScript names.
assert "Arial" in oanim.text_fonts()
assert oanim.text_font_from_postscript("Arial-BoldMT") == ("Arial", 700, False)
print("text layer: ok")
