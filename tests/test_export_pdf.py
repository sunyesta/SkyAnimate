"""Export PDF: frames through the camera as vector pages, a page a frame, every layer a PDF layer (optional content)
named and nested as in the Layers editor; hidden layers in the file too, switched off. The file's cross-reference
table finds every object; the document stays at its own frame.

Run with:  OpenAnimate --background --python tests/test_export_pdf.py
"""

import pathlib
import re
import tempfile
import zlib

import oanim

doc = oanim.data.document
layers = oanim.data.layers
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_export_pdf_"))


def square(name, x0, x1, hidden=False):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(x0, x0), (x1, x0), (x1, x1), (x0, x1)], closed=True, fill=True)
    l.visible = not hidden
    return l


def export(name, **args):
    path = folder / name
    assert oanim.ops.export.pdf(filepath=str(path), **args) == {'FINISHED'}
    data = path.read_bytes()
    assert data.startswith(b"%PDF-1.7") and data.rstrip().endswith(b"%%EOF"), data[:20]
    # Every object where the cross-reference table says.
    start = int(re.search(rb"startxref\s+(\d+)", data).group(1))
    table = data[start:].split(b"trailer")[0].split(b"\n")
    count = int(table[1].split()[1])
    for n in range(1, count):
        offset = int(table[2 + n][:10])
        assert data[offset:].startswith(b"%d 0 obj" % n), (n, data[offset:offset + 20])
    # The streams, unpacked.
    streams = [zlib.decompress(m.group(1)) for m in re.finditer(rb">>\nstream\n(.*?)\nendstream", data, re.S)]
    return data, streams


def utf16(name):
    return ("<FEFF" + "".join("%04X" % ord(c) for c in name) + ">").encode()


def pages(data):
    return len(re.findall(rb"/Type /Page /Parent", data))


# Without a filepath and no window there's nothing to ask: it says so.
assert oanim.ops.export.pdf() == {'CANCELLED'}

art = square("art", -0.5, 0.5)
cut = square("cut ✂", 0.0, 2.0, hidden=True)
layers.active = art
assert oanim.ops.mask.add(sources=cut.name, mode="SUBTRACT") == {'FINISHED'}

data, streams = export("frame.pdf")
assert pages(data) == 1
# A PDF layer for each layer, named as it is (any script), the hidden one switched off.
ocgs = dict(re.findall(rb"(\d+) 0 obj\n<< /Type /OCG /Name (<[0-9A-F]+>) >>", data))
names = {v: k for k, v in ocgs.items()}
assert utf16("art") in names and utf16("cut ✂") in names, ocgs
off = re.search(rb"/OFF \[([^\]]*)\]", data).group(1)
assert (names[utf16("cut ✂")] + b" 0 R") in off and (names[utf16("art")] + b" 0 R") not in off, off
assert b"/PageMode /UseOC" in data
# Content marked by layer; the art masked by a luminosity soft mask; fills and paths.
content = b"".join(streams)
assert content.count(b"BDC") >= 2 and content.count(b"BDC") == content.count(b"EMC"), content.count(b"BDC")
assert b"/S /Luminosity" in data and b"/SMask " in data
assert b" c\n" in content and b"\nf\n" in content
# Drawn in pixels, y down, on a page a pixel three quarters of a point.
assert b"/MediaBox [0 0 %d %d]" % (doc.width * 3 // 4, doc.height * 3 // 4) in data

# A page a frame over the frame range; the document stays where it was.
for f, x in ((1, 0.0), (3, 0.5)):
    doc.frame_current = f
    art.location_x = x
    art.keyframe_insert("position")
doc.frame_start, doc.frame_end = 1, 3
doc.frame_current = 2
between = art.location_x
data, streams = export("range.pdf", frames="RANGE")
assert pages(data) == 3, pages(data)
assert doc.frame_current == 2
data, _ = export("some.pdf", frames="RANGE", frame_start="2", frame_end="3")
assert pages(data) == 2, pages(data)
# Each page is its own frame: the art moved between them.
_, first = export("first.pdf", frames="RANGE", frame_start="1", frame_end="1")
_, last = export("last.pdf", frames="RANGE", frame_start="3", frame_end="3")
assert b"".join(first) != b"".join(last), "the frames differ"
assert doc.frame_current == 2 and art.location_x == between, (doc.frame_current, art.location_x, between)

# A .pdf is added when the name has none.
assert oanim.ops.export.pdf(filepath=str(folder / "named")) == {'FINISHED'}
assert (folder / "named.pdf").exists()

print("export pdf OK")
