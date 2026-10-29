"""The renderer's referee: renders the same scenes through two configurations of Open Animate and compares them.

A configuration is a build (OpenAnimate.exe) and, optionally, Viewport Shading settings, e.g.
    python tools/render_check/render_check.py --a build/run/20261028-120000/OpenAnimate.exe --b build/bin/RelWithDebInfo/OpenAnimate.exe
    python tools/render_check/render_check.py --a EXE --b EXE:layer_caching=0
    python tools/render_check/render_check.py --a EXE --b EXE:gpu_geometry=1 --suites fills,seams,masks
GPU Geometry draws fills of 1,024 outline points or more by stencil-then-cover; with OANIM_FAN_MIN_POINTS=0 in the
environment (the copies inherit it) it draws every colour fill so, which puts every suite through it.
Each runs in a private copy (its own LOCALAPPDATA, never the user's running app) driven over remote control.

Suites (--suites, default all):
  seams     abutting opaque shapes of one colour over a contrasting background, split along edges at several
            angles, as separate layers and as shapes of one layer; scored against the same outline drawn whole:
            any difference inside it is background showing through a seam.
  masks     nested, intersecting and inverted masks, a masked group, a mask on a layer inside a masked group,
            six groups deep each masked (Classic keeps four levels), masked layers side by side in a masked group,
            a card cutting only lines with one cutting both, Clip to Below onto a masked base, and shrunk, grown and
            Inside the Stroke rows.
  fills     big fills (what GPU Geometry draws by stencil-then-cover): a spiky star of 800 points (over 1,024 outline points as drawn), rings with holes
            (Even-Odd and Nonzero), outlines crossing themselves (a twist, and a curl covering its middle twice),
            big fills under masks, and see-through ones overlapping, with lines over them. Classic against GPU
            Geometry: a sample or two differs along edges, and fill_twist differs whole (Classic's triangulation
            drops one lobe of a figure-8; GPU Geometry fills where the outline winds, as the app's own inside test),
            and fill_see_through differs where its shapes overlap (GPU Geometry fades a see-through layer as a whole).
  temporal  a fill, a thin line and a hairline turned 0.15 degrees a frame for 40 frames; scored by flicker (pixels
            whose value goes up, down and up again by more than a little across three frames) per edge pixel.
  scenes    frames of real files (--scene FILE:F1,F2,...), compared between A and B.

Every case is rendered with render_png (the final renderer, the camera's frame) and, with --viewport, also captured
from the viewport (overlays off), which takes the viewport's own paths (layer caching, the pan margin).
--truth adds a ground truth: A at 4x the resolution, 16 samples, averaged down 4x4 (seam-free, smooth edges).

Output: OUT/<case>_{a,b,truth}.png, OUT/<case>_diff.png (differences x8), and OUT/summary.json, with a table on
stdout: pixels differing by more than --tolerance (of 255), the largest difference, the seam score (largest
difference from the whole outline inside it), flicker, and render times. Exit status 1 when A and B differ beyond
the tolerance in any case and --strict is given.
"""
import argparse
import base64
import io
import json
import os
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, ".claude", "skills", "match-vectors-to-reference", "scripts"))
import oa_remote  # noqa: E402

SIZE = (960, 540)


# --- Private copies --------------------------------------------------------------------------------------------------
class Copy:
    """A private copy of Open Animate: its own LOCALAPPDATA, remote control on a free port, stopped by its own PID."""

    def __init__(self, exe, work, settings):
        self.exe, self.work, self.settings = os.path.abspath(exe), work, settings
        os.makedirs(work, exist_ok=True)
        self.session = os.path.join(work, "private.json")
        if os.path.exists(self.session):
            os.remove(self.session)
        start = os.path.join(work, "start.py")
        with open(start, "w") as f:
            f.write("import json, os, _oanim\n"
                    "port = _oanim.remote_start(0)\n"
                    "json.dump({'pid': os.getpid(), 'port': port, 'token': _oanim.remote_token()}, "
                    "open(%r, 'w'))\n" % self.session)
        env = dict(os.environ, LOCALAPPDATA=os.path.join(work, "appdata"))
        os.makedirs(env["LOCALAPPDATA"], exist_ok=True)
        self.proc = subprocess.Popen([self.exe, "--python", start], env=env, cwd=work,
                                     stdout=open(os.path.join(work, "log.txt"), "w"), stderr=subprocess.STDOUT)
        for _ in range(240):
            if os.path.exists(self.session) and os.path.getsize(self.session) > 0:
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("%s did not start" % exe)
        time.sleep(0.5)

    def run(self, code):
        return oa_remote.run(self.session, code)

    def pump(self, n=3):
        for _ in range(n):
            oa_remote.call(self.session, "screenshot", max_size=16)

    def new_document(self, code):
        """A fresh document, the configuration's settings, then `code` to build the case."""
        self.run("import oanim\noanim.ops.wm.read_homefile(confirmed='1')")
        self.pump(3)
        setup = "import oanim\n" + "".join("oanim.wm.set_shading(%r, %r)\n" % (k, v) for k, v in self.settings.items())
        self.run(setup + code)
        self.pump(3)

    def open(self, path, frame):
        self.run("import oanim\noanim.ops.wm.open_mainfile(filepath=%r, confirmed='1')" % os.path.abspath(path))
        self.pump(5)
        setup = "import oanim\n" + "".join("oanim.wm.set_shading(%r, %r)\n" % (k, v) for k, v in self.settings.items())
        self.run(setup + "oanim.data.document.frame_current = %d" % frame)
        self.pump(3)

    def render(self, path, width=SIZE[0], height=SIZE[1], samples=None):
        code = "import oanim\n"
        if samples:
            code += "oanim.data.document.render_samples = %d\n" % samples
        code += "oanim.wm.render_png(%r, %d, %d)" % (os.path.abspath(path), width, height)
        if os.path.exists(path):
            os.remove(path)
        t = time.perf_counter()
        self.run(code)
        for _ in range(200):
            self.pump(1)
            if os.path.exists(path) and os.path.getsize(path) > 0:
                break
        dt = time.perf_counter() - t
        time.sleep(0.05)
        return np.asarray(Image.open(path).convert("RGBA")).astype(np.int32), dt

    def viewport(self):
        """The viewport as shown, overlays off (Shift Z), cropped to the scene."""
        self.run("import oanim\nif oanim.wm.overlays()['show']:\n    oanim.wm.set_overlay('show', False)")
        self.pump(6)
        r = oa_remote.call(self.session, "screenshot", max_size=4000)["result"]
        data = r.get("png") or r.get("image") or r.get("data")
        im = np.asarray(Image.open(io.BytesIO(base64.b64decode(data))).convert("RGBA")).astype(np.int32)
        # The default layout's viewport, well inside its header, toolbar and the panels around it.
        h, w = im.shape[:2]
        return im[int(h * 0.06):int(h * 0.74), int(w * 0.03):int(w * 0.72)]

    def stop(self):
        try:
            pid = json.load(open(self.session))["pid"]
            subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
        except Exception:
            self.proc.kill()


def parse_config(text):
    """EXE[:name=0/1,...] (the path may itself have a drive letter's colon: settings come after ".exe")."""
    i = text.lower().find(".exe")
    if i < 0:
        raise SystemExit("not an .exe: %s" % text)
    exe, rest = text[:i + 4], text[i + 5:]
    settings = {}
    for item in filter(None, rest.split(",")):
        k, _, v = item.partition("=")
        settings[k] = v not in ("0", "false", "False", "off")
    return exe, settings


# --- Cases -----------------------------------------------------------------------------------------------------------
SETUP = r'''
import math
doc = oanim.data.document
doc.render_samples = 16
def layer(name):
    oanim.ops.layer.new()
    L = oanim.data.layers.active
    L.name = name
    return L
def poly(L, pts, color=(0.95, 0.85, 0.2, 1.0), line=None, width=0.0, sharp=True):
    L.add_curve(pts, closed=True, fill=True)
    i = len(L.shapes) - 1
    if sharp:
        # Straight sides: every handle on its point.
        c = L.curve_count - 1
        for k, p in enumerate(pts):
            L.place_handle(c, k, "IN", p, keep_other=True)
            L.place_handle(c, k, "OUT", p, keep_other=True)
    style = {"fill": color, "has_outline": line is not None}
    if line is not None:
        style.update(line=line, line_width=width)
    L.set_shape_style(i, **style)
    return i
# A dark backdrop under everything (what shows through a seam).
poly(layer("Backdrop"), [(-4, -4), (4, -4), (4, 4), (-4, 4)], (0.05, 0.05, 0.05, 1.0))
'''


def seam_cases():
    """(name, split code, whole code): the same outline split along an edge, and drawn whole."""
    cases = []
    for angle in (0.0, 7.0, 33.0, 45.0):
        a = angle * 3.14159265 / 180.0
        cases.append(("seam_layers_%g" % angle, a, "layers"))
        cases.append(("seam_shapes_%g" % angle, a, "shapes"))
    out = []
    for name, a, kind in cases:
        split = SETUP + r'''
a = %r
c, s = math.cos(a), math.sin(a)
def rot(p): return (p[0] * c - p[1] * s, p[0] * s + p[1] * c)
left = [rot(p) for p in [(-0.71, -0.53), (0.013, -0.53), (0.013, 0.47), (-0.71, 0.47)]]
right = [rot(p) for p in [(0.013, -0.53), (0.69, -0.53), (0.69, 0.47), (0.013, 0.47)]]
if %r == "layers":
    poly(layer("Left"), left)
    poly(layer("Right"), right)
else:
    L = layer("Both")
    poly(L, left)
    poly(L, right)
''' % (a, kind)
        whole = SETUP + r'''
a = %r
c, s = math.cos(a), math.sin(a)
def rot(p): return (p[0] * c - p[1] * s, p[0] * s + p[1] * c)
poly(layer("Whole"), [rot(p) for p in [(-0.71, -0.53), (0.69, -0.53), (0.69, 0.47), (-0.71, 0.47)]])
''' % a
        out.append((name, split, whole))
    # Many abutting strips across many layers (cache boundaries in the viewport).
    split = SETUP + r'''
xs = [-0.8 + k * 0.0688 for k in range(25)]
for k in range(24):
    x0, x1 = xs[k], xs[k + 1]
    poly(layer("Strip%d" % k), [(x0, -0.5), (x1, -0.5), (x1 + 0.05, 0.5), (x0 + 0.05, 0.5)])
'''
    whole = SETUP + r'''
xs = [-0.8 + k * 0.0688 for k in range(25)]
x0, x1 = xs[0], xs[24]
poly(layer("Whole"), [(x0, -0.5), (x1, -0.5), (x1 + 0.05, 0.5), (x0 + 0.05, 0.5)])
'''
    out.append(("seam_strips", split, whole))
    return out


def mask_cases():
    base = SETUP + r'''
def circle(L, cx, cy, r, color, n=48):
    return poly(L, [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)], color, sharp=False)
'''
    return [
        ("mask_simple", base + r'''
m = layer("Mask"); circle(m, 0.0, 0.0, 0.45, (1, 1, 1, 1))
art = layer("Art"); poly(art, [(-0.8, -0.6), (0.8, -0.6), (0.8, 0.6), (-0.8, 0.6)], (0.2, 0.6, 0.95, 1))
oanim.ops.mask.add(layer=str(art.id), sources=str(m.id), mode="ADD")
m.visible = False
'''),
        ("mask_intersect", base + r'''
m1 = layer("M1"); circle(m1, -0.15, 0.0, 0.42, (1, 1, 1, 1))
m2 = layer("M2"); circle(m2, 0.18, 0.05, 0.38, (1, 1, 1, 1))
art = layer("Art"); poly(art, [(-0.8, -0.6), (0.8, -0.6), (0.8, 0.6), (-0.8, 0.6)], (0.9, 0.3, 0.4, 1))
oanim.ops.mask.add(layer=str(art.id), sources=str(m1.id), mode="ADD")
oanim.ops.mask.add(layer=str(art.id), sources=str(m2.id), mode="ADD")
m1.visible = False; m2.visible = False
'''),
        ("mask_invert_subtract", base + r'''
m = layer("Hole"); circle(m, 0.1, -0.05, 0.3, (1, 1, 1, 1))
art = layer("Art"); poly(art, [(-0.8, -0.6), (0.8, -0.6), (0.8, 0.6), (-0.8, 0.6)], (0.3, 0.9, 0.5, 1))
oanim.ops.mask.add(layer=str(art.id), sources=str(m.id), mode="SUBTRACT")
m.visible = False
'''),
        ("mask_nested_group", base + r'''
m = layer("Outer"); circle(m, 0.0, 0.0, 0.5, (1, 1, 1, 1))
m2 = layer("Inner"); circle(m2, 0.2, 0.1, 0.3, (1, 1, 1, 1))
a = layer("A"); poly(a, [(-0.8, -0.6), (0.3, -0.6), (0.3, 0.6), (-0.8, 0.6)], (0.95, 0.6, 0.2, 1))
b = layer("B"); poly(b, [(0.3, -0.6), (0.8, -0.6), (0.8, 0.6), (0.3, 0.6)], (0.95, 0.6, 0.2, 1))
oanim.ops.mask.add(layer=str(a.id), sources=str(m.id), mode="ADD")
oanim.ops.mask.add(layer=str(b.id), sources=str(m.id), mode="ADD")
oanim.ops.mask.add(layer=str(b.id), sources=str(m2.id), mode="SUBTRACT")
m.visible = False; m2.visible = False
'''),
        ("mask_deep_groups", base + r'''
art = layer("Art"); poly(art, [(-0.8, -0.6), (0.8, -0.6), (0.8, 0.6), (-0.8, 0.6)], (0.6, 0.85, 0.95, 1))
masks = []
for k in range(6):
    # Each cuts its own side, so every level shows.
    m = layer("M%d" % k); circle(m, 0.18 * math.cos(k * math.pi / 3), 0.18 * math.sin(k * math.pi / 3), 0.42, (1, 1, 1, 1))
    m.visible = False
    masks.append(m)
inner = art
for k in range(6):
    oanim.data.layers.active = inner
    oanim.ops.layer.group()
    g = oanim.data.layers.active
    g.name = "G%d" % k
    oanim.ops.mask.add(layer=str(g.id), sources=str(masks[k].id), mode="ADD")
    inner = g
'''),
        ("mask_siblings", base + r'''
outer = layer("Outer"); circle(outer, 0.0, 0.0, 0.5, (1, 1, 1, 1)); outer.visible = False
kids = []
for k in range(4):
    a = layer("Kid%d" % k)
    poly(a, [(-0.7 + 0.35 * k, -0.6), (-0.3 + 0.35 * k, -0.6), (-0.3 + 0.35 * k, 0.6), (-0.7 + 0.35 * k, 0.6)],
         (0.95, 0.3 + 0.15 * k, 0.4, 1))
    h = layer("Hole%d" % k); circle(h, -0.5 + 0.35 * k, 0.1 * k - 0.15, 0.12, (1, 1, 1, 1)); h.visible = False
    oanim.ops.mask.add(layer=str(a.id), sources=str(h.id), mode="SUBTRACT")
    kids.append(a)
oanim.data.layers.active = kids[0]
for a in kids[1:]:
    a.select = True
oanim.ops.layer.group()
g = oanim.data.layers.active
oanim.ops.mask.add(layer=str(g.id), sources=str(outer.id), mode="ADD")
'''),
        ("mask_lines_only", base + r'''
m = layer("Band"); poly(m, [(-0.2, -0.7), (0.15, -0.7), (0.15, 0.7), (-0.2, 0.7)], (1, 1, 1, 1)); m.visible = False
c = layer("Round"); circle(c, 0.0, 0.0, 0.45, (1, 1, 1, 1)); c.visible = False
art = layer("Art")
poly(art, [(-0.6, -0.4), (0.6, -0.4), (0.6, 0.4), (-0.6, 0.4)], (0.95, 0.8, 0.3, 1), line=(0.1, 0.1, 0.6, 1), width=0.04)
oanim.ops.mask.add(layer=str(art.id), sources=str(m.id), mode="SUBTRACT", cuts="LINES")
oanim.ops.mask.add(layer=str(art.id), sources=str(c.id), mode="ADD")
'''),
        ("mask_clip_base", base + r'''
hole = layer("Hole"); circle(hole, 0.1, 0.0, 0.2, (1, 1, 1, 1)); hole.visible = False
base_l = layer("Base"); circle(base_l, 0.0, 0.0, 0.45, (0.3, 0.3, 0.9, 1))
oanim.ops.mask.add(layer=str(base_l.id), sources=str(hole.id), mode="SUBTRACT")
top = layer("Top"); poly(top, [(-0.8, -0.1), (0.8, -0.1), (0.8, 0.5), (-0.8, 0.5)], (0.95, 0.5, 0.2, 1))
oanim.data.layers.active = top
oanim.ops.mask.clip_to_below(set="1")
'''),
        ("mask_grow_shrink", base + r'''
m = layer("Shape"); poly(m, [(-0.3, -0.3), (0.3, -0.35), (0.35, 0.3), (-0.25, 0.32)], (1, 1, 1, 1), line=(1, 1, 1, 1), width=0.05)
m.visible = False
a = layer("Shrunk"); poly(a, [(-0.8, -0.6), (0.0, -0.6), (0.0, 0.6), (-0.8, 0.6)], (0.4, 0.9, 0.5, 1))
oanim.ops.mask.add(layer=str(a.id), sources=str(m.id), mode="ADD", grow="-8")
b = layer("Grown"); poly(b, [(0.0, -0.6), (0.8, -0.6), (0.8, 0.6), (0.0, 0.6)], (0.9, 0.4, 0.5, 1))
oanim.ops.mask.add(layer=str(b.id), sources=str(m.id), mode="ADD", grow="10")
i = layer("Inside"); poly(i, [(-0.8, -0.1), (0.8, -0.1), (0.8, 0.1), (-0.8, 0.1)], (0.95, 0.95, 0.3, 1))
oanim.ops.mask.add(layer=str(i.id), sources=str(m.id), mode="ADD", shape="FILLS_ONLY", inside_stroke="1")
'''),
    ]


def fill_cases():
    base = SETUP + r'''
def ring_points(cx, cy, r, n=64, turns=1.0, grow=0.0):
    return [(cx + (r + grow * i / n) * math.cos(2 * math.pi * turns * i / n),
             cy + (r + grow * i / n) * math.sin(2 * math.pi * turns * i / n)) for i in range(n)]
def star(cx, cy, r0, r1, tips):
    return [(cx + (r1 if i % 2 else r0) * math.cos(math.pi * i / tips),
             cy + (r1 if i % 2 else r0) * math.sin(math.pi * i / tips)) for i in range(2 * tips)]
def joined(L, loops, color, rule):
    """Several loops as one shape (F), filling by `rule`."""
    first = L.point_count
    for loop in loops:
        poly(L, loop, color)
    L.select_points(range(first, L.point_count))
    oanim.ops.mesh.make_segment()
    L.set_shape_style(L.shape_count - 1, fill_rule=rule)
'''
    return [
        ("fill_spiky", base + r'''
poly(layer("Star"), star(0.0, 0.0, 0.2, 0.45, 400), (0.95, 0.75, 0.3, 1))
'''),
        ("fill_ring_evenodd", base + r'''
joined(layer("Ring"), [ring_points(0, 0, 0.45, 96), ring_points(0.05, 0, 0.22, 96)], (0.4, 0.8, 0.95, 1), "EVEN_ODD")
'''),
        ("fill_ring_nonzero", base + r'''
outer = ring_points(0, 0, 0.45, 96)
joined(layer("Ring"), [outer, list(reversed(ring_points(0.05, 0, 0.22, 96))), ring_points(-0.5, 0.1, 0.12, 48)],
       (0.4, 0.95, 0.5, 1), "NONZERO")
'''),
        ("fill_star_holes", base + r'''
joined(layer("Holes"), [star(0, 0, 0.3, 0.48, 160), ring_points(-0.1, 0, 0.12, 80), ring_points(0.14, 0.05, 0.08, 80)],
       (0.95, 0.5, 0.6, 1), "EVEN_ODD")
'''),
        ("fill_twist", base + r'''
n = 300
poly(layer("Twist"), [(0.6 * math.sin(2 * math.pi * i / n), 0.35 * math.sin(4 * math.pi * i / n)) for i in range(n)],
     (0.8, 0.6, 0.95, 1))
'''),
        ("fill_curl", base + r'''
poly(layer("Curl"), ring_points(0, 0, 0.25, 400, turns=2.0, grow=0.2), (0.95, 0.9, 0.4, 1))
'''),
        ("fill_masked", base + r'''
m = layer("Mask"); poly(m, ring_points(0.1, 0.0, 0.35, 64), (1, 1, 1, 1), sharp=False)
a = layer("Star"); poly(a, star(0.0, 0.0, 0.25, 0.5, 300), (0.3, 0.6, 0.95, 1))
b = layer("Under"); poly(b, star(-0.3, 0.1, 0.1, 0.3, 200), (0.95, 0.4, 0.3, 1))
oanim.ops.mask.add(layer=str(a.id), sources=str(m.id), mode="ADD")
oanim.ops.mask.add(layer=str(b.id), sources=str(m.id), mode="SUBTRACT")
m.visible = False
'''),
        ("fill_translucent", base + r'''
L = layer("Both")
poly(L, star(-0.15, 0.0, 0.2, 0.4, 200), (0.95, 0.3, 0.3, 0.5), line=(0.1, 0.1, 0.1, 1), width=0.01)
poly(L, ring_points(0.15, 0.05, 0.3, 200), (0.3, 0.5, 0.95, 0.5))
o = layer("Over"); poly(o, star(0.25, -0.1, 0.1, 0.25, 150), (0.3, 0.95, 0.4, 0.6))
o.opacity = 0.7
'''),
        ("fill_see_through", base + r'''
under = layer("Under"); poly(under, [(-0.7, -0.15), (0.7, -0.15), (0.7, 0.15), (-0.7, 0.15)], (0.2, 0.9, 0.4, 1))
L = layer("SeeThrough")
poly(L, star(-0.12, 0.0, 0.18, 0.36, 40), (0.95, 0.35, 0.3, 1), line=(0.1, 0.1, 0.5, 1), width=0.03)
poly(L, ring_points(0.15, 0.05, 0.28, 64), (0.3, 0.5, 0.95, 1), line=(0.95, 0.95, 0.95, 1), width=0.03)
L.opacity = 0.5
'''),
    ]


TEMPORAL = SETUP + r'''
fill = layer("Fill"); poly(fill, [(-0.6, -0.35), (-0.1, -0.4), (-0.15, 0.35), (-0.62, 0.3)], (0.9, 0.9, 0.9, 1))
line = layer("Line"); line.add_curve([(0.05, -0.4), (0.3, 0.1), (0.6, 0.4)])
line.set_shape_style(0, line=(0.9, 0.9, 0.9, 1), line_width=0.012, has_outline=True)
hair = layer("Hairline"); hair.add_curve([(0.05, 0.4), (0.35, -0.05), (0.62, -0.4)])
hair.set_shape_style(0, line=(0.9, 0.9, 0.9, 1), line_width=0.0015, has_outline=True)
'''


# --- Measures --------------------------------------------------------------------------------------------------------
def diff(a, b):
    d = np.abs(a - b).max(axis=2)
    return d


def seam_score(split, whole):
    """Largest difference from the whole outline where the whole one shows its own colour, 2 px in from its edge
    (the backdrop under it is opaque too, so inside is told by colour)."""
    colors, counts = np.unique(whole[:, :, :3].reshape(-1, 3), axis=0, return_counts=True)
    order = np.argsort(-counts)
    backdrop, shape = colors[order[0]], colors[order[1]]
    if np.abs(shape - backdrop).max() < 30 and len(order) > 2:
        shape = colors[order[2]]
    inside = np.abs(whole[:, :, :3] - shape).max(axis=2) == 0
    inner = inside.copy()
    for dy in (-2, -1, 0, 1, 2):
        for dx in (-2, -1, 0, 1, 2):
            inner &= np.roll(np.roll(inside, dy, 0), dx, 1)
    d = np.abs(split - whole).max(axis=2)
    return int(d[inner].max()) if inner.any() else 0


def flicker(frames):
    """Pixels whose value goes up, down and up again (or the reverse) by more than 6 across three frames, per edge
    pixel (one partly covered in any frame)."""
    v = np.stack([f[:, :, :3].mean(axis=2) for f in frames])
    edge = ((v > 8) & (v < 247)).any(axis=0)
    d = np.diff(v, axis=0)
    flips = ((d[:-1] > 6) & (d[1:] < -6)) | ((d[:-1] < -6) & (d[1:] > 6))
    return float(flips.sum(axis=0)[edge].mean()) if edge.any() else 0.0


def save(path, im):
    Image.fromarray(np.clip(im, 0, 255).astype(np.uint8)).save(path)


# --- Main ------------------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True, help="EXE[:setting=0/1,...]")
    ap.add_argument("--b", help="EXE[:setting=0/1,...] (omit: A alone, scored against truth and itself)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "render_check"))
    ap.add_argument("--suites", default="seams,masks,fills,temporal,scenes")
    ap.add_argument("--scene", action="append", default=[], help="FILE.oanim:F1,F2,...")
    ap.add_argument("--truth", action="store_true")
    ap.add_argument("--viewport", action="store_true")
    ap.add_argument("--tolerance", type=int, default=2)
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    suites = set(args.suites.split(","))

    configs = [("a", parse_config(args.a))] + ([("b", parse_config(args.b))] if args.b else [])
    copies = {k: Copy(exe, os.path.join(args.out, "work_" + k), st) for k, (exe, st) in configs}
    rows, failed = [], False

    def compare(name, images, times, extra=None):
        nonlocal failed
        row = {"case": name, "wall_ms": {k: round(t * 1000, 1) for k, t in times.items()}}
        for k, im in images.items():
            save(os.path.join(args.out, "%s_%s.png" % (name, k)), im)
        if "b" in images:
            d = diff(images["a"], images["b"])
            save(os.path.join(args.out, "%s_diff.png" % name), np.repeat(np.minimum(d * 8, 255)[:, :, None], 3, 2))
            row["differing"] = int((d > args.tolerance).sum())
            row["largest"] = int(d.max())
            failed |= row["differing"] > 0
        if "truth" in images:
            row["vs_truth_largest"] = {k: int(diff(images[k], images["truth"]).max()) for k in images if k != "truth"}
        row.update(extra or {})
        rows.append(row)
        print(json.dumps(row))
        sys.stdout.flush()

    def truth_render(c, path):
        big, _ = c.render(path + "_big.png", SIZE[0] * 4, SIZE[1] * 4, samples=16)
        return big.reshape(SIZE[1], 4, SIZE[0], 4, 4).mean(axis=(1, 3)).round().astype(np.int32)

    try:
        if "seams" in suites:
            for name, split, whole in seam_cases():
                images, times, scores = {}, {}, {}
                for k, c in copies.items():
                    c.new_document(whole)
                    w, _ = c.render(os.path.join(args.out, "%s_%s_whole.png" % (name, k)))
                    c.new_document(split)
                    images[k], times[k] = c.render(os.path.join(args.out, "%s_%s_raw.png" % (name, k)))
                    scores["seam_" + k] = seam_score(images[k], w)
                    if args.viewport:
                        vs = c.viewport()
                        c.new_document(whole)
                        vw = c.viewport()
                        scores["viewport_seam_" + k] = seam_score(vs, vw)
                if args.truth:
                    copies["a"].new_document(split)
                    images["truth"] = truth_render(copies["a"], os.path.join(args.out, name + "_truth"))
                compare(name, images, times, scores)
        if "masks" in suites:
            for name, code in mask_cases():
                images, times = {}, {}
                for k, c in copies.items():
                    c.new_document(code)
                    images[k], times[k] = c.render(os.path.join(args.out, "%s_%s_raw.png" % (name, k)))
                if args.truth:
                    copies["a"].new_document(code)
                    images["truth"] = truth_render(copies["a"], os.path.join(args.out, name + "_truth"))
                compare(name, images, times)
        if "fills" in suites:
            for name, code in fill_cases():
                images, times = {}, {}
                for k, c in copies.items():
                    c.new_document(code)
                    images[k], times[k] = c.render(os.path.join(args.out, "%s_%s_raw.png" % (name, k)))
                    if args.viewport:
                        images["viewport_" + k] = c.viewport()
                if args.truth:
                    copies["a"].new_document(code)
                    images["truth"] = truth_render(copies["a"], os.path.join(args.out, name + "_truth"))
                extra = {}
                if "viewport_a" in images and "viewport_b" in images:
                    extra["viewport_differing"] = int((diff(images["viewport_a"], images["viewport_b"]) > args.tolerance).sum())
                compare(name, {k: v for k, v in images.items() if not k.startswith("viewport_")}, times, extra)
        if "temporal" in suites:
            for k, c in copies.items():
                c.new_document(TEMPORAL)
            frames = {k: [] for k in copies}
            for i in range(40):
                for k, c in copies.items():
                    c.run("import oanim\nfor n in ('Fill', 'Line', 'Hairline'):\n"
                          "    [l for l in oanim.data.layers.all() if l.name == n][0].rotation = %r" % (i * 0.15))
                    im, _ = c.render(os.path.join(args.out, "temporal_%s_%02d.png" % (k, i)))
                    frames[k].append(im)
            extra = {"flicker_" + k: round(flicker(f), 4) for k, f in frames.items()}
            compare("temporal_last", {k: f[-1] for k, f in frames.items()}, {k: 0 for k in frames}, extra)
        if "scenes" in suites:
            for spec in args.scene:
                path, _, frames = spec.rpartition(":")
                for frame in [int(f) for f in frames.split(",")]:
                    name = "%s_f%d" % (os.path.splitext(os.path.basename(path))[0], frame)
                    images, times = {}, {}
                    for k, c in copies.items():
                        c.open(path, frame)
                        images[k], times[k] = c.render(os.path.join(args.out, "%s_%s_raw.png" % (name, k)))
                    compare(name, images, times)
    finally:
        for c in copies.values():
            c.stop()
    json.dump(rows, open(os.path.join(args.out, "summary.json"), "w"), indent=1)
    print("\n%-26s %10s %8s %s" % ("case", "differing", "largest", "other"))
    for r in rows:
        other = {k: v for k, v in r.items() if k not in ("case", "differing", "largest")}
        print("%-26s %10s %8s %s" % (r["case"], r.get("differing", "-"), r.get("largest", "-"), other))
    sys.exit(1 if (failed and args.strict) else 0)


if __name__ == "__main__":
    main()
