"""Opening Flash's .fla files: the binary format of Flash 5 to CS6 (not CS5+'s zipped XFL).

A .fla is a compound file (Microsoft's OLE2): a Contents stream with the document's settings and library,
a Page stream per scene, a Symbol stream per library item and Media streams with bitmaps. Pages and symbols
are MFC archives of CPic* objects: a page holds layers, a layer its keyframes, a keyframe its drawing and
the objects on it (drawing groups, symbol instances, bitmaps, text).

The format notes are fla-decoder's (github.com/eddiemoore/fla-decoder, MIT licence), reverse-engineered
from Flash 8, and fla-viewer's (github.com/lifeart/fla-viewer, ISC licence: layers stored bottom first, edge
units, the layer record's current/locked/hidden), with what CS3/CS4 files add worked out here: object
back-references, gradient extras, motion tween ease curves, shape tween records, mask layers and the graphic
symbol instance's fields (First Frame, loop, colour transform).

convert(fla, oanim) writes an .oanim document: each Flash layer a layer (folders groups, guides guides,
masks Open Animate masks), drawings as vector layers whose fills are rebuilt from Flash's edges, symbol
instances as groups with the instance's transform, bitmaps as Media layers packed into the file. A symbol
instance shows the frame it is set to (graphic symbols' First Frame); the main timeline's keyframes switch
by visibility and its motion tweens become transform keys. What has no equivalent is listed in the
returned report (gradients come in as solid fills of their average colour, for instance).
"""

import hashlib
import json
import math
import os
import re
import struct
import zipfile
import zlib

# --- The compound file ([MS-CFB]) -----------------------------------------------------------------

_FREE, _END = 0xFFFFFFFF, 0xFFFFFFFE


class CompoundFile:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = d = f.read()
        if d[:8] != bytes.fromhex('D0CF11E0A1B11AE1'):
            raise ValueError('not a binary .fla (a compound file); a CS5+ XFL .fla is a zip')
        self.ssize = 1 << struct.unpack_from('<H', d, 0x1E)[0]
        self.msize = 1 << struct.unpack_from('<H', d, 0x20)[0]
        n_fat = struct.unpack_from('<I', d, 0x2C)[0]
        dir_start = struct.unpack_from('<I', d, 0x30)[0]
        self.mini_cutoff = struct.unpack_from('<I', d, 0x38)[0]
        minifat_start = struct.unpack_from('<I', d, 0x3C)[0]
        n_minifat = struct.unpack_from('<I', d, 0x40)[0]
        difat_sector = struct.unpack_from('<I', d, 0x44)[0]
        n_difat = struct.unpack_from('<I', d, 0x48)[0]
        difat = list(struct.unpack_from('<109I', d, 0x4C))
        per = self.ssize // 4
        for _ in range(n_difat):
            vals = struct.unpack_from('<%dI' % per, d, self._off(difat_sector))
            difat.extend(vals[:-1])
            difat_sector = vals[-1]
        self.fat = []
        for s in [x for x in difat if x not in (_FREE, _END)][:n_fat]:
            self.fat.extend(struct.unpack_from('<%dI' % per, d, self._off(s)))
        mf = self._chain(minifat_start) if n_minifat else b''
        self.minifat = list(struct.unpack('<%dI' % (len(mf) // 4), mf))
        entries = self._chain(dir_start)
        self.streams = {}
        root = None
        for i in range(len(entries) // 128):
            e = entries[i * 128:(i + 1) * 128]
            nlen = struct.unpack_from('<H', e, 64)[0]
            name = e[:max(0, nlen - 2)].decode('utf-16le', 'replace')
            start, size = struct.unpack_from('<I', e, 116)[0], struct.unpack_from('<Q', e, 120)[0]
            if e[66] == 5:
                root = (start, size)
            elif e[66] == 2:
                self.streams[name] = (start, size)
        self.ministream = self._chain(root[0])[:root[1]] if root and root[0] != _END else b''

    def _off(self, sector):
        return (sector + 1) * self.ssize

    def _chain(self, start):
        out = bytearray()
        s, guard = start, 0
        while s not in (_END, _FREE) and s < len(self.fat) and guard <= len(self.fat):
            off = self._off(s)
            out += self.data[off:off + self.ssize]
            s = self.fat[s]
            guard += 1
        return bytes(out)

    def read(self, name):
        start, size = self.streams[name]
        if size < self.mini_cutoff:
            out = bytearray()
            s = start
            while s not in (_END, _FREE) and len(out) < size and s < len(self.minifat):
                out += self.ministream[s * self.msize:(s + 1) * self.msize]
                s = self.minifat[s]
            return bytes(out[:size])
        return self._chain(start)[:size]


# --- The MFC archive -------------------------------------------------------------------------------

class Fail(Exception):
    pass


INT_MIN = -2 ** 31


class Archive:
    """CArchive's tagged objects: 0 null, FFFF a new class (schema, name), 8000|i a new object of class
    entry i, a plain i (or 7FFF + u32) an object read already. Classes and objects share one table."""

    def __init__(self, data):
        self.b, self.p = data, 0
        self.table = []

    def need(self, n):
        if self.p + n > len(self.b):
            raise Fail('stream ends at %x' % self.p)

    def u8(self):
        self.need(1)
        v = self.b[self.p]
        self.p += 1
        return v

    def u16(self):
        self.need(2)
        v = struct.unpack_from('<H', self.b, self.p)[0]
        self.p += 2
        return v

    def s16(self):
        self.need(2)
        v = struct.unpack_from('<h', self.b, self.p)[0]
        self.p += 2
        return v

    def u32(self):
        self.need(4)
        v = struct.unpack_from('<I', self.b, self.p)[0]
        self.p += 4
        return v

    def s32(self):
        self.need(4)
        v = struct.unpack_from('<i', self.b, self.p)[0]
        self.p += 4
        return v

    def raw(self, n):
        self.need(n)
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def matrix(self):
        """a b c d in 16.16, tx ty in twips: as (a, b, c, d, tx, ty) in pixels."""
        a, b, c, d, tx, ty = struct.unpack('<iiiiii', self.raw(24))
        return (a / 65536, b / 65536, c / 65536, d / 65536, tx / 20, ty / 20)

    def cstring(self):
        n = self.u8()
        if n < 0xFF:
            return self.raw(n).decode('latin1')
        w = self.u16()
        if w == 0xFFFE:
            n = self.u8()
            if n == 0xFF:
                n = self.u16()
            return self.raw(n * 2).decode('utf-16le', 'replace')
        if w == 0xFFFF:
            return self.raw(self.u32()).decode('latin1')
        return self.raw(w).decode('latin1')

    def tag(self):
        """('new', class name, table index) | ('ref', object) | None."""
        at = self.p
        t = self.u16()
        if t == 0:
            return None
        if t == 0xFFFF:
            self.u16()
            name = self.raw(self.u16()).decode('ascii', 'replace')
            self.table.append(('class', name))
            self.table.append(None)
            return ('new', name, len(self.table) - 1)
        if t == 0x7FFF:
            v = self.u32()
            if not v & 0x80000000:
                return self._ref(v, at)
            idx = v & 0x7FFFFFFF
        elif t & 0x8000:
            idx = t & 0x7FFF
        else:
            return self._ref(t, at)
        if not 0 < idx <= len(self.table) or not isinstance(self.table[idx - 1], tuple):
            raise Fail('bad class reference %x at %x' % (idx, at))
        self.table.append(None)
        return ('new', self.table[idx - 1][1], len(self.table) - 1)

    def _ref(self, idx, at):
        if not 0 < idx <= len(self.table):
            raise Fail('bad object reference %x at %x' % (idx, at))
        return ('ref', self.table[idx - 1])


# --- Flash's objects ------------------------------------------------------------------------------

class Obj:
    def __init__(self, kind):
        self.kind = kind
        self.children = []


def _read_object(ar, tag, notes):
    _, name, slot = tag
    reader = _READERS.get(name)
    if not reader:
        raise Fail('%s is not known (at %x)' % (name, ar.p))
    o = Obj(name)
    ar.table[slot] = o
    reader(ar, o, notes)
    return o


def _base(ar, o, notes):
    """CPicObj: schema, flags, the child objects, then (by schema) its transformation point (the pivot,
    in twips, where it is in its parent; both INT_MIN when unset) and two flags."""
    schema = ar.u8()
    ar.u8()
    while True:
        t = ar.tag()
        if t is None:
            break
        if t[0] == 'ref':
            continue
        o.children.append(_read_object(ar, t, notes))
    if schema > 0:
        x, y = ar.s32(), ar.s32()
        if x != -0x80000000 and y != -0x80000000:
            o.point = (x / 20, y / 20)
    if schema > 2:
        ar.u8()
    if schema > 3:
        ar.u8()


def _fill(ar, caps):
    color = ar.u32()
    sub = ar.u8()
    ar.u8()
    f = {'color': color, 'kind': 'solid'}
    if sub & 0x10:
        f['kind'] = 'gradient'
        ar.raw(24)
        n = ar.u8()
        if caps:
            ar.raw(8)  # spread, interpolation, focal point (CS3+: 8 bytes; Flash 8: 3)
        f['stops'] = [(ar.u8(), ar.u32()) for _ in range(n)]
    elif sub & 0x20:
        f['kind'] = 'bitmap'
        ar.raw(24 + 4 + 8)
    elif sub & 0x40:
        f['kind'] = 'bitmap'
        ar.raw(24 + 4)
    return f


def _delta(ar, kind):
    if kind == 0:
        return (0, 0)
    if kind == 1:
        return (ar.s16(), ar.s16())
    if kind == 2:
        return (ar.s32(), ar.s32())
    return (ar.s16() << 7, ar.s16() << 7)


def _shape_data(ar, caps):
    """The geometry: fill and line styles, then edges, each a quadratic Bezier (or straight) with the fill
    on its left and right and its line style (1-based; 0 none). Coordinates in 1/2560 px."""
    schema = ar.u8()
    ar.u32()
    fills = []
    for _ in range(ar.u16()):
        if schema < 3:
            fills.append({'color': ar.u32(), 'kind': 'solid'})
            ar.u16()
        else:
            fills.append(_fill(ar, caps))
    lines = []
    for _ in range(ar.u16()):
        color = ar.u32()
        width = ar.u16()
        ar.raw(4)
        if caps:
            ar.raw(6)
            _fill(ar, caps)
        lines.append({'color': color, 'width': width})
    edges = []
    if schema >= 2:
        x = y = 0
        f0 = f1 = ln = 0
        while True:
            flags = ar.u8()
            if flags == 0:
                break
            # A style change: the line style, then the fills either side.
            if flags & 0x40:
                if flags & 0x80:
                    ln, f0, f1 = ar.u8(), ar.u8(), ar.u8()
                else:
                    ln, f0, f1 = ar.u16() & 0x7FFF, ar.u16() & 0x7FFF, ar.u16() & 0x7FFF
            d1 = _delta(ar, flags & 3)
            d2 = _delta(ar, (flags >> 2) & 3)
            d3 = _delta(ar, (flags >> 4) & 3)
            fx, fy = x + d1[0], y + d1[1]
            to = (fx + d3[0], fy + d3[1])
            ctrl = None if (flags >> 2) & 3 == 0 else (fx + d2[0], fy + d2[1])
            edges.append(((fx, fy), ctrl, to, f0, f1, ln))
            x, y = to
    if schema > 4:
        n = ar.s32()
        ar.raw(32 * n)
    # Edge units: 1/5120 px for shapes of schema 3 on (fla-viewer's finding), 1/2560 before.
    return {'fills': fills, 'lines': lines, 'edges': edges, 'units': 5120.0 if caps else 2560.0}


def _shape(ar, o, notes):
    _base(ar, o, notes)
    schema = ar.u8()
    o.matrix = ar.matrix()
    o.data = _shape_data(ar, schema > 2)


def _frame(ar, o, notes):
    """A keyframe: its drawing (CPicFrame is a CPicShape), then its duration, tween and the rest."""
    _shape(ar, o, notes)
    fs = ar.u8()
    o.duration = max(1, ar.u16())
    o.flags = ar.u16() if fs > 2 else ar.u8()
    o.tween = o.flags & 0xFF  # 0 none, 1 motion, 2 shape
    o.ease = ar.s16() if fs > 1 else 0
    o.label = ''
    o.custom_ease = None
    if fs > 4:
        ar.u16()  # sound
    if fs > 5:
        ar.raw(8 * ar.u16())
    if fs > 6:
        ar.raw(11)
    if fs > 7:
        ar.u16()
    if fs > 8:
        if fs >= 23:
            o.label = ar.cstring()
        if fs >= 19:
            kind, fmt = ar.u32(), ar.u32()
            if kind >= 1:
                ar.u32()
                ar.raw(4 * ar.u32())
            if fmt == 1 and kind >= 4:
                o.script = ar.cstring()
            elif fmt == 0:
                ar.u32()
                ar.raw(4 * ar.u32())
            else:
                raise Fail('frame timeline data %d/%d at %x' % (kind, fmt, ar.p))
        else:
            raise Fail('frame schema %d is not known (at %x)' % (fs, ar.p))
    if fs > 10:
        ar.raw(8)
    if fs > 11:
        ar.u32()
    if fs > 12:
        t = ar.tag()
        if t and t[0] == 'new':
            _morph(ar)
    if fs > 13:
        ar.u32()
    if fs > 14:
        t = ar.tag()
        if t and t[0] == 'new':
            _hint_list(ar)
    if fs > 15 and fs >= 23:
        ar.cstring()
    if fs > 19:
        ar.u32()
    if fs > 20:
        ar.u32()
    if fs >= 22:
        ar.u32()
    if fs >= 24:
        ar.u32()
        if ar.u32():
            # Custom ease: six curves (all properties, then position, rotation, scale, colour, filters),
            # each a point count and per point (x, y) of the handle before, the anchor, the handle after.
            curves = []
            for _ in range(6):
                n = ar.u32()
                if n > 1000:
                    raise Fail('ease curve of %d points at %x' % (n, ar.p))
                curves.append([struct.unpack('<6d', ar.raw(48)) for _ in range(n)])
            o.custom_ease = curves[0]


def _morph(ar):
    """CPicMorphShape (a shape tween): schema, two matrices, 8 bytes, its segments (each 16 bytes, two
    points and its curves), then its fills and lines."""
    ar.u8()
    ar.raw(48 + 8)
    for _ in range(ar.u16()):
        t = ar.tag()
        if not t or t[1] != 'CMorphSegment':
            raise Fail('shape tween segment expected at %x' % ar.p)
        ar.raw(32)
        for _ in range(ar.u16()):
            t = ar.tag()
            if not t or t[1] != 'CMorphCurve':
                raise Fail('shape tween curve expected at %x' % ar.p)
            ar.raw(36)
    ar.u16()
    ar.raw(6 * ar.u16())
    ar.raw(10 * ar.u16())


def _hint_list(ar):
    """A CObList of shape hints (two points and a u32 each)."""
    n = ar.u16()
    if n == 0xFFFF:
        n = ar.u32()
    for _ in range(n):
        t = ar.tag()
        if not t or t[1] != 'CMorphHintItem':
            raise Fail('shape hint expected at %x' % ar.p)
        ar.raw(20)


def _layer(ar, o, notes):
    _base(ar, o, notes)
    ls = ar.u8()
    o.name = ar.cstring()
    o.current, o.locked, o.hidden = 0, 0, 0
    if ls <= 3:
        ar.u8()
    else:
        o.current, o.locked, o.hidden = ar.u8(), ar.u8(), ar.u8()
    if ls >= 5:
        ar.u32()
    if ls >= 6:
        ar.raw(8)
    if ls >= 8:
        ar.u32()
    o.mode = ar.u8()  # 0 normal, 1 guide, 3 folder, 4 mask
    o.parent = None
    t = ar.tag()
    if t:
        o.parent = t[1] if t[0] == 'ref' else _read_object(ar, t, notes)
    if 7 <= ls < 9:
        ar.tag()
    if 2 <= ls < 6:
        ar.u8()
    if 3 <= ls < 9:
        ar.u8()
    if ls >= 9:
        ar.u8()
    if ls >= 10:
        ar.u8()


def _page(ar, o, notes):
    _base(ar, o, notes)
    ps = ar.u8()
    if ps != 4:
        ar.u16()
    if ps >= 5:
        ar.u16()
    if ps >= 7:
        ar.u32()
    if ps >= 3:
        ar.raw(8 * ar.u32())


def _symbol(ar, o, notes):
    """A symbol instance: its matrix, First Frame and loop (0 loop, 1 play once, 2 single frame), colour
    transform (alpha, red, green, blue: multiplier /256 and offset), name and library symbol."""
    _base(ar, o, notes)
    ar.u8()
    o.matrix = ar.matrix()
    o.first = ar.u16()
    o.loop = ar.u16()
    ar.u8()
    o.color = [(ar.u16(), ar.u16()) for _ in range(4)]
    o.effect = ar.u32() & 0xFF
    ar.u32()
    o.name = ar.cstring()
    o.symbol = ar.u32()
    ar.raw(7)
    # The pivot in the symbol's own pixels, as XFL keeps it.
    point = getattr(o, 'point', None)
    o.pivot = _apply(_inverse(o.matrix), point) if point else (0.0, 0.0)


def _bitmap(ar, o, notes):
    _base(ar, o, notes)
    schema = ar.u8()
    o.matrix = ar.matrix()
    o.media = ar.u16()
    if schema >= 2 and ar.u8():
        raise Fail('bitmap filters are not known (at %x)' % ar.p)


def _continues(ar, at):
    """Whether the parent goes on at `at`: a null tag and an unset point, or an object that reads in full."""
    if at + 12 > len(ar.b):
        return False
    t = struct.unpack_from('<H', ar.b, at)[0]
    if t == 0:
        return struct.unpack_from('<ii', ar.b, at + 2) == (INT_MIN, INT_MIN)
    if not t & 0x8000:
        return False
    trial = Archive(ar.b)
    trial.p, trial.table = at, list(ar.table)
    try:
        tag = trial.tag()
        if not tag or tag[0] != 'new' or tag[1] not in _READERS:
            return False
        _read_object(trial, tag, [])
        return True
    except (Fail, IndexError, ValueError, KeyError, struct.error):
        return False


def _text(ar, o, notes):
    """Text: kept out (its run records aren't fully known); read past by finding where its parent goes on."""
    _base(ar, o, notes)
    ar.u8()
    o.matrix = ar.matrix()
    ar.raw(16)
    start = ar.p
    for k in range(0, 1 << 16):
        if _continues(ar, start + k):
            ar.p = start + k
            notes.append('text')
            return
    raise Fail('the end of a text object (from %x)' % start)


_READERS = {'CPicPage': _page, 'CPicLayer': _layer, 'CPicFrame': _frame, 'CPicShape': _shape,
            'CPicSymbol': _symbol, 'CPicBitmap': _bitmap, 'CPicText': _text}


def read_timeline(data, notes):
    """A Page or Symbol stream: its CPicPage."""
    ar = Archive(data)
    if ar.u8() != 1:
        raise Fail('not a timeline stream')
    t = ar.tag()
    if not t or t[1] != 'CPicPage':
        raise Fail('a timeline starts with CPicPage')
    return _read_object(ar, t, notes)


# --- The document: Contents ------------------------------------------------------------------------

def read_contents(data):
    """The stage size, frame rate and background, and the library's names ("Symbol N" -> name)."""
    out = {'width': 550, 'height': 400, 'fps': 24.0, 'background': (255, 255, 255, 255), 'names': {}}
    # The stage: a rect (0, right, 0, bottom) in twips just after "?? 00 05 00 00", the first byte
    # differing from file to file (04 to 20 seen); the rect's own check keeps out other matches.
    i = data.find(b'\x00\x05\x00\x00\x00\x00\x00', 1)
    while 0 < i and i + 20 <= len(data):
        l, r, t, b = struct.unpack_from('<iiii', data, i + 4)
        if l == 0 and t == 0 and 0 < r < 200000 and 0 < b < 200000:
            out['width'], out['height'] = round(r / 20), round(b / 20)
            break
        i = data.find(b'\x00\x05\x00\x00\x00\x00\x00', i + 1)
    # Background and grid colours (RGBA), a zero byte, then the frame rate (8.8 fixed).
    for i in range(len(data) - 12):
        if data[i + 3] == 0xFF and data[i + 7] == 0xFF and data[i + 8] == 0:
            fps = struct.unpack_from('<H', data, i + 9)[0] / 256
            if 1 <= fps <= 120 and data[i + 4:i + 7] == b'\xc0\xc0\xc0':
                out['background'] = tuple(data[i:i + 4])
                out['fps'] = fps
                break
    # Library items: "Symbol N" (u8 count + UTF-16) followed by the name (FF FE FF + u8 count + UTF-16).
    i = 0
    while True:
        i = data.find('Symbol '.encode('utf-16le'), i)
        if i < 0:
            break
        n = data[i - 1]
        stream = data[i:i + 2 * n].decode('utf-16le', 'replace')
        j = i + 2 * n
        if data[j:j + 3] == b'\xff\xfe\xff':
            m = data[j + 3]
            out['names'][stream] = data[j + 4:j + 4 + 2 * m].decode('utf-16le', 'replace')
        i = j
    return out


# --- Bitmaps -------------------------------------------------------------------------------------------

def _png(width, height, rgba):
    rows = b''.join(b'\x00' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


# A lossless bitmap's alpha is stored one more than it is (but 0 and 255 as they are).
_ALPHA_STORED = bytes(v - 1 if 0 < v < 255 else v for v in range(256))


def read_media(data):
    """(extension, bytes, (width, height) or None) of a Media stream, or None for what isn't a picture."""
    if data[:3] == b'\xff\xd8\xff':
        return ('jpg', data, None)
    if data[:8] == b'\x89PNG\r\n\x1a\n':
        return ('png', data, None)
    if data[:2] == b'\x03\x05':
        # Lossless: row size, width, height, frame (4 x u32 twips), flags (1: alpha), variant, then zlib
        # data in chunks; pixels A B G R, alpha premultiplied and stored +1.
        _, w, h = struct.unpack_from('<HHH', data, 2)
        alpha = data[24] & 1
        p, packed = 26, bytearray()
        while p + 2 <= len(data):
            n = struct.unpack_from('<H', data, p)[0]
            p += 2
            if n == 0:
                break
            packed += data[p:p + n]
            p += n
        px = zlib.decompress(bytes(packed))[:w * h * 4]
        # A B G R into R G B A all at once; alpha one less where it was stored +1. Only the pixels left
        # partly clear (edges, mostly) are un-premultiplied one by one.
        n = len(px) // 4
        a = px[0::4]
        a = b'\xff' * n if not alpha else a.translate(_ALPHA_STORED)
        rgba = bytearray(n * 4)
        rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4] = px[3::4], px[2::4], px[1::4], a
        if alpha:
            for found in re.finditer(rb'[\x01-\xfd]', a):
                i = found.start()
                k = a[i]
                for c in range(i * 4, i * 4 + 3):
                    rgba[c] = min(255, rgba[c] * 255 // k)
        return ('png', _png(w, h, bytes(rgba)), (w, h))
    return None


def _jpeg_size(data):
    p = 2
    while p + 9 < len(data):
        if data[p] != 0xFF:
            p += 1
            continue
        marker = data[p + 1]
        length = struct.unpack_from('>H', data, p + 2)[0]
        if marker in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack_from('>HH', data, p + 5)
            return (w, h)
        p += 2 + length
    return None


def _png_size(data):
    return struct.unpack_from('>II', data, 16)


# --- Geometry: Flash edges into Open Animate fills and lines ---------------------------------------

UNITS = 2560.0  # edge coordinates per pixel, in shapes before schema 3 (5120 after)


def _rgba(u32):
    return [((u32 >> s) & 0xFF) / 255.0 for s in (0, 8, 16, 24)]


def _fill_color(fill, notes):
    if fill['kind'] == 'gradient' and fill['stops']:
        # Its average colour across the gradient: each stop's colour over the stretch it covers.
        stops = sorted(fill['stops'])
        total, acc = 0.0, [0.0] * 4
        for i, (pos, color) in enumerate(stops):
            lo = 0 if i == 0 else (pos + stops[i - 1][0]) / 2
            hi = 255 if i == len(stops) - 1 else (pos + stops[i + 1][0]) / 2
            wgt = max(hi - lo, 1e-3)
            for k, v in enumerate(_rgba(color)):
                acc[k] += v * wgt
            total += wgt
        notes.append('gradient')
        return [v / total for v in acc]
    if fill['kind'] == 'bitmap':
        notes.append('bitmap fill')
        return [0.6, 0.6, 0.6, 1.0]
    return _rgba(fill['color'])


def _apply(m, p):
    a, b, c, d, tx, ty = m
    return (a * p[0] + c * p[1] + tx, b * p[0] + d * p[1] + ty)


def _mul(m, n):
    """m after n."""
    a, b, c, d, tx, ty = m
    e, f, g, h, ux, uy = n
    return (a * e + c * f, b * e + d * f, a * g + c * h, b * g + d * h, a * ux + c * uy + tx, b * ux + d * uy + ty)


def _inverse(m):
    a, b, c, d, tx, ty = m
    det = a * d - b * c
    if abs(det) < 1e-12:
        return (1, 0, 0, 1, -tx, -ty)
    ia, ib, ic, id_ = d / det, -b / det, -c / det, a / det
    return (ia, ib, ic, id_, -(ia * tx + ic * ty), -(ib * tx + id_ * ty))


def _area(poly):
    s = 0.0
    for i in range(len(poly)):
        x0, y0 = poly[i - 1]
        x1, y1 = poly[i]
        s += x0 * y1 - x1 * y0
    return s / 2


def _inside(pt, poly):
    x, y = pt
    inside = False
    for i in range(len(poly)):
        x0, y0 = poly[i - 1]
        x1, y1 = poly[i]
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            inside = not inside
    return inside


def _loops(edges):
    """Directed edges (from, ctrl, to) linked end to start into closed loops."""
    starts = {}
    for i, e in enumerate(edges):
        starts.setdefault(e[0], []).append(i)
    used = [False] * len(edges)
    loops = []
    for i in range(len(edges)):
        if used[i]:
            continue
        loop, j = [], i
        while j is not None and not used[j]:
            used[j] = True
            loop.append(edges[j])
            nxt = None
            for k in starts.get(edges[j][2], ()):
                if not used[k]:
                    nxt = k
                    break
            j = nxt
        if loop:
            loops.append(loop)
    return loops


def _flatten(loop):
    pts = []
    for (p0, ctrl, p2) in loop:
        pts.append(p0)
        if ctrl is not None:
            pts.append(((p0[0] + 2 * ctrl[0] + p2[0]) / 4, (p0[1] + 2 * ctrl[1] + p2[1]) / 4))
    return pts


class Drawing:
    """An Open Animate drawing being built: points, curves (cubic, handles stored) and shapes."""

    def __init__(self):
        self.points, self.curves, self.shapes = [], [], []

    def empty(self):
        return not self.shapes

    def _curve(self, segments, closed, hidden=()):
        """segments: (p0, ctrl or None, p2) in document space, joined end to start. Weights (1) and offsets (0)
        are left to the reader's defaults: a big file has hundreds of thousands of curve points."""
        points = self.points
        first = len(points)
        n = len(segments)
        ends = [s[0] for s in segments] + ([] if closed else [segments[-1][2]])
        m = len(ends)
        for p in ends:
            points.append({'pos': [round(p[0], 6), round(p[1], 6)]})
        h_in = [None] * m
        h_out = [None] * m
        for i, (p0, ctrl, p2) in enumerate(segments):
            x0, y0 = p0
            x2, y2 = p2
            # The handles' ends worked out first, then less their points (rounded the same either way).
            if ctrl is None:
                c1x, c1y = x0 + (x2 - x0) / 3, y0 + (y2 - y0) / 3
                c2x, c2y = x0 + (x2 - x0) * 2 / 3, y0 + (y2 - y0) * 2 / 3
            else:
                cx, cy = ctrl
                c1x, c1y = x0 + 2 / 3 * (cx - x0), y0 + 2 / 3 * (cy - y0)
                c2x, c2y = x2 + 2 / 3 * (cx - x2), y2 + 2 / 3 * (cy - y2)
            h_out[i] = [round(c1x - x0, 6), round(c1y - y0, 6)]
            h_in[(i + 1) % m] = [round(c2x - x2, 6), round(c2y - y2, 6)]
        cps = []
        for k in range(m):
            cp = {'point': first + k, 'curvature': 0.0, 'handle_in': h_in[k] or [0.0, 0.0],
                  'handle_out': h_out[k] or [0.0, 0.0]}
            if k in hidden:
                cp['edge_hidden'] = True
            cps.append(cp)
        self.curves.append({'closed': closed, 'points': cps})
        return len(self.curves) - 1, n

    def add_fill(self, outer, holes, color, name):
        """A filled area: its outline and holes (segment loops in document space), a curve each, in one
        shape filling Even-Odd so the holes are holes."""
        edges = []
        for loop in [outer] + list(holes):
            ci, n = self._curve(loop, True)
            edges += [[ci, k] for k in range(n)]
        shape = {'name': name, 'fill': True, 'outline': False,
                 'style': {'fill': color, 'line': color, 'line_width': 0.0}, 'edges': edges}
        if holes:
            shape['fill_rule'] = 'EVEN_ODD'
        self.shapes.append(shape)

    def add_line(self, segments, closed, color, width, name):
        ci, n = self._curve(segments, closed)
        self.shapes.append({'name': name, 'fill': False, 'outline': True,
                            'style': {'fill': color, 'line': color, 'line_width': width},
                            'edges': [[ci, k] for k in range(n)]})

    def to_json(self):
        for i, p in enumerate(self.points):
            p['id'] = i + 1
        for k, s in enumerate(self.shapes):
            s['id'] = len(self.points) + k + 1
        return {'points': self.points, 'curves': self.curves, 'shapes': self.shapes,
                'next_id': len(self.points) + len(self.shapes) + 1}


def add_shape_data(drawing, data, m, k, notes):
    """Flash shape data into `drawing`: its points through `m` (pixels, then to document units by `k`)."""
    units = data.get('units', UNITS)

    def tr(p):
        x, y = _apply(m, (p[0] / units, p[1] / units))
        return (x * k, -y * k)

    fills, lines = data['fills'], data['lines']
    # Fills: each edge has a fill on either side; going round each fill, an edge is forward with the fill
    # on its right (fill1) and backward with it on its left (fill0).
    by_fill = {}
    for (p0, ctrl, p2, f0, f1, ln) in data['edges']:
        if p0 == p2 and ctrl is None:
            continue
        if f1:
            by_fill.setdefault(f1, []).append((p0, ctrl, p2))
        if f0:
            by_fill.setdefault(f0, []).append((p2, ctrl, p0))
    for fi in sorted(by_fill):
        if fi > len(fills):
            continue
        color = _fill_color(fills[fi - 1], notes)
        loops = [[(tr(a), None if c is None else tr(c), tr(b)) for (a, c, b) in lp] for lp in _loops(by_fill[fi])]
        loops = [lp for lp in loops if len(lp) >= 2]
        polys = [_flatten(lp) for lp in loops]
        # Nesting: inside an even number of others, an outline; odd, a hole of the innermost one.
        depth, parent = [], []
        for i, poly in enumerate(polys):
            containers = [j for j in range(len(polys)) if j != i and polys[j] and _inside(poly[0], polys[j])]
            depth.append(len(containers))
            parent.append(min(containers, key=lambda j: abs(_area(polys[j]))) if containers else None)
        for i in range(len(loops)):
            if depth[i] % 2 == 0:
                holes = [loops[j] for j in range(len(loops)) if depth[j] % 2 == 1 and parent[j] == i]
                drawing.add_fill(loops[i], holes, color, 'Fill %d' % fi)
    # Lines: runs of edges with the same line style, joined where they meet end to end.
    by_line = {}
    for (p0, ctrl, p2, f0, f1, ln) in data['edges']:
        if ln and ln <= len(lines):
            by_line.setdefault(ln, []).append((p0, ctrl, p2))
    for li, edges in sorted(by_line.items()):
        style = lines[li - 1]
        width = max(style['width'] / 20.0, 1.0) * k
        for run, closed in _runs(edges):
            segs = [(tr(a), None if c is None else tr(c), tr(b)) for (a, c, b) in run]
            drawing.add_line(segs, closed, _rgba(style['color']), width, 'Line %d' % li)


def _runs(edges):
    """Edges joined into runs end to end (turning edges round as needed), broken where three or more meet."""
    ends = {}
    for i, (a, c, b) in enumerate(edges):
        ends.setdefault(a, []).append(i)
        ends.setdefault(b, []).append(i)
    used = [False] * len(edges)
    out = []

    def walk(start_point, i, forward):
        run = []
        p = start_point
        while i is not None:
            used[i] = True
            a, c, b = edges[i]
            if a == p:
                run.append((a, c, b))
                p = b
            else:
                run.append((b, c, a))
                p = a
            nxt = None
            if len(ends.get(p, ())) == 2:
                for j in ends[p]:
                    if not used[j]:
                        nxt = j
            i = nxt
        return run, p

    # Runs from the ends (points not joining exactly two), then what is left: closed loops.
    for p, members in ends.items():
        if len(members) != 2:
            for i in members:
                if not used[i]:
                    run, _ = walk(p, i, True)
                    out.append((run, False))
    for i in range(len(edges)):
        if not used[i]:
            run, last = walk(edges[i][0], i, True)
            out.append((run, last == run[0][0] and len(run) > 1))
    return out


# --- The scene ---------------------------------------------------------------------------------------

class Converter:
    def __init__(self, fla_path):
        self.cf = CompoundFile(fla_path)
        self.name = os.path.splitext(os.path.basename(fla_path))[0]
        self.notes = []
        self.next_id = 1
        self.symbols = {}
        self.media = {}
        self.packed = {}
        info = read_contents(self.cf.read('Contents')) if 'Contents' in self.cf.streams else read_contents(b'')
        self.info = info
        self.k = 2.0 / info['height']

    # Streams: Symbol N or S N <time>, Page N or P N <time>, Media N or M N <time>.
    def _stream(self, prefix_long, prefix_short, number):
        for name in (f'{prefix_long} {number}',):
            if name in self.cf.streams:
                return name
        for name in self.cf.streams:
            parts = name.split(' ')
            if len(parts) >= 2 and parts[0] == prefix_short and parts[1] == str(number):
                return name
        return None

    def symbol(self, number):
        if number not in self.symbols:
            name = self._stream('Symbol', 'S', number)
            page = None
            if name:
                try:
                    page = read_timeline(self.cf.read(name), self.notes)
                except Fail as e:
                    self.notes.append('unreadable symbol: %s (%s)' % (name, e))
            self.symbols[number] = page
        return self.symbols[number]

    def symbol_length(self, page):
        length = 0
        for layer in page.children:
            if layer.kind == 'CPicLayer':
                length = max(length, sum(f.duration for f in layer.children if f.kind == 'CPicFrame'))
        return length

    def symbol_name(self, number):
        return self.info['names'].get('Symbol %d' % number, 'Symbol %d' % number)

    def new_id(self):
        self.next_id += 1
        return self.next_id - 1

    def layer(self, name, kind, **extra):
        j = {'id': self.new_id(), 'name': name, 'type': kind, 'visible': True, 'render_visible': True,
             'locked': False, 'edit_with_bones': False, 'edit_descendants': True, 'expanded': False,
             'selected': False, 'label_color': 'NONE', 'position': [0.0, 0.0], 'rotation': 0.0,
             'scale': [1.0, 1.0], 'opacity': 1.0, 'parent_bone_deform': 'POINTS'}
        j.update(extra)
        return j

    # Flash's matrices act in pixels with y down; layers in document units with y up.
    def to_layer_transform(self, m):
        """The layer transform making `m` (pixels) exactly: (position, rotation, scale, shear), as Open
        Animate's layers keep it (translate, rotate, shear, scale: the X axis by the rotation, the Y axis by
        the rotation and the shear's y), and what is left for what is inside (nothing: the identity)."""
        a, b, c, d, tx, ty = m
        # In document space: y flipped both sides.
        p, q, r, s = a, -c, -b, d
        sx = math.hypot(p, r)
        if sx < 1e-9:
            return ([tx * self.k, -ty * self.k], 0.0, [0.0, 0.0], [0.0, 0.0]), (1, 0, 0, 1, 0, 0)
        theta = math.atan2(r, p)
        # The Y axis unturned: its length is the scale's y, its lean from up the shear (a mirrored one
        # points down: a negative scale).
        co, si = math.cos(-theta), math.sin(-theta)
        yx, yy = co * q - si * s, si * q + co * s
        sy = math.hypot(yx, yy)
        if yy < 0:
            yx, yy, sy = -yx, -yy, -sy
        shear = math.degrees(math.atan2(-yx, yy)) if abs(sy) > 1e-12 else 0.0
        return ([tx * self.k, -ty * self.k], math.degrees(theta), [sx, sy], [0.0, round(shear, 6)]), \
            (1, 0, 0, 1, 0, 0)

    @staticmethod
    def shear_of(shear):
        """A layer's shear, for its JSON: left out when there is none."""
        return {'shear': shear} if abs(shear[0]) > 1e-6 or abs(shear[1]) > 1e-6 else {}

    def doc_point(self, p):
        """A point in pixels (y down) in document units (y up)."""
        return [p[0] * self.k, -p[1] * self.k]

    def media_layer(self, element, residual, name):
        stream = self._stream('Media', 'M', element.media)
        if not stream:
            self.notes.append('missing bitmap')
            return None
        if stream not in self.media:
            got = read_media(self.cf.read(stream))
            if not got:
                self.notes.append('unknown bitmap format')
                return None
            ext, data, size = got
            size = size or (_jpeg_size(data) if ext == 'jpg' else _png_size(data))
            pack_id = hashlib.sha1(data).hexdigest()[:24]
            self.packed[pack_id] = ('%s.%s' % (stream.replace(' ', '_'), ext), data)
            self.media[stream] = (pack_id, size)
        pack_id, size = self.media[stream]
        if not size:
            return None
        w, h = size
        m = _mul(residual, element.matrix)
        # The picture's middle sits at the layer's origin.
        mid = _mul(m, (1, 0, 0, 1, w / 2, h / 2))
        (pos, rot, scale, shear), _ = self.to_layer_transform(mid)
        file_name = self.packed[pack_id][0]
        return self.layer(name, 'MEDIA', position=pos, rotation=rot, scale=scale, **self.shear_of(shear),
                          media={'fit': 'ORIGINAL', 'greyscale': False, 'tint': [0.2, 0.45, 1.0, 1.0],
                                 'tint_amount': 0.0, 'in_front': False, 'dont_auto_key': True, 'volume': 1.0,
                                 'muted': False, 'next_clip_id': 2,
                                 'clips': [{'id': 1, 'filepath': file_name, 'start': 1, 'length': self.length,
                                            'in': 0.0, 'timing': 'FRAME_BY_FRAME', 'end': 'HOLD',
                                            'packed': pack_id}]})

    def elements(self, frame, m, depth, elapsed=0, tween_to=None, span=1):
        """A keyframe's content as layers, top first: its own drawing at the bottom, then its objects
        (drawing groups, instances, bitmaps) above in order. Runs of drawings share one vector layer."""
        out = []
        drawing = [Drawing()]

        def flush(name):
            if not drawing[0].empty():
                out.append(self.layer(name, 'VECTOR', drawing=drawing[0].to_json()))
            drawing[0] = Drawing()

        add_shape_data(drawing[0], frame.data, _mul(m, frame.matrix), self.k, self.notes)
        self._objects(frame.children, m, depth, drawing, flush, out, (elapsed, span), tween_to)
        flush('Drawing')
        out.reverse()
        return out

    def _objects(self, objects, m, depth, drawing, flush, out, timing, tween_to):
        instance_index = 0
        for el in objects:
            if el.kind == 'CPicShape':
                mm = _mul(m, el.matrix)
                add_shape_data(drawing[0], el.data, mm, self.k, self.notes)
                self._objects(el.children, mm, depth, drawing, flush, out, timing, None)
            elif el.kind == 'CPicSymbol':
                flush('Drawing')
                target = None
                if tween_to is not None:
                    later = [o for o in tween_to[0].children if o.kind == 'CPicSymbol']
                    if instance_index < len(later) and later[instance_index].symbol == el.symbol:
                        target = later[instance_index]
                g = self.instance(el, m, depth, target and (target, tween_to[1], tween_to[2]), timing)
                instance_index += 1
                if g:
                    out.append(g)
            elif el.kind == 'CPicBitmap':
                flush('Drawing')
                g = self.media_layer(el, m, 'Bitmap')
                if g:
                    out.append(g)
            elif el.kind == 'CPicText' and getattr(el, 'runs', None):
                # Text read whole (XFL's); the binary format's text records stay unread.
                flush('Drawing')
                out.append(self.text_layer(el, m))

    def font_of(self, face):
        """A Flash font (its PostScript name, "Arial-Black") as (family, weight, italic): the installed font
        that has that name, else worked out from the name ("TimesNewRomanPS-BoldMT": Times New Roman,
        bold)."""
        try:
            import _oanim
            found = _oanim.text_font_from_postscript(face)
        except (ImportError, AttributeError):
            found = None
        if found:
            return tuple(found)
        base, _, style = face.partition('-')
        for tail in ('PSMT', 'MT', 'PS'):
            if base.endswith(tail) and len(base) > len(tail):
                base = base[:-len(tail)]
        family = re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', base)
        weight = 700 if 'Bold' in style else 900 if 'Black' in style else 400
        family_ok = True
        try:
            import _oanim
            family_ok = _oanim.text_font_installed(family)
        except (ImportError, AttributeError):
            pass
        if not family_ok:
            self.notes.append('font not installed')
        return family, weight, 'Italic' in style or 'Oblique' in style

    def text_layer(self, el, m):
        """A text field as a text layer: its runs' fonts, sizes and colours, aligned as in Flash. Its
        origin is the first line's top on its left end (or middle, or right end, as aligned); a box that
        wraps starts at the box's left edge, inside Flash's 2-pixel gutter."""
        runs = []
        for r in el.runs:
            family, weight, italic = self.font_of(r['face'])
            color = [round(c, 4) for c in r['color']]
            runs.append({'text': r['text'], 'font': family, 'weight': 700 if r['bold'] else weight,
                         'italic': italic or r['italic'], 'size': round(r['size'] * self.k, 6),
                         'fill': True, 'outline': False,
                         'style': {'fill': color, 'line': color, 'line_width': 0.0}})
        align = {'center': 'CENTER', 'right': 'RIGHT', 'justify': 'JUSTIFY'}.get(el.align, 'LEFT')
        size = max(el.runs[0]['size'], 1e-3) if el.runs else 12.0
        gutter = 2.0
        inner = max(el.width - 2 * gutter, 0.0)
        across = el.left + gutter
        if not el.wraps:
            across += {'CENTER': inner / 2, 'RIGHT': inner}.get(align, 0.0)
        whole = _mul(m, el.matrix)
        (pos, rot, scale, shear), _ = self.to_layer_transform(whole)
        pos = self.doc_point(_apply(whole, (across, gutter)))
        text = {'runs': runs, 'align': align if el.wraps or align != 'JUSTIFY' else 'LEFT',
                'letter_spacing': round(el.letter_spacing / size, 6),
                # Flash's line spacing is pixels added between lines: about that much of the font's own.
                'line_spacing': round(1.0 + el.line_spacing / (size * 1.15), 6),
                'box_width': round(inner * self.k, 6) if el.wraps else 0.0}
        first = ''.join(r['text'] for r in el.runs).split('\n')[0].strip()[:24]
        return self.layer(first or 'Text', 'TEXT', position=pos, rotation=rot, scale=scale, text=text,
                          **self.shear_of(shear))

    def instance(self, el, m, depth, tween=None, timing=(0, 1)):
        """A symbol instance: a group with the instance's transform (the skew left over pushed into its
        content), showing the symbol's frame it is set to."""
        if depth > 24:
            self.notes.append('symbols nested too deep')
            return None
        page = self.symbol(el.symbol)
        if page is None:
            return None
        whole = _mul(m, el.matrix)
        (pos, rot, scale, shear), residual = self.to_layer_transform(whole)
        # The group sits on the instance's transformation point (Flash's pivot), what is inside moved back
        # by as much, so it turns and scales about the same point it did in Flash.
        pivot = getattr(el, 'pivot', (0.0, 0.0))
        if abs(pivot[0]) > 1e-4 or abs(pivot[1]) > 1e-4:
            pos = self.doc_point(_apply(whole, pivot))
            residual = _mul(residual, (1, 0, 0, 1, -pivot[0], -pivot[1]))
        # Graphic symbols play in step with where they are: Single Frame stays on its First Frame, Loop
        # goes round, Play Once stops on its last frame. `timing`: frames since the keyframe began, and how
        # long it shows (a span longer than one on the main timeline would need the symbol animated).
        elapsed, span = timing
        length = self.symbol_length(page)
        if el.loop == 2:
            frame = el.first
        elif el.loop == 0:
            frame = (el.first + elapsed) % max(length, 1)
        else:
            frame = min(el.first + elapsed, max(length - 1, 0))
        if el.loop != 2 and span > 1 and length > 1:
            self.notes.append('looping symbol shown at its first frame')
        layers = self.timeline(page, residual, depth + 1, frame)
        extra = {}
        alpha_mul = el.color[0][0] / 256.0
        if el.effect == 0 or (alpha_mul >= 0.999 and all(c == (256, 0) for c in el.color)):
            pass
        elif all(c == (256, 0) for c in el.color[1:]) and el.color[0][1] == 0:
            extra['opacity'] = round(max(0.0, min(1.0, alpha_mul)), 4)
        else:
            self.notes.append('symbol colour effect (tint, brightness or advanced)')
            if el.color[0][0] != 256:
                extra['opacity'] = round(max(0.0, min(1.0, alpha_mul)), 4)
        if tween:
            target, start, end = tween
            whole2 = _mul(m, target.matrix)
            (pos2, rot2, scale2, shear2), _ = self.to_layer_transform(whole2)
            # The same point of the symbol at the end, so the tween runs on the pivot's path as Flash's.
            pos2 = self.doc_point(_apply(whole2, pivot))
            # The shorter way round, as Flash's Auto rotation.
            while rot2 - rot > 180:
                rot2 -= 360
            while rot2 - rot < -180:
                rot2 += 360
            interp = 'LINEAR'
            extra['position_keys'] = [self.key(0, pos, 'LINEAR'), self.key(start, pos, 'LINEAR'),
                                      self.key(end, pos2, interp)]
            extra['rotation_keys'] = [self.key(0, rot, 'LINEAR'), self.key(start, rot, 'LINEAR'),
                                      self.key(end, rot2, interp)]
            extra['scale_keys'] = [self.key(0, scale, 'LINEAR'), self.key(start, scale, 'LINEAR'),
                                   self.key(end, scale2, interp)]
            names = ['position_keys', 'rotation_keys', 'scale_keys']
            # Skew tweens as Flash's do: its angle eased with the rest.
            if shear != shear2:
                extra['shear_keys'] = [self.key(0, shear, 'LINEAR'), self.key(start, shear, 'LINEAR'),
                                       self.key(end, shear2, interp)]
                names.append('shear_keys')
            for name in names:
                extra[name] = self.dedupe(extra[name])
        extra.update(self.shear_of(shear))
        g = self.layer(el.name or self.symbol_name(el.symbol), 'GROUP', position=pos, rotation=rot, scale=scale,
                       children=layers, **extra)
        return g

    def key(self, frame, value, interp):
        return {'frame': frame, 'value': value, 'interp': interp}

    def dedupe(self, keys):
        out = []
        for k in keys:
            if out and out[-1]['frame'] == k['frame']:
                out[-1] = k
            else:
                out.append(k)
        return out

    def layer_tree(self, page):
        """The page's layers, top first, as (layer, children) with folders holding theirs."""
        order = [c for c in page.children if c.kind == 'CPicLayer']
        roots, kids = [], {}

        def folder_of(layer):
            parent = getattr(layer, 'parent', None)
            if parent is not None and parent.mode == 4:
                return folder_of(parent)
            return parent if parent is not None and parent.mode == 3 else None

        def place(layer):
            folder = folder_of(layer)
            bucket = roots if folder is None else kids.setdefault(id(folder), [])
            if layer not in bucket:
                bucket.append(layer)
            # A mask layer (stored only as its masked layers' parent) sits just above them.
            parent = getattr(layer, 'parent', None)
            if parent is not None and parent.mode == 4:
                place(parent)
            if folder is not None:
                place(folder)

        for layer in order:
            place(layer)
        # Stored bottom first (as fla-viewer found too); Open Animate lists them top first.
        roots.reverse()
        for v in kids.values():
            v.reverse()
        return roots, kids

    def timeline(self, page, m, depth, frame, main=False):
        """A timeline's layers at `frame` (a symbol shown still), or all its frames (`main`), top first."""
        roots, kids = self.layer_tree(page)
        ids = {}
        # A symbol's length is its longest layer's; a frame past a layer's end shows nothing there.
        length = 0
        for layer in [c for c in page.children if c.kind == 'CPicLayer']:
            length = max(length, sum(f.duration for f in layer.children if f.kind == 'CPicFrame'))
        if length and not main:
            frame %= length

        def build(layer):
            if layer.mode == 3:
                children = [x for x in (build(c) for c in kids.get(id(layer), [])) if x]
                if not children:
                    return None
                return self.layer(layer.name or 'Folder', 'GROUP', children=children, expanded=True)
            frames = [f for f in layer.children if f.kind == 'CPicFrame']
            if not frames:
                return None
            starts = []
            t = 0
            for f in frames:
                starts.append(t)
                t += f.duration
            if main:
                content = self.keyframes(layer, frames, starts, m, depth)
            else:
                if frame >= t:
                    return None
                at = frame
                i = max(k for k in range(len(frames)) if starts[k] <= at)
                f = frames[i]
                if f.tween == 1 and i + 1 < len(frames) and at > starts[i]:
                    content = self.tweened_still(f, frames[i + 1], (at - starts[i]) / f.duration, m, depth,
                                                 at - starts[i])
                else:
                    if f.tween == 2:
                        self.notes.append('shape tween')
                    content = self.elements(f, m, depth, at - starts[i])
            if not content:
                return None
            # Flash's eye icon hides a layer only while drawing: published, every layer but the guides
            # shows (Include Hidden Layers, on by default). Sheets keep the art hidden under visible
            # guide copies of it, so a hidden layer comes in shown and only a hidden guide stays hidden.
            # Guides show on Flash's stage, inside symbols too, and are left out only when published:
            # some sheets draw a part only on one (the front jaw's Line Reserve), so a guide comes in
            # shown in the viewport but not rendered.
            extra = {}
            if layer.mode == 1:
                extra['render_visible'] = False
                if layer.hidden:
                    extra['visible'] = False
            if len(content) == 1 and not main:
                j = content[0]
                j['name'] = layer.name or j['name']
                j.update(extra)
            else:
                j = self.layer(layer.name or 'Layer', 'GROUP', children=content, **extra)
            ids[id(layer)] = j
            return j

        out = [x for x in (build(layer) for layer in roots) if x]
        # Masks: a mask layer's shapes show what's under them in the layers it masks.
        for layer in [c for c in page.children if c.kind == 'CPicLayer']:
            parent = getattr(layer, 'parent', None)
            if parent is not None and parent.mode == 4 and id(layer) in ids and id(parent) in ids:
                mask = ids[id(parent)]
                ids[id(layer)].setdefault('masks', []).append(
                    {'name': 'Mask', 'start': 'NOTHING', 'enabled': True, 'render': True, 'cuts': 'ALL',
                     'end_lines': False, 'end_lines_flat': False, 'line_reach': 0.0, 'show_expanded': False,
                     'steps': [{'mode': 'ADD', 'invert': False, 'layers': [mask['id']], 'combine': 'UNION',
                                'grow': 0.0, 'fills_only': False, 'strokes_only': False, 'inside_stroke': False,
                                'filled': False, 'extend': 'NONE', 'show_expanded': False}]})
                mask['visible'] = False
        return out

    def tweened_still(self, f, nxt, t, m, depth, elapsed=0):
        """A motion tween seen on one frame: its instances where the tween has them then."""
        t = self.eased(f, t)
        later = [o for o in nxt.children if o.kind == 'CPicSymbol']
        saved = []
        k = 0
        for o in f.children:
            if o.kind == 'CPicSymbol':
                if k < len(later) and later[k].symbol == o.symbol:
                    saved.append((o, o.matrix))
                    o.matrix = self.blend(o.matrix, later[k].matrix, t, getattr(o, 'pivot', (0.0, 0.0)))
                k += 1
        try:
            return self.elements(f, m, depth, elapsed)
        finally:
            for o, mat in saved:
                o.matrix = mat

    def eased(self, f, t):
        if f.custom_ease:
            pts = [(p[2], p[3]) for p in f.custom_ease]
            for i in range(len(pts) - 1):
                (x0, y0), (x1, y1) = pts[i], pts[i + 1]
                if x0 <= t <= x1 and x1 > x0:
                    return y0 + (y1 - y0) * (t - x0) / (x1 - x0)
            return t
        e = f.ease / 100.0
        return t + e * t * (1 - t)

    def blend(self, a, b, t, pivot=(0.0, 0.0)):
        """Between two matrices as Flash's classic tween: rotation, scale and skew each, and the pivot's
        place (the transformation point, in the symbol's space)."""
        def parts(m):
            ma, mb, mc, md, tx, ty = m
            sx = math.hypot(ma, mb)
            sy = math.hypot(mc, md)
            rx = math.atan2(mb, ma)
            ry = math.atan2(-mc, md)
            return sx, sy, rx, ry, tx, ty

        A, B = parts(a), parts(b)
        out = []
        for i, (x, y) in enumerate(zip(A, B)):
            if i in (2, 3):
                while y - x > math.pi:
                    y -= 2 * math.pi
                while y - x < -math.pi:
                    y += 2 * math.pi
            out.append(x + (y - x) * t)
        sx, sy, rx, ry, _, _ = out
        lin = (sx * math.cos(rx), sx * math.sin(rx), -sy * math.sin(ry), sy * math.cos(ry), 0.0, 0.0)
        pa, pb = _apply(a, pivot), _apply(b, pivot)
        q = _apply(lin, pivot)
        return lin[:4] + (pa[0] + (pb[0] - pa[0]) * t - q[0], pa[1] + (pb[1] - pa[1]) * t - q[1])

    def keyframes(self, layer, frames, starts, m, depth):
        """A main timeline layer: one group per keyframe, shown from its frame until the next. A motion
        tween's instances get transform keys to where the next keyframe has them."""
        content = []
        for i, f in enumerate(frames):
            tween_to = None
            if f.tween == 1 and i + 1 < len(frames):
                tween_to = (frames[i + 1], starts[i] + 1, starts[i + 1] + 1)
            elif f.tween == 2:
                self.notes.append('shape tween')
            layers = self.elements(f, m, depth, 0, tween_to, f.duration)
            if not layers:
                continue
            if len(frames) == 1:
                content.extend(layers)
                continue
            start, end = starts[i] + 1, starts[i] + f.duration + 1
            g = self.layer(f.label or 'Frame %d' % start, 'GROUP', children=layers)
            keys = [self.key(0, 1.0 if i == 0 else 0.0, 'STEP')]
            if i > 0:
                keys.append(self.key(start, 1.0, 'STEP'))
            if i + 1 < len(frames):
                keys.append(self.key(end, 0.0, 'STEP'))
            g['visible_keys'] = self.dedupe(keys)
            g['visible'] = i == 0
            content.append(g)
        return content

    def convert(self):
        pages = sorted((n for n in self.cf.streams if n.split(' ')[0] in ('Page', 'P')),
                       key=lambda n: int(n.split(' ')[1]))
        if not pages:
            raise Fail('no scene in the file')
        # The length: the longest layer of the first scene.
        page = read_timeline(self.cf.read(pages[0]), self.notes)
        length = 1
        for layer in page.children:
            if layer.kind == 'CPicLayer':
                length = max(length, sum(f.duration for f in layer.children if f.kind == 'CPicFrame'))
        self.length = length
        if len(pages) > 1:
            self.notes.append('scenes after the first')
        w, h = self.info['width'], self.info['height']
        root = (1, 0, 0, 1, -w / 2, -h / 2)
        layers = self.timeline(page, root, 0, 0, main=True)
        return layers

    def report(self):
        counts = {}
        for n in self.notes:
            counts[n] = counts.get(n, 0) + 1
        return counts


_SAYS = {
    'gradient': 'gradient fills came in as solid fills of their average colour',
    'bitmap fill': 'bitmap fills came in grey',
    'text': 'text objects were left out (only Animate .fla files have their text read)',
    'font not installed': 'text in fonts that are not installed shows in another font',
    'shape tween': 'shape tweens hold their first shape',
    'looping symbol shown at its first frame': 'animated symbols on the main timeline show their frame at each keyframe only',
    'symbol colour effect (tint, brightness or advanced)': 'symbol tints and brightness were left out (alpha kept)',
    'scenes after the first': 'only the first scene came in',
}


def convert(fla_path, oanim_path, template=None, compress=True):
    """Writes `fla_path` as the .oanim `oanim_path` (`compress` off: stored as it is, for a file opened at once
    and thrown away). Returns {"layers", "frames", "notes": [sentences]}."""
    from . import io_xfl
    # Flash CS5 on (Animate) saves XFL: a zip of XML, or an .xfl project folder.
    c = io_xfl.XflConverter(fla_path) if io_xfl.is_xfl(fla_path) else Converter(fla_path)
    layers = c.convert()
    info = c.info
    bg = [round(v / 255.0, 4) for v in info['background']]
    doc = {'name': c.name, 'frame_start': 1, 'frame_end': max(1, c.length), 'frame_current': 1, 'fps': info['fps'],
           'width': info['width'], 'height': info['height'], 'resolution_linked': False,
           'resolution_aspect': round(info['width'] / info['height'], 6), 'background': bg,
           'layers': layers, 'next_layer_id': c.next_id, 'active_layer': layers[0]['id'] if layers else 0,
           'camera': {'position': [0.0, 0.0], 'roll': 0.0, 'zoom': 1.0, 'selected': False, 'passepartout': 0.6,
                      'keys': {}}}
    root = {'format': 'OpenAnimate', 'version': 4, 'document': doc}
    with zipfile.ZipFile(oanim_path, 'w', zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED) as z:
        z.writestr('document.json', json.dumps(root, separators=(',', ':')))
        for pack_id, (name, data) in c.packed.items():
            z.writestr('media/%s/%s' % (pack_id, name), data, compress_type=zipfile.ZIP_STORED)
    notes = []
    for key, n in sorted(c.report().items()):
        if key in _SAYS:
            notes.append('%s (%d)' % (_SAYS[key], n))
        elif key.startswith('unreadable symbol'):
            notes.append(key)
    def count(ls):
        return sum(1 + count(l.get('children', [])) for l in ls)
    return {'layers': count(layers), 'frames': c.length, 'notes': notes}
