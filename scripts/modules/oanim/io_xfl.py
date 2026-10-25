"""Flash CS5 and later (Animate) documents: XFL, a zip of XML (DOMDocument.xml, LIBRARY/<symbol>.xml,
bin/Media N.dat), or the same unzipped as a folder with its .xfl file. Read into the objects the binary .fla
reader makes (io_fla), so the one converter turns either into Open Animate layers.

Shapes are fills and strokes with edges, as in the binary format: each Edge has a fill on its left
(fillStyle0) and right (fillStyle1) and a stroke; its `edges` text is "!x y" (move), "|x y" or "/x y" (line),
"[cx cy x y" or "]cx cy x y" (quadratic), in twips, a number "#HHHHHH.HH" being 24.8 fixed point in hex
(fla-viewer's notes on XFL). Bitmaps in bin/ are the binary format's media streams as they are.
"""

import os
import re
import struct
import xml.etree.ElementTree as ET
import zipfile
import zlib

from . import io_fla
from .io_fla import Obj

NS = '{http://ns.adobe.com/xfl/2008/}'
TWIPS = 20.0


def is_xfl(path):
    """Whether `path` is an XFL document: a zip (an .fla from Flash CS5 on) or an .xfl project file."""
    if path.lower().endswith('.xfl'):
        return True
    with open(path, 'rb') as f:
        return f.read(4) == b'PK\x03\x04'


def _zip_entries(path):
    """Every file in a zip, by name. When its central directory is wrong (Animate leaves some so), the
    entries are read one by one from their own headers instead."""
    try:
        with zipfile.ZipFile(path) as z:
            return {i.filename: z.read(i) for i in z.infolist() if not i.is_dir()}
    except (zipfile.BadZipFile, OSError, EOFError, zlib.error):
        pass
    data = open(path, 'rb').read()
    out = {}
    p = 0
    while True:
        p = data.find(b'PK\x03\x04', p)
        if p < 0 or p + 30 > len(data):
            break
        _ver, flags, method, _t, _d, _crc, csize, _usize, nlen, xlen = struct.unpack_from('<HHHHHIIIHH', data, p + 4)
        name = data[p + 30:p + 30 + nlen].decode('utf-8' if flags & 0x800 else 'cp437', 'replace')
        start = p + 30 + nlen + xlen
        try:
            if method == 8:
                d = zlib.decompressobj(-15)
                body = d.decompress(data[start:] if flags & 8 else data[start:start + csize])
                used = len(data) - start - len(d.unused_data) if flags & 8 else csize
            elif method == 0:
                body, used = data[start:start + csize], csize
            else:
                p = start
                continue
        except zlib.error:
            p = start
            continue
        if not name.endswith('/'):
            out[name] = body
        p = start + max(used, 0)
    return out


class Files:
    """The document's files by path inside it ('DOMDocument.xml', 'LIBRARY/x.xml', 'bin/Media 1.dat')."""

    def __init__(self, path):
        if path.lower().endswith('.xfl'):
            root = os.path.dirname(os.path.abspath(path))
            self.entries = None
            self.root = root
        else:
            self.entries = {k.replace('\\', '/'): v for k, v in _zip_entries(path).items()}
            self.root = None

    def read(self, name):
        if self.entries is not None:
            if name in self.entries:
                return self.entries[name]
            # Names differ in case on some saves.
            low = name.lower()
            for k, v in self.entries.items():
                if k.lower() == low:
                    return v
            return None
        full = os.path.join(self.root, *name.split('/'))
        return open(full, 'rb').read() if os.path.isfile(full) else None


def _local(tag):
    return tag[len(NS):] if tag.startswith(NS) else tag


def _kids(el, name):
    """The children of `el` called `name` (the XFL namespace or none)."""
    return [c for c in el if _local(c.tag) == name]


def _kid(el, name):
    for c in el:
        if _local(c.tag) == name:
            return c
    return None


def _number(text):
    """An edge coordinate: decimal, or "#HHHHHH.HH" (24.8 fixed point in hex, two's complement)."""
    if text.startswith('#'):
        whole, _, frac = text[1:].partition('.')
        v = (int(whole, 16) << 8) | (int(frac[:2].ljust(2, '0'), 16) if frac else 0)
        # Negative numbers are written whole, as 32 bits (FFFE99.0D); short ones are positive.
        if len(whole) >= 6 and v & 0x80000000:
            v -= 1 << 32
        return v / 256.0
    return float(text)


def _color(hex_text, alpha=1.0):
    """'#RRGGBB' and an alpha (0-1) as the binary format's colour word (R, G, B, A from the low byte)."""
    h = (hex_text or '#000000').lstrip('#')
    if len(h) == 3:
        h = ''.join(c * 2 for c in h)
    r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    a = max(0, min(255, round(float(alpha) * 255)))
    return r | g << 8 | b << 16 | a << 24


def _matrix(el):
    """An element's matrix (a b c d tx ty, pixels), identity without one."""
    holder = _kid(el, 'matrix')
    m = _kid(holder, 'Matrix') if holder is not None else None
    if m is None:
        return (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
    g = lambda k, d: float(m.get(k, d))
    return (g('a', 1), g('b', 0), g('c', 0), g('d', 1), g('tx', 0), g('ty', 0))


def _fill(style_el, notes):
    """A FillStyle's (or a stroke's fill) as the binary reader's fill."""
    for c in style_el:
        kind = _local(c.tag)
        if kind == 'SolidColor':
            return {'color': _color(c.get('color'), c.get('alpha', 1)), 'kind': 'solid'}
        if kind in ('LinearGradient', 'RadialGradient'):
            stops = [(round(float(e.get('ratio', 0)) * 255), _color(e.get('color'), e.get('alpha', 1)))
                     for e in _kids(c, 'GradientEntry')]
            return {'kind': 'gradient', 'stops': stops, 'color': stops[0][1] if stops else 0}
        if kind == 'BitmapFill':
            return {'kind': 'bitmap', 'color': 0}
    return {'color': _color('#000000'), 'kind': 'solid'}


_TOKENS = re.compile(r'[!|/\[\]]|S\d+|[^\s!|/\[\]S]+')


def _edges(text):
    """An Edge's `edges` text as segments ((x0, y0), ctrl or None, (x1, y1)), in twips."""
    out = []
    at = None
    tokens = _TOKENS.findall(text)
    i = 0

    def take(n):
        nonlocal i
        vals = []
        while len(vals) < n and i < len(tokens):
            t = tokens[i]
            if t in '!|/[]' or t.startswith('S'):
                break
            vals.append(_number(t))
            i += 1
        return vals if len(vals) == n else None

    while i < len(tokens):
        t = tokens[i]
        i += 1
        if t == '!':
            p = take(2)
            if p:
                at = (p[0], p[1])
        elif t in '|/':
            p = take(2)
            if p and at is not None:
                out.append((at, None, (p[0], p[1])))
                at = (p[0], p[1])
        elif t in '[]':
            p = take(4)
            if p and at is not None:
                out.append((at, (p[0], p[1]), (p[2], p[3])))
                at = (p[2], p[3])
    return out


def _shape(el, notes):
    """A DOMShape as the binary reader's shape data."""
    fills, lines = [], []
    fills_el = _kid(el, 'fills')
    by_index = {}
    if fills_el is not None:
        for f in _kids(fills_el, 'FillStyle'):
            by_index[int(f.get('index', len(by_index) + 1))] = _fill(f, notes)
    fills = [by_index.get(i + 1, {'color': 0, 'kind': 'solid'}) for i in range(max(by_index, default=0))]
    strokes_el = _kid(el, 'strokes')
    by_index = {}
    if strokes_el is not None:
        for s in _kids(strokes_el, 'StrokeStyle'):
            stroke = next(iter(s), None)
            if stroke is None:
                continue
            fill_el = _kid(stroke, 'fill')
            color = _fill(fill_el, notes)['color'] if fill_el is not None else _color('#000000')
            if _local(stroke.tag) != 'SolidStroke':
                notes.append('stroke style (dashed, dotted, ...) drawn solid')
            weight = float(stroke.get('weight', 1))
            by_index[int(s.get('index', len(by_index) + 1))] = {'color': color, 'width': round(weight * TWIPS)}
    lines = [by_index.get(i + 1, {'color': 0, 'width': 0}) for i in range(max(by_index, default=0))]
    edges = []
    edges_el = _kid(el, 'edges')
    if edges_el is not None:
        for e in _kids(edges_el, 'Edge'):
            f0, f1 = int(e.get('fillStyle0', 0)), int(e.get('fillStyle1', 0))
            ln = int(e.get('strokeStyle', 0))
            for (p0, ctrl, p1) in _edges(e.get('edges', '')):
                edges.append((p0, ctrl, p1, f0, f1, ln))
    return {'fills': fills, 'lines': lines, 'edges': edges, 'units': TWIPS}


_EMPTY = {'fills': [], 'lines': [], 'edges': [], 'units': TWIPS}
_IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
_LOOPS = {'loop': 0, 'play once': 1, 'single frame': 2}


class XflConverter(io_fla.Converter):
    """io_fla's converter on an XFL document: its symbols and media come from XML and bin/ instead of
    the binary file's streams."""

    def __init__(self, path):
        self.files = Files(path)
        doc_xml = self.files.read('DOMDocument.xml')
        if doc_xml is None:
            raise io_fla.Fail('no DOMDocument.xml (not an XFL document)')
        self.doc = ET.fromstring(doc_xml)
        stem = os.path.splitext(os.path.basename(path))[0]
        if path.lower().endswith('.xfl'):
            stem = os.path.basename(os.path.dirname(os.path.abspath(path))) or stem
        self.name = stem
        self.notes = []
        self.next_id = 1
        self.symbols = {}
        self.media = {}
        self.packed = {}
        d = self.doc
        bg = _color(d.get('backgroundColor', '#FFFFFF'))
        self.info = {'width': round(float(d.get('width', 550))), 'height': round(float(d.get('height', 400))),
                     'fps': float(d.get('frameRate', 24)),
                     'background': (bg & 0xFF, bg >> 8 & 0xFF, bg >> 16 & 0xFF, 255), 'names': {}}
        self.k = 2.0 / self.info['height']
        # The library: each symbol a number (as the binary file's "Symbol N"), bitmaps by name.
        self.symbol_numbers, self.symbol_files = {}, {}
        symbols_el = _kid(d, 'symbols')
        for inc in (_kids(symbols_el, 'Include') if symbols_el is not None else []):
            href = inc.get('href', '')
            number = len(self.symbol_files) + 1
            self.symbol_files[number] = href
            name = href[:-4] if href.lower().endswith('.xml') else href
            self.symbol_numbers[name] = number
            self.info['names']['Symbol %d' % number] = name
        self.bitmaps = {}
        media_el = _kid(d, 'media')
        for item in (media_el if media_el is not None else []):
            if _local(item.tag) == 'DOMBitmapItem':
                self.bitmaps[item.get('name', '')] = item.get('bitmapDataHRef') or item.get('href', '')
        self.media_numbers = {}
        # Streams the base class reads by name: the bitmaps, as "Media N".
        self.cf = self

    # --- What io_fla.Converter reads from the binary file -------------------------------------------

    @property
    def streams(self):
        return {'Media %d' % n: None for n in self.media_numbers.values()}

    def read(self, stream):
        for href, n in self.media_numbers.items():
            if stream == 'Media %d' % n:
                return self.files.read('bin/' + href) or b''
        return b''

    def symbol(self, number):
        if number not in self.symbols:
            page = None
            href = self.symbol_files.get(number)
            raw = self.files.read('LIBRARY/' + href) if href else None
            if raw is not None:
                try:
                    item = ET.fromstring(raw)
                    timeline = _kid(item, 'timeline')
                    tl = _kid(timeline, 'DOMTimeline') if timeline is not None else None
                    if tl is not None:
                        page = self.xml_page(tl)
                except ET.ParseError as e:
                    self.notes.append('unreadable symbol: %s (%s)' % (href, e))
            self.symbols[number] = page
        return self.symbols[number]

    # --- XML into the binary reader's objects -----------------------------------------------------

    def xml_page(self, timeline):
        """A DOMTimeline as a CPicPage: its layers stored bottom first, as the binary format keeps them."""
        page = Obj('CPicPage')
        layers_el = _kid(timeline, 'layers')
        xml_layers = _kids(layers_el, 'DOMLayer') if layers_el is not None else []
        made = []
        for el in xml_layers:
            layer = Obj('CPicLayer')
            layer.name = el.get('name', 'Layer')
            layer.mode = {'folder': 3, 'mask': 4, 'guide': 1}.get(el.get('layerType', 'normal'), 0)
            layer.hidden = 1 if el.get('visible') == 'false' else 0
            layer.locked = 1 if el.get('locked') == 'true' else 0
            layer.current = 1 if el.get('current') == 'true' else 0
            layer.parent = None
            frames_el = _kid(el, 'frames')
            at = 0
            for f in (_kids(frames_el, 'DOMFrame') if frames_el is not None else []):
                index = int(f.get('index', at))
                if index > at:
                    layer.children.append(self.xml_frame(None, index - at))
                    at = index
                frame = self.xml_frame(f, int(f.get('duration', 1)))
                layer.children.append(frame)
                at += frame.duration
            made.append((el, layer))
        for el, layer in made:
            parent = el.get('parentLayerIndex')
            if parent is not None and 0 <= int(parent) < len(made):
                layer.parent = made[int(parent)][1]
        page.children = [layer for _, layer in reversed(made)]
        return page

    def xml_frame(self, el, duration):
        frame = Obj('CPicFrame')
        frame.duration = max(1, duration)
        frame.data = _EMPTY
        frame.matrix = _IDENTITY
        frame.tween, frame.ease, frame.label, frame.custom_ease = 0, 0, '', None
        if el is None:
            return frame
        kind = el.get('tweenType', '')
        frame.tween = {'motion': 1, 'shape': 2}.get(kind, 0)
        if kind == 'motion object':
            self.notes.append('motion tween (Animate style) shown at its keyframes')
        frame.ease = int(float(el.get('acceleration', 0)))
        frame.label = el.get('name', '')
        elements = _kid(el, 'elements')
        frame.children = self.xml_elements(elements) if elements is not None else []
        return frame

    def xml_elements(self, holder):
        out = []
        for el in holder:
            kind = _local(el.tag)
            if kind == 'DOMShape':
                o = Obj('CPicShape')
                o.matrix = _matrix(el)
                o.data = _shape(el, self.notes)
                out.append(o)
            elif kind == 'DOMGroup':
                o = Obj('CPicShape')
                o.matrix = _matrix(el)
                o.data = _EMPTY
                members = _kid(el, 'members')
                o.children = self.xml_elements(members) if members is not None else []
                out.append(o)
            elif kind == 'DOMSymbolInstance':
                out.append(self.instance_of(el))
            elif kind == 'DOMBitmapInstance':
                href = self.bitmaps.get(el.get('libraryItemName', ''))
                if not href:
                    self.notes.append('missing bitmap')
                    continue
                if href not in self.media_numbers:
                    self.media_numbers[href] = len(self.media_numbers) + 1
                o = Obj('CPicBitmap')
                o.matrix = _matrix(el)
                o.media = self.media_numbers[href]
                out.append(o)
            elif kind in ('DOMStaticText', 'DOMDynamicText', 'DOMInputText'):
                out.append(self.text_of(el, kind))
            elif kind in ('DOMRectangleObject', 'DOMOvalObject'):
                self.notes.append('rectangle and oval primitives')
        return out

    def text_of(self, el, kind):
        """A text field: its runs (text, face, size in pixels, colour (r, g, b, a), bold, italic), alignment,
        letter and line spacing (pixels) and box: `left` and `width` across from its origin, wrapping when
        it is a fixed-width box (Static text that doesn't auto-expand, or multiline Dynamic and Input)."""
        o = Obj('CPicText')
        o.matrix = _matrix(el)
        o.runs = []
        o.align, o.letter_spacing, o.line_spacing = 'left', 0.0, 0.0
        holder = _kid(el, 'textRuns')
        for run in _kids(holder, 'DOMTextRun') if holder is not None else []:
            chars = _kid(run, 'characters')
            text = (chars.text or '') if chars is not None else ''
            text = text.replace('\r\n', '\n').replace('\r', '\n')
            attrs_holder = _kid(run, 'textAttrs')
            a = _kid(attrs_holder, 'DOMTextAttrs') if attrs_holder is not None else None
            a = a.attrib if a is not None else {}
            if not o.runs:
                o.align = a.get('alignment', 'left')
                o.letter_spacing = float(a.get('letterSpacing', 0))
                o.line_spacing = float(a.get('lineSpacing', 0))
            u = _color(a.get('fillColor', '#000000'), a.get('alpha', 1))
            o.runs.append({'text': text, 'face': a.get('face', 'Arial'), 'size': float(a.get('size', 12)),
                           'color': [(u >> s & 0xFF) / 255.0 for s in (0, 8, 16, 24)],
                           'bold': a.get('bold') == 'true', 'italic': a.get('italic') == 'true'})
        o.left = float(el.get('left', 0))
        o.width = float(el.get('width', 0))
        if kind == 'DOMStaticText':
            o.wraps = el.get('autoExpand') != 'true' and o.width > 0
        else:
            o.wraps = el.get('lineType', 'single line') == 'multiline' and o.width > 0
        return o

    def instance_of(self, el):
        o = Obj('CPicSymbol')
        o.matrix = _matrix(el)
        # The transformation point (Flash's pivot), in the symbol's own pixels.
        holder = _kid(el, 'transformationPoint')
        point = _kid(holder, 'Point') if holder is not None else None
        o.pivot = (float(point.get('x', 0)), float(point.get('y', 0))) if point is not None else (0.0, 0.0)
        o.name = el.get('name', '')
        o.symbol = self.symbol_numbers.get(el.get('libraryItemName', ''), 0)
        if el.get('symbolType', 'graphic') == 'graphic':
            o.first = int(el.get('firstFrame', 0))
            o.loop = _LOOPS.get(el.get('loop', 'loop'), 0)
        else:
            # A movie clip or button plays on its own: shown at its first frame.
            o.first, o.loop = 0, 2
        # Colour: alpha, red, green, blue as (multiplier x 256, offset), from the multipliers and offsets, or
        # a tint or brightness.
        color = [(256, 0)] * 4
        o.effect = 0
        holder = _kid(el, 'color')
        c = _kid(holder, 'Color') if holder is not None else None
        if c is not None:
            o.effect = 1
            mul = {k: float(c.get(k + 'Multiplier', 1)) for k in ('alpha', 'red', 'green', 'blue')}
            add = {k: float(c.get(k + 'Offset', 0)) for k in ('alpha', 'red', 'green', 'blue')}
            if c.get('tintMultiplier') is not None:
                t = float(c.get('tintMultiplier', 0))
                tint = _color(c.get('tintColor', '#000000'))
                for k, shift in (('red', 0), ('green', 8), ('blue', 16)):
                    mul[k], add[k] = 1 - t, (tint >> shift & 0xFF) * t
            if c.get('brightness') is not None:
                b = float(c.get('brightness', 0))
                for k in ('red', 'green', 'blue'):
                    mul[k], add[k] = (1 - b, 255 * b) if b > 0 else (1 + b, 0)
            color = [(round(mul[k] * 256), round(add[k])) for k in ('alpha', 'red', 'green', 'blue')]
        o.color = color
        return o

    def convert(self):
        timelines = _kid(self.doc, 'timelines')
        scenes = _kids(timelines, 'DOMTimeline') if timelines is not None else []
        if not scenes:
            raise io_fla.Fail('no scene in the file')
        if len(scenes) > 1:
            self.notes.append('scenes after the first')
        page = self.xml_page(scenes[0])
        self.length = max([sum(f.duration for f in l.children) for l in page.children] or [1]) or 1
        w, h = self.info['width'], self.info['height']
        return self.timeline(page, (1, 0, 0, 1, -w / 2, -h / 2), 0, 0, main=True)
