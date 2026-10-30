"""Render as SVG: the current frame through the camera as vectors. Fills are Bezier paths in their colours, lines
filled outlines, masks SVG masks the art refers to, a see-through layer a group with an opacity; the file is
well-formed and sized as the document.

Run with:  OpenAnimate --background --python tests/test_render_svg.py
"""

import pathlib
import tempfile
import xml.etree.ElementTree as ET

import oanim

doc = oanim.data.document
layers = oanim.data.layers
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_render_svg_"))
SVG = "{http://www.w3.org/2000/svg}"


def square(name, x0, x1, hidden=False, fill=True):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(x0, x0), (x1, x0), (x1, x1), (x0, x1)], closed=True, fill=fill)
    l.visible = not hidden
    return l


def render(name, **args):
    path = folder / name
    assert oanim.ops.export.svg(filepath=str(path), **args) == {'FINISHED'}
    assert path.exists(), path
    return ET.parse(path).getroot()


def paths(root):
    return [e for e in root.iter(SVG + "path")]


# Without a filepath and no window there's nothing to ask: it says so.
assert oanim.ops.export.svg() == {'CANCELLED'}

art = square("art", -0.5, 0.5)
root = render("plain.svg")
assert root.tag == SVG + "svg", root.tag
assert (root.get("width"), root.get("height")) == (str(doc.width), str(doc.height)), root.attrib
assert root.get("viewBox") == "0 0 %d %d" % (doc.width, doc.height)
# The background, then the square's fill (Beziers) and its line (an outline, filled).
ps = paths(root)
assert len(ps) == 3, [p.attrib for p in ps]
background, fill, line = ps
assert background.get("d") == "M0 0L%d 0 %d %d 0 %dZ" % (doc.width, doc.width, doc.height, doc.height), background.attrib
assert "C" in fill.get("d") and fill.get("d").endswith("Z"), fill.get("d")[:80]
assert "C" not in line.get("d"), line.get("d")[:80]
# The square is in the middle of the picture: its corners at the frame's +-0.5 (the frame 2 units tall).
# (Every third point is on the curve; the others are handles.)
xs = [float(v) for v in fill.get("d").replace("M", " ").replace("C", " ").replace("Z", " ").split()][0::2][0::3]
assert abs(min(xs) - (doc.width / 2 - doc.height / 4)) < 0.6 and abs(max(xs) - (doc.width / 2 + doc.height / 4)) < 0.6, xs

# A transparent one has no background.
root = render("clear.svg", transparent="1")
assert len(paths(root)) == 2, "no background"

# A .svg is added when the name has none.
assert oanim.ops.export.svg(filepath=str(folder / "named")) == {'FINISHED'}
assert (folder / "named.svg").exists()

# A mask: the art refers to a mask (its layer's card), painted from the hidden square's shape.
square("cut", 0.0, 2.0, hidden=True)
layers.active = art
assert oanim.ops.mask.add(sources="cut", mode="SUBTRACT") == {'FINISHED'}
root = render("masked.svg")
masks = {m.get("id"): m for m in root.iter(SVG + "mask")}
assert masks, "masks written"
masked = [g for g in root.iter(SVG + "g") if g.get("mask")]
assert masked, "the art masked"
mask_id = masked[0].get("mask")[5:-1]
assert mask_id in masks, mask_id
# The hidden mask layer isn't drawn in colour, only in the mask.
drawn = [p for e in root if e.tag != SVG + "defs" for p in e.iter(SVG + "path")]
assert len(drawn) == 3, [p.attrib for p in drawn]

# A see-through layer fades as a whole: its shapes in a group with its opacity.
art.opacity = 0.5
root = render("faded.svg")
faded = [g for g in root.iter(SVG + "g") if g.get("opacity") == "0.5"]
assert faded and len(paths(faded[0])) == 2, [g.attrib for g in root.iter(SVG + "g")]

# A media picture shown in renders goes in as a PNG, placed by a matrix (a guide doesn't).
STILL = str(pathlib.Path(__file__).parent / "data" / "media" / "still.png")
assert oanim.ops.media.add(filepath=STILL) == {'FINISHED'}
still = layers.active
doc.frame_current = 1
root = render("guide.svg")
assert not list(root.iter(SVG + "image")), "a guide isn't rendered"
still.media_guide = False
root = render("picture.svg")
(image,) = list(root.iter(SVG + "image"))
href = image.get("{http://www.w3.org/1999/xlink}href")
assert href.startswith("data:image/png;base64,") and image.get("transform").startswith("matrix("), image.attrib

print("render svg OK")
