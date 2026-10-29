"""Opening an XFL .fla (Flash CS5 on, Animate): a zip of XML. A small one is built here: a stage, a layer whose
keyframe has a red square (edges with a hex coordinate) and an instance (scaled 2x) of a library symbol holding
a blue square, a hidden layer (shown: Flash publishes it), a guide and a hidden guide. Opened as a zip, as a zip whose central directory is wrong (as Animate leaves
some), and unzipped as an .xfl project, it makes the same document each time.

Run with:  OpenAnimate --background --python tests/test_xfl_import.py
"""

import os
import pathlib
import struct
import tempfile
import zipfile

import oanim

W, H, FPS = 640, 480, 12.0
NS = 'xmlns="http://ns.adobe.com/xfl/2008/"'


def square(x, y, size, color, twips_hex=False):
    """A filled square as an XFL shape: edges in twips, the first corner written in hex if asked."""
    t = lambda v: v * 20
    x0 = ('#%X.00' % t(x)) if twips_hex else str(t(x))
    pts = [(t(x), t(y)), (t(x + size), t(y)), (t(x + size), t(y + size)), (t(x), t(y + size))]
    edges = '!%s %d' % (x0, pts[0][1])
    for px, py in pts[1:] + pts[:1]:
        edges += '|%d %d' % (px, py)
    return ('<DOMShape><fills><FillStyle index="1"><SolidColor color="%s"/></FillStyle></fills>'
            '<edges><Edge fillStyle1="1" edges="%s"/></edges></DOMShape>' % (color, edges))


document = ('<DOMDocument %s width="%d" height="%d" frameRate="%g" backgroundColor="#C0C0C0">'
            '<symbols><Include href="Blue Box.xml"/></symbols>'
            '<timelines><DOMTimeline name="Scene 1"><layers>'
            '<DOMLayer name="Notes" layerType="guide" visible="false"><frames><DOMFrame index="0"><elements>%s'
            '</elements></DOMFrame></frames></DOMLayer>'
            '<DOMLayer name="Guide" layerType="guide"><frames><DOMFrame index="0"><elements>%s</elements>'
            '</DOMFrame></frames></DOMLayer>'
            '<DOMLayer name="Hidden" visible="false"><frames><DOMFrame index="0"><elements>%s</elements></DOMFrame>'
            '</frames></DOMLayer>'
            '<DOMLayer name="Art"><frames><DOMFrame index="0"><elements>%s'
            '<DOMSymbolInstance libraryItemName="Blue Box" symbolType="graphic" loop="single frame">'
            '<matrix><Matrix a="2" d="2" tx="400" ty="100"/></matrix></DOMSymbolInstance>'
            '<DOMSymbolInstance libraryItemName="Blue Box" name="Skewed" symbolType="graphic" loop="single frame">'
            '<matrix><Matrix c="0.5" tx="300" ty="300"/></matrix></DOMSymbolInstance>'
            '<DOMStaticText left="0" width="100" height="50" autoExpand="true">'
            '<matrix><Matrix tx="50" ty="300"/></matrix><textRuns><DOMTextRun><characters>Hi&#xD;there</characters>'
            '<textAttrs><DOMTextAttrs size="20" face="Arial-BoldMT" fillColor="#0000FF"/></textAttrs>'
            '</DOMTextRun></textRuns></DOMStaticText>'
            '</elements></DOMFrame></frames></DOMLayer>'
            '</layers></DOMTimeline></timelines></DOMDocument>'
            % (NS, W, H, FPS, square(0, 0, 20, '#00FF00'), square(0, 0, 20, '#00FF00'), square(0, 0, 20, '#00FF00'),
               square(100, 100, 50, '#FF0000', twips_hex=True)))
symbol = ('<DOMSymbolItem %s name="Blue Box" symbolType="graphic"><timeline><DOMTimeline name="Blue Box"><layers>'
          '<DOMLayer name="Box Guide" layerType="guide"><frames><DOMFrame index="0"><elements>%s</elements>'
          '</DOMFrame></frames></DOMLayer>'
          '<DOMLayer name="Layer 1"><frames><DOMFrame index="0"><elements>%s</elements></DOMFrame></frames>'
          '</DOMLayer></layers></DOMTimeline></timeline></DOMSymbolItem>'
          % (NS, square(0, 0, 10, '#FFFFFF'), square(0, 0, 10, '#0000FF')))
files = {'DOMDocument.xml': document, 'LIBRARY/Blue Box.xml': symbol, 'mimetype': 'application/vnd.adobe.xfl'}

folder = pathlib.Path(tempfile.mkdtemp())
good = folder / 'boxes.fla'
with zipfile.ZipFile(good, 'w', zipfile.ZIP_DEFLATED) as z:
    for name, text in files.items():
        z.writestr(name, text)
# The same with its central directory said to start past where it is.
data = bytearray(good.read_bytes())
eocd = data.rfind(b'PK\x05\x06')
offset = struct.unpack_from('<I', data, eocd + 16)[0]
struct.pack_into('<I', data, eocd + 16, offset + 54)
broken = folder / 'broken.fla'
broken.write_bytes(bytes(data))
project = folder / 'project' / 'boxes'
for name, text in files.items():
    (project / name).parent.mkdir(parents=True, exist_ok=True)
    (project / name).write_text(text, encoding='utf-8')
(project / 'boxes.xfl').write_text('PROXY-CS5', encoding='utf-8')

k = 2.0 / H


def to_doc(x, y):
    return ((x - W / 2) * k, -(y - H / 2) * k)


def check(path, name):
    assert oanim.ops.wm.open_mainfile(filepath=str(path), confirmed="1") == {'FINISHED'}, path
    doc = oanim.data.document
    assert (doc.width, doc.height) == (W, H) and abs(doc.fps - FPS) < 1e-3, (doc.width, doc.height, doc.fps)
    assert doc.name == name, doc.name
    layers = oanim.data.layers
    art = next(l for l in layers.all() if l.name == 'Art')
    # Flash publishes hidden layers and leaves guides out; a guide hidden too stays hidden.
    hidden = next(l for l in layers.all() if l.name == 'Hidden')
    assert hidden.visible and hidden.render_visible
    guide = next(l for l in layers.all() if l.name == 'Guide')
    assert guide.visible and not guide.render_visible
    notes = next(l for l in layers.all() if l.name == 'Notes')
    assert not notes.visible and not notes.render_visible
    # A symbol's guide shows on Flash's stage where the symbol is placed too (some sheets draw a part only on
    # one), and is left out when published.
    box_guides = [l for l in layers.all() if l.name == 'Box Guide']
    assert box_guides and all(l.visible and not l.render_visible for l in box_guides), box_guides
    square_layer = next(l for l in layers.all() if l.type == 'VECTOR' and l.parent and l.parent.id == art.id)
    outline, closed = square_layer.shape_outline(0, 1)
    assert closed
    xs, ys = [p[0] for p in outline], [p[1] for p in outline]
    (x0, y1), (x1, y0) = to_doc(100, 100), to_doc(150, 150)
    assert abs(min(xs) - x0) < 1e-4 and abs(max(xs) - x1) < 1e-4 and abs(min(ys) - y0) < 1e-4 and abs(max(ys) - y1) < 1e-4, outline
    style = square_layer.shape_style(0)
    assert [round(c, 3) for c in style['fill'][:3]] == [1.0, 0.0, 0.0] and style['has_fill'], style
    box = next(l for l in layers.all() if l.name == 'Blue Box')
    px, py = to_doc(400, 100)
    assert abs(box.location_x - px) < 1e-4 and abs(box.location_y - py) < 1e-4, (box.location_x, box.location_y)
    assert abs(box.scale_x - 2.0) < 1e-4 and abs(box.scale_y - 2.0) < 1e-4
    # A skewed instance: the skew is its layer's shear (none pushed into the symbol's drawing), and the
    # square's corners land where Flash's matrix puts them.
    skewed = next(l for l in layers.all() if l.name == 'Skewed')
    assert abs(skewed.shear_x) < 1e-4 and abs(skewed.shear_y) > 1.0, (skewed.shear_x, skewed.shear_y)
    inner = next(l for l in layers.all() if l.type == 'VECTOR' and l.parent and l.parent.id == skewed.id)
    outline, _ = inner.shape_outline(0, 1)
    shown = [inner.to_document(p) for p in outline]
    for fx, fy in [(0, 0), (10, 0), (10, 10), (0, 10)]:
        ex, ey = to_doc(300 + fx + 0.5 * fy, 300 + fy)
        assert any(abs(x - ex) < 1e-4 and abs(y - ey) < 1e-4 for x, y in shown), ((fx, fy), shown)
    # The text: a text layer in its font (found by its PostScript name), size and colour, its first line's
    # top-left inside Flash's 2-pixel gutter.
    text = next(l for l in layers.all() if l.type == 'TEXT')
    assert text.name == 'Hi' and text.text_body == 'Hi\nthere', (text.name, text.text_body)
    run = text.text_runs[0]
    assert (run['font'], run['weight']) == ('Arial', 700), run
    assert abs(run['size'] - 20 * k) < 1e-5 and [round(c, 3) for c in run['fill']] == [0.0, 0.0, 1.0, 1.0], run
    tx, ty = to_doc(52, 302)
    assert abs(text.location_x - tx) < 1e-4 and abs(text.location_y - ty) < 1e-4, (text.location_x, text.location_y)
    assert text.shape_count == 7, text.shape_count


check(good, 'boxes')
check(broken, 'broken')
check(project / 'boxes.xfl', 'boxes')
print("xfl import: ok")
