"""Opening a Flash .fla (the binary format of Flash 5 to CS6): a small one is built here, a compound file with
a Contents stream (stage, background, frame rate, library) and a scene whose layer's keyframe has a red square
and an instance (scaled 2x) of a symbol holding a blue square. Opening it makes an untitled document of the
same size and rate, a layer for each Flash layer, fills rebuilt from Flash's edges and the instance a group.

Run with:  OpenAnimate --background --python tests/test_fla_import.py
"""

import ctypes
import math
import pathlib
import struct
import subprocess
import tempfile

import oanim

W, H, FPS = 640, 480, 12.0
INT_MIN = -2 ** 31


def u8(v): return struct.pack('<B', v)
def u16(v): return struct.pack('<H', v)
def u32(v): return struct.pack('<I', v)
def s32(v): return struct.pack('<i', v)


def cstring(text):
    data = text.encode('utf-16le')
    return b'\xff\xfe\xff' + u8(len(text)) + data


def new_class(name):
    return b'\xff\xff' + u16(1) + u16(len(name)) + name.encode('ascii')


def base(children=b''):
    """CPicObj: schema 4, flags, children, null tag, an unset point and two flags."""
    return u8(4) + u8(0) + children + u16(0) + s32(INT_MIN) + s32(INT_MIN) + u8(0) + u8(0)


def matrix(a=1.0, d=1.0, tx=0.0, ty=0.0):
    return s32(round(a * 65536)) + s32(0) + s32(0) + s32(round(d * 65536)) + s32(round(tx * 20)) + s32(round(ty * 20))


def square(x, y, size, rgba):
    """Shape data: one fill, a square drawn with straight edges (edge units 1/5120 px)."""
    k = 5120
    out = u8(5) + u32(4) + u16(1) + bytes(rgba) + u8(0) + u8(0) + u16(0)
    # First edge: a move to the corner, a style change (line 0, fills 0 and 1), then along the top.
    out += u8(0x40 | 2 | (2 << 4)) + u16(0) + u16(0) + u16(1) + s32(x * k) + s32(y * k) + s32(size * k) + s32(0)
    for dx, dy in ((0, size), (-size, 0), (0, -size)):
        out += u8(2 << 4) + s32(dx * k) + s32(dy * k)
    return out + u8(0) + s32(0)


def frame(children, shape):
    tail = (u8(24) + u16(1) + u16(0x600) + u16(0) + u16(0) + u16(0) + b'\x00' * 11 + u16(0xFFFF) + cstring('') +
            u32(4) + u32(1) + u32(0) + u32(0) + cstring('') + b'\x00' * 8 + u32(0) + u16(0) + u32(0) + u16(0) +
            cstring('') + u32(1) + u32(0) + u32(0) + u32(1) + u32(0))
    return base(children) + u8(3) + matrix() + shape + tail


def layer(name, frames):
    return (base(frames) + u8(11) + cstring(name) + u8(0) + u8(0) + u8(0) + u32(0xFFFFFFFF) + b'\x00' * 8 + u32(1) +
            u8(0) + u16(0) + u8(0) + u8(0))


def page(layers):
    return base(layers) + u8(7) + u16(0) + u16(0) + u32(0) + u32(0)


def symbol_instance(number, a, tx, ty):
    return (base() + u8(19) + matrix(a, a, tx, ty) + u16(0) + u16(2) + u8(1) + (u16(256) + u16(0)) * 4 + u32(0) + u32(0) +
            cstring('') + u32(number) + b'\x00\x00\x00\x00\x01\x00\x00')


def timeline(layer_name, frame_children, shape):
    """A Page or Symbol stream: the classes declared on first use, later ones by reference."""
    out = u8(1) + new_class('CPicPage')
    body_layer = new_class('CPicFrame') + frame(frame_children, shape)
    return out + page(new_class('CPicLayer') + layer(layer_name, body_layer))


def compound_file(streams):
    """A compound file ([MS-CFB]) with 512-byte sectors: FAT in sector 0, directory in 1, then the streams
    (each padded to at least 4096 bytes so none goes in the mini stream)."""
    sectors, chains = [], {}
    for name, data in streams.items():
        data = data + b'\x00' * max(0, 4096 - len(data))
        start = 2 + len(sectors)
        n = (len(data) + 511) // 512
        chains[name] = (start, len(data), n)
        for i in range(n):
            sectors.append(data[i * 512:(i + 1) * 512].ljust(512, b'\x00'))
    fat = [0xFFFFFFFD, 0xFFFFFFFE]
    for name, (start, size, n) in chains.items():
        fat += [start + i + 1 for i in range(n - 1)] + [0xFFFFFFFE]
    fat += [0xFFFFFFFF] * (128 - len(fat))

    def entry(name, kind, start, size):
        raw = name.encode('utf-16le') + b'\x00\x00'
        return (raw.ljust(64, b'\x00') + u16(len(raw)) + u8(kind) + u8(1) + u32(0xFFFFFFFF) * 3 + b'\x00' * 36 +
                u32(start) + struct.pack('<Q', size))

    directory = entry('Root Entry', 5, 0xFFFFFFFE, 0)
    for name, (start, size, n) in chains.items():
        directory += entry(name, 2, start, size)
    directory = directory.ljust(512, b'\x00')
    header = (bytes.fromhex('D0CF11E0A1B11AE1') + b'\x00' * 16 + u16(0x3E) + u16(3) + u16(0xFFFE) + u16(9) + u16(6) +
              b'\x00' * 10 + u32(1) + u32(1) + u32(0) + u32(4096) + u32(0xFFFFFFFE) + u32(0) + u32(0xFFFFFFFE) + u32(0) +
              u32(0) + u32(0xFFFFFFFF) * 108)
    return header + b''.join(struct.pack('<I', v) for v in fat) + directory + b''.join(sectors)


contents = (b'\x00' * 32 + u8(8) + 'Symbol 1'.encode('utf-16le') + cstring('Blue Box') + b'\x00' * 16 +
            b'\x14\x00\x05\x00\x00' + s32(0) + s32(W * 20) + s32(0) + s32(H * 20) + b'\x00' * 16 +
            bytes((255, 255, 255, 255)) + bytes((0xC0, 0xC0, 0xC0, 0xFF)) + u8(0) + u16(round(FPS * 256)) + b'\x00' * 16)
scene = timeline('Art', new_class('CPicSymbol') + symbol_instance(1, 2.0, 400, 100), square(100, 100, 50, (255, 0, 0, 255)))
blue = timeline('Layer 1', b'', square(0, 0, 10, (0, 0, 255, 255)))
folder = pathlib.Path(tempfile.mkdtemp())
path = folder / 'boxes.fla'
path.write_bytes(compound_file({'Contents': contents, 'Page 1': scene, 'Symbol 1': blue}))

assert oanim.ops.wm.open_mainfile(filepath=str(path), confirmed="1") == {'FINISHED'}
doc = oanim.data.document
assert (doc.width, doc.height) == (W, H) and abs(doc.fps - FPS) < 1e-3, (doc.width, doc.height, doc.fps)
assert doc.name == 'boxes'
k = 2.0 / H


def to_doc(x, y):
    return ((x - W / 2) * k, -(y - H / 2) * k)


layers = oanim.data.layers
art = next(l for l in layers.all() if l.name == 'Art')
assert art.type == 'GROUP', art.type
square_layer = next(l for l in layers.all() if l.type == 'VECTOR' and l.parent and l.parent.id == art.id)
shapes = square_layer.shapes
assert len(shapes) == 1, shapes
outline, closed = square_layer.shape_outline(0, 1)
assert closed
xs, ys = [p[0] for p in outline], [p[1] for p in outline]
(x0, y1), (x1, y0) = to_doc(100, 100), to_doc(150, 150)
assert abs(min(xs) - x0) < 1e-4 and abs(max(xs) - x1) < 1e-4 and abs(min(ys) - y0) < 1e-4 and abs(max(ys) - y1) < 1e-4, outline
style = square_layer.shape_style(0)
assert [round(c, 3) for c in style['fill'][:3]] == [1.0, 0.0, 0.0] and style['has_fill'] and not style['has_outline'], style

# The instance: a group named after its library symbol, at its place, scaled 2x.
box = next(l for l in layers.all() if l.name == 'Blue Box')
assert box.type == 'GROUP'
px, py = to_doc(400, 100)
assert abs(box.location_x - px) < 1e-4 and abs(box.location_y - py) < 1e-4, (box.location_x, box.location_y)
assert abs(box.scale_x - 2.0) < 1e-4 and abs(box.scale_y - 2.0) < 1e-4 and abs(box.rotation) < 1e-3

# The byte before the stage's rect differs from file to file: the stage is read whatever it is.
from oanim import io_fla
stage_13 = contents.replace(b'\x14\x00\x05\x00\x00' + s32(0), b'\x13\x00\x05\x00\x00' + s32(0), 1)
assert stage_13 != contents
info = io_fla.read_contents(stage_13)
assert (info['width'], info['height']) == (W, H), info

# Given on the command line, it opens too: the importer is Python, which starts before the file opens.
exe = ctypes.create_unicode_buffer(32768)
ctypes.windll.kernel32.GetModuleFileNameW(None, exe, len(exe))
p = subprocess.run([exe.value, '--background', str(path), '--python-expr',
                    "import oanim; assert oanim.data.document.name == 'boxes', oanim.data.document.name"],
                   capture_output=True, timeout=120)
assert p.returncode == 0, (p.returncode, p.stdout, p.stderr)

print("fla import: ok")
