"""Matching a character's vectors to a reference picture, one tool with subcommands.

  snapshot SESSION WORK [--char Clay] [--ref HeadTurn] [--size 1920x1080] [--skip Hair_X] [--frame N]
      Renders WORK/clay.png (the character alone, transparent) and WORK/ref.png (the reference alone, the
      character hidden, full opacity), and writes WORK/state.json: every layer under the character with its
      path, masks in words, shapes and where its points and handles show (document space), plus
      WORK/mapping.json (document <-> pixel). Layers whose path has a --skip part are left out (default
      Hair_X: the user doesn't want the hair changed). Visibility and opacity are put back after, and with
      --frame the app's frame too (it renders frame N).
  solo SESSION WORK NAME PATH... [--size ...]
      Renders WORK/NAME.png with only those layers of the character showing (and the groups they're in),
      to see one part's look or to score it. Masks still cut as in the full render: a hidden layer still
      masks (a solo Ear_R shows only the sliver outside Head_Base_X).
  wires WORK PATH... [--on ref|clay|both] [--out FILE] [--pad 40] [--zoom 2] [--curves 0,2] [--level K]
      [--target NAME] [--edits FILE]
      Draws those layers' curves as they show, handles and point numbers (curve:index, then the point's
      number, then L1 for a detail point), over the reference, cropped around them and zoomed: what to
      look at before and after a fit. --level K: the curves as detail level K has them (its points only).
  region WORK X,Y [X,Y ...] NAME [--tol 28] [--image ref] [--open 2] [--close 3] [--grow 0]
      Flood-fills the reference from those pixels (in WORK's image pixels) and keeps the region's outline as
      WORK/NAME.json (document space) and WORK/NAME.png (a preview). A painted reference's dark lines stop
      the fill, so a seed in the skin gives the skin's shape.
  stroke WORK X0,Y0,X1,Y1 NAME [--dark 90] [--start X,Y] [--not REGION]
      A drawn line's middle (the biggest run of pixels darker than --dark in that pixel box), followed from
      --start (or its far end) to the other end, as an open target WORK/NAME.json and a preview NAME.png.
      --not leaves out a region traced before (WORK/REGION_mask.png), such as the nose over a mouth line.
  fit WORK PATH CURVE TARGET [--range A:B] [--part X0,Y0,X1,Y1] [--drop X0,Y0,X1,Y1[,R]] [--fixed 3,4] [--handles-only]
      [--join-fixed] (an open target runs on straight to the range's held ends: hidden points stay put)
      [--pin I=X,Y] [--corner I] [--level K] [--out FILE]
      Moves curve CURVE's points (index range A:B, inclusive, wrapping on a closed curve) onto the target
      outline (TARGET: a region name or a JSON list of [x, y] in document space) in order, then fits each
      segment's two handles to the outline between its points, a smooth point's handles kept in line.
      --part keeps only the outline inside that pixel box. Writes edits (default
      WORK/edits_<layer>_c<curve>[_L<level>].json) and prints the error before and after, in pixels.
      A layer with detail levels is fitted one level at a time (--level, default 0): level 0 moves its own
      points (the detail rides along); level K moves only level-K points, the coarser ones held, and
      smooths a detail point onto the coarser curve where that is within --smooth-under px (3).
  follow WORK EDITS PATH [POINT | CURVE:INDEX ... | all] [--by P,P] [--out FILE]
      Edits that move PATH's points (those listed, else all the edits didn't move) as the edits moved theirs
      (or just points --by): the best move, turn and even scale, handles too. For what keeps its shape with
      a fitted part: an ear's part behind the head and its inner line, an eye's iris, pupil and lid.
  ellipse WORK PATH CURVE REGION [--scale F] [--size-from BASE_WORK] [--out FILE]
      Makes a closed curve a true ellipse fitted to REGION's pixels: its points evenly round it, each keeping
      its place, with exact-arc handles. Eyes, irises, pupils and shines: a point-by-point fit to a pixel
      outline leaves ovals lumpy. --scale: that ellipse made bigger or smaller (a pupil inside its iris).
      --size-from (a frame-0 snapshot): keep the part's designed size, the upright axis as it was there and
      the level one no longer (eyes don't grow; a turn only narrows them); the reference gives place and tilt.
  lid WORK LID_LAYER WHITE_REGION NAME [--dark 80] [--band 9]
      A lid target NAME: the reference's dark lid line measured round the white's ellipse (thickness at
      each angle from truly dark pixels, not the halo), as a crescent whose drawn line (the lid layer's line
      width) touches the white without covering it. Then fit the lid to NAME.
  carry WORK EDITS BASE_WORK PATH... [--shift-to REGION --shift-by PATH]
      Carries PATHs as drawn in BASE_WORK (a snapshot of another frame: frame 0, the rig's design) by the
      stretch, turn and move taking the edits' layer there to where the edits put it: an eye's lid, iris,
      pupil and shine with its fitted white. Then, with --shift-to, all moved so PATH sits in REGION's
      middle (the iris where the reference's pupil is). Writes WORK/carry_<layer>.json each.
  restore WORK BASE_WORK PATH...
      Edits putting PATHs back as the snapshot BASE_WORK had them: undoes one part without undoing later
      steps for others. Writes WORK/restore_<layer>.json each.
  apply SESSION EDITS... [--label "Match Vectors"] [--set-frame]
      Places the edits' points and handles as ONE undo step in that app (keyed at the current frame),
      coarsest level first, each at its own edit level so finer detail rides along; edit levels put back.
  score WORK NAME TARGET
      How well WORK/NAME.png's (a solo render's) shape covers the target region: IoU and the outline's
      mean and worst distance, in pixels.
"""

import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oa_remote  # noqa: E402

# --- In-app code ---------------------------------------------------------------------------------------

LAYER_PATHS = """
import oanim, json
def _path(l):
    names = []
    while l is not None:
        names.append(l.name)
        l = l.parent
    return "/".join(reversed(names))
def _under(root):
    out = []
    def walk(l):
        out.append(l)
        for c in l.children:
            walk(c)
    walk(root)
    return out
def _by_path(path):
    found = [l for l in oanim.data.layers.all() if _path(l) == path or _path(l).endswith("/" + path) or l.name == path]
    exact = [l for l in found if _path(l) == path]
    if exact:
        found = exact
    if len(found) != 1:
        raise RuntimeError("%d layers match %r: give more of the path (%s)" % (len(found), path, [_path(l) for l in found]))
    return found[0]
"""


def snapshot_code(char, ref, size, skip, clay_png, ref_png, state_json):
    return LAYER_PATHS + f"""
doc = oanim.data.document
char = _by_path({char!r})
ref = _by_path({ref!r})
layers = []
for l in _under(char):
    p = _path(l)
    if any(s in p.split("/") for s in {skip!r}):
        continue
    e = {{"path": p, "id": l.id, "type": l.type, "visible": l.visible}}
    try:
        e["masks"] = l.mask_summary
        e["masking"] = l.masking
    except Exception:
        pass
    if l.type == "VECTOR":
        e["reference_of"] = (lambda s: _path(s) if s is not None else None)(l.reference_source)
        try:
            e["shapes"] = [(name, curves, l.shape_style(i)) for i, (name, curves) in enumerate(l.shapes)]
            points, curves = l.shown()
            e["points"] = points
            e["curves"] = curves
            e["detail"] = l.detail
            top = max(e["detail"]["levels"] or [0])
            if top > 0:  # the curves as each detail level has them, coarsest first
                e["level_curves"] = {{str(k): l.shown(level=k)[1] for k in range(top + 1)}}
        except Exception as error:
            e["error"] = str(error)
    layers.append(e)
cam = oanim.data.camera
a, b, c = cam.to_frame((0.0, 0.0)), cam.to_frame((1.0, 0.0)), cam.to_frame((0.0, 1.0))
state = {{"frame": doc.frame_current, "action": doc.action, "doc": [doc.width, doc.height], "size": {list(size)!r},
          "frame_affine": [a, b, c], "layers": layers}}
open({state_json!r}, "w").write(json.dumps(state))
# The character alone: everything else hidden for the render, then put back.
top = [l for l in oanim.data.layers.all() if l.parent is None]
was = {{l.id: l.visible for l in top}}
for l in top:
    l.visible = l.id == char.id
oanim.wm.render_png({clay_png!r}, {size[0]}, {size[1]}, transparent=True)
"""


def restore_and_ref_code(char, ref, size, ref_png):
    return LAYER_PATHS + f"""
char = _by_path({char!r})
ref = _by_path({ref!r})
top = [l for l in oanim.data.layers.all() if l.parent is None]
chain = []
l = ref
while l is not None:
    chain.append(l.id)
    l = l.parent
for l in top:
    l.visible = l.id in chain
opacity = ref.opacity
ref.opacity = 1.0
oanim.wm.render_png({ref_png!r}, {size[0]}, {size[1]}, guides=True)
print(json.dumps({{"opacity": opacity}}))
"""


def put_back_code(visible, ref, opacity):
    return LAYER_PATHS + f"""
for l in oanim.data.layers.all():
    if l.id in {visible!r}:
        l.visible = {visible!r}[l.id]
_by_path({ref!r}).opacity = {opacity!r}
"""


def top_visibility_code():
    return LAYER_PATHS + """
print(json.dumps({str(l.id): l.visible for l in oanim.data.layers.all() if l.parent is None}))
"""


# --- Document <-> pixel ----------------------------------------------------------------------------------


class Mapping:
    """Document space to a render's pixels: pixel = (w/2 + s fx, h/2 - s fy), s = h/2, with (fx, fy) the
    camera frame's coordinates (an affine function of the document point)."""

    def __init__(self, state):
        (ax, ay), (bx, by), (cx, cy) = state["frame_affine"]
        w, h = state["size"]
        s = h / 2.0
        # frame = F @ [x, y, 1]
        F = np.array([[bx - ax, cx - ax, ax], [by - ay, cy - ay, ay], [0, 0, 1.0]])
        P = np.array([[s, 0, w / 2.0], [0, -s, h / 2.0], [0, 0, 1.0]])
        self.to_px_m = P @ F
        self.to_doc_m = np.linalg.inv(self.to_px_m)
        self.px_per_unit = math.sqrt(abs(np.linalg.det(self.to_px_m[:2, :2])))

    def to_px(self, pts):
        a = np.asarray(pts, float).reshape(-1, 2)
        return (a @ self.to_px_m[:2, :2].T) + self.to_px_m[:2, 2]

    def to_doc(self, pts):
        a = np.asarray(pts, float).reshape(-1, 2)
        return (a @ self.to_doc_m[:2, :2].T) + self.to_doc_m[:2, 2]


def load_state(work):
    with open(os.path.join(work, "state.json")) as f:
        return json.load(f)


def find_layer(state, path):
    found = [l for l in state["layers"] if l["path"] == path or l["path"].endswith("/" + path)]
    if len(found) > 1:  # a group and the drawing in it of the same name: the drawing
        found = [l for l in found if l["path"] == path] or [l for l in found if l["type"] == "VECTOR"] or found
    if len(found) != 1:
        sys.exit("%d layers match %r: %s" % (len(found), path, [l["path"] for l in found]))
    return found[0]


# --- Béziers -------------------------------------------------------------------------------------------


def bez(p0, p1, p2, p3, t):
    t = np.asarray(t, float)[:, None]
    u = 1 - t
    return u ** 3 * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t ** 3 * p3


def bez_d(p0, p1, p2, p3, t):
    t = np.asarray(t, float)[:, None]
    u = 1 - t
    return 3 * u * u * (p1 - p0) + 6 * u * t * (p2 - p1) + 3 * t * t * (p3 - p2)


def bez_dd(p0, p1, p2, p3, t):
    t = np.asarray(t, float)[:, None]
    return 6 * (1 - t) * (p2 - 2 * p1 + p0) + 6 * t * (p3 - 2 * p2 + p1)


def segments(curve, closed):
    """[(i, j)] curve-local indices of each segment's start and end."""
    n = len(curve)
    pairs = [(i, i + 1) for i in range(n - 1)]
    if closed and n > 1:
        pairs.append((n - 1, 0))
    return pairs


def sample_curve(curve, closed, steps=24, only=None):
    out = []
    for i, j in segments(curve, closed):
        if only is not None and (i not in only or j not in only):
            continue
        p0, p1 = np.array(curve[i][0]), np.array(curve[i][2])
        p2, p3 = np.array(curve[j][1]), np.array(curve[j][0])
        out.append(bez(p0, p1, p2, p3, np.linspace(0, 1, steps)))
    return np.concatenate(out) if out else np.zeros((0, 2))


def shown_curve(layer, index, level=None):
    """[(pos, in, out, aligned, hidden, point, index in the whole curve, detail level)] for one curve of a
    layer in state.json: as drawn (level None), or as that detail level's curve has it (its points up to
    that level, with that level's handles)."""
    if level is not None and layer.get("level_curves"):
        top = len(layer["level_curves"]) - 1
        cps, closed = layer["level_curves"][str(min(level, top))][index]
    else:
        cps, closed = layer["curves"][index]
    pts = layer["points"]
    return [(tuple(pts[c[0]]), tuple(c[1]), tuple(c[2]), c[3], c[4], c[0], c[6], c[5]) for c in cps], closed


def detail_top(layer):
    """The layer's highest detail level (0: no detail)."""
    return len(layer["level_curves"]) - 1 if layer.get("level_curves") else 0


# --- Polylines ---------------------------------------------------------------------------------------


def arclen(poly):
    d = np.linalg.norm(np.diff(poly, axis=0), axis=1)
    return np.concatenate([[0], np.cumsum(d)])


def at_length(poly, s, cum):
    s = np.clip(s, 0, cum[-1])
    k = np.clip(np.searchsorted(cum, s, side="right") - 1, 0, len(poly) - 2)
    seg = cum[k + 1] - cum[k]
    t = np.where(seg > 0, (s - cum[k]) / np.where(seg > 0, seg, 1), 0)
    return poly[k] + (poly[k + 1] - poly[k]) * t[..., None] if np.ndim(s) else poly[k] + (poly[k + 1] - poly[k]) * t


def project(poly, cum, p):
    """Arc length of the polyline's nearest place to p, and the distance."""
    a, b = poly[:-1], poly[1:]
    ab = b - a
    l2 = np.maximum((ab ** 2).sum(1), 1e-12)
    t = np.clip(((p - a) * ab).sum(1) / l2, 0, 1)
    q = a + ab * t[:, None]
    d = np.linalg.norm(q - p, axis=1)
    k = int(np.argmin(d))
    return cum[k] + t[k] * math.sqrt(l2[k]), float(d[k])


def dist_to_poly(points, poly):
    """Each point's distance to the polyline."""
    a, b = poly[:-1], poly[1:]
    ab = b - a
    l2 = np.maximum((ab ** 2).sum(1), 1e-12)
    out = []
    for p in points:
        t = np.clip(((p - a) * ab).sum(1) / l2, 0, 1)
        out.append(np.min(np.linalg.norm(a + ab * t[:, None] - p, axis=1)))
    return np.array(out)


def near_segment(px, seg, radius):
    """Which pixels are within `radius` of the segment (x0, y0, x1, y1)."""
    a, b = np.array(seg[:2], float), np.array(seg[2:4], float)
    ab = b - a
    t = np.clip(((px - a) @ ab) / max(float(ab @ ab), 1e-12), 0, 1)
    return np.linalg.norm(a + t[:, None] * ab - px, axis=1) <= radius


def load_target(work, target, mapping, part, drops=()):
    """The target outline in document space: (polyline, closed). `part`: keep the outline inside that pixel
    box; `drops`: (x0, y0, x1, y1, radius) pixel segments whose nearby outline is left out (an edge that is
    another layer's, such as where a far ear meets the head). The longest stretch left is the target."""
    path = target if os.path.exists(target) else os.path.join(work, target + ".json")
    with open(path) as f:
        data = json.load(f)
    poly = np.array(data["outline"] if isinstance(data, dict) else data, float)
    closed = bool(data.get("closed", False)) if isinstance(data, dict) else False
    if part or drops:
        px = mapping.to_px(poly)
        inside = np.ones(len(px), bool)
        if part:
            x0, y0, x1, y1 = part
            inside &= (px[:, 0] >= x0) & (px[:, 0] <= x1) & (px[:, 1] >= y0) & (px[:, 1] <= y1)
        for d in drops:
            inside &= ~near_segment(px, d[:4], d[4] if len(d) > 4 else 4.0)
        if not inside.any():
            sys.exit("No outline left after --part and --drop")
        if closed and not inside.all():
            # Start the outline just after a stretch outside, so the inside stretch is in one piece.
            k = int(np.argmin(inside))
            poly, inside = np.roll(poly, -k, axis=0), np.roll(inside, -k)
        runs, start = [], None
        for i, v in enumerate(list(inside) + [False]):
            if v and start is None:
                start = i
            if not v and start is not None:
                runs.append((start, i))
                start = None
        a, b = max(runs, key=lambda r: r[1] - r[0])
        poly, closed = poly[a:b], False
    if closed:
        poly = np.vstack([poly, poly[:1]])
    return poly, closed


def tangent_at(poly, cum, s, window):
    a = at_length(poly, s - window, cum)
    b = at_length(poly, s + window, cum)
    d = np.asarray(b) - np.asarray(a)
    n = np.linalg.norm(d)
    return d / n if n > 0 else np.array([1.0, 0.0])


def outline_corners(poly, cum, degrees=35):
    """Arc lengths where the outline turns sharply (more than `degrees` over 1.5% of its length)."""
    total = cum[-1]
    w = total * 0.015
    ss = np.linspace(0, total, 600, endpoint=False)
    turn = []
    for v in ss:
        a, b, c = (np.asarray(at_length(poly, (v + d) % total, cum)) for d in (-w, 0, w))
        u, t = b - a, c - b
        nu, nt = np.linalg.norm(u), np.linalg.norm(t)
        turn.append(0 if nu < 1e-12 or nt < 1e-12 else math.degrees(math.acos(np.clip(np.dot(u, t) / (nu * nt), -1, 1))))
    turn = np.array(turn)
    k = len(turn)
    return [float(ss[i]) for i in range(k) if turn[i] > degrees and turn[i] >= turn[(i - 1) % k] and turn[i] >= turn[(i + 1) % k]]


def extreme_axis(cp, degrees=20):
    """0 if the point's handles run level (it's a top or bottom), 1 if upright (a left or right side), else None."""
    d = np.array(cp[2]) - np.array(cp[1])
    if np.linalg.norm(d) < 1e-12:
        return None
    angle = math.degrees(math.atan2(abs(d[1]), abs(d[0])))
    return 0 if angle < degrees else 1 if angle > 90 - degrees else None


def curve_bend(curve, closed, i, j, steps=24):
    """Turning (degrees per unit length) along segment i -> j: [(t, bend)]."""
    p0, p1 = np.array(curve[i][0]), np.array(curve[i][2])
    p2, p3 = np.array(curve[j][1]), np.array(curve[j][0])
    t = np.linspace(0, 1, steps)
    d1, d2 = bez_d(p0, p1, p2, p3, t), bez_dd(p0, p1, p2, p3, t)
    speed = np.maximum(np.linalg.norm(d1, axis=1), 1e-12)
    return np.abs(d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0]) / speed ** 3


def winding(pts):
    """+1 if the closed polyline runs anticlockwise (y up), -1 if clockwise."""
    pts = np.asarray(pts)
    x, y = pts[:, 0], pts[:, 1]
    return 1.0 if float(np.dot(x, np.roll(y, -1)) - np.dot(np.roll(x, -1), y)) >= 0 else -1.0


def is_crease(curve, closed, i):
    """Whether corner point i turns inward: against the way the whole curve winds (a notch, not a point)."""
    if not closed:
        return False
    pos, hin, hout = (np.array(v) for v in curve[i][:3])
    n = len(curve)
    a = pos - hin if np.linalg.norm(pos - hin) > 1e-12 else pos - np.array(curve[(i - 1) % n][0])
    b = hout - pos if np.linalg.norm(hout - pos) > 1e-12 else np.array(curve[(i + 1) % n][0]) - pos
    turn = a[0] * b[1] - a[1] * b[0]
    return turn * winding(sample_curve(curve, closed)) < 0


def outline_crease(poly, cum, s, reach, wind, window=0.03):
    """Where within `reach` of s the outline turns most inward (against `wind`), over `window` of its
    length; None if it doesn't turn inward there at all."""
    total = cum[-1]
    w = total * window
    ss = np.linspace(s - reach, s + reach, 121)
    inward = []
    for v in ss:
        a, b, c = (np.asarray(at_length(poly, (v + d) % total, cum)) for d in (-w, 0, w))
        u, t = b - a, c - b
        inward.append(-(u[0] * t[1] - u[1] * t[0]) * wind / max(np.linalg.norm(u) * np.linalg.norm(t), 1e-12))
    k = int(np.argmax(inward))
    return float(ss[k]) if inward[k] > 0 and 3 <= k <= len(ss) - 4 else None


def shape_turn(before, after):
    """How far (radians) a shape turned: the angle between its long axes (principal components), the
    smaller of the two ways the axis can face."""
    def axis(pts):
        c = pts - pts.mean(0)
        w, v = np.linalg.eigh(c.T @ c)
        return v[:, int(np.argmax(w))]
    a, b = axis(np.asarray(before)), axis(np.asarray(after))
    angle = math.atan2(a[0] * b[1] - a[1] * b[0], a[0] * b[0] + a[1] * b[1])
    if angle > math.pi / 2:
        angle -= math.pi
    elif angle < -math.pi / 2:
        angle += math.pi
    return angle


def outline_reach(poly, cum, s, reach, direction):
    """Where within `reach` of s the outline goes farthest in `direction`, or None (at the stretch's ends)."""
    total = cum[-1]
    ss = np.linspace(s - reach, s + reach, 121)
    pts = np.array([at_length(poly, v % total, cum) for v in ss])
    k = int(np.argmax(pts @ direction))
    return float(ss[k]) if 3 <= k <= len(ss) - 4 else None


def smooth_looking(cp, degrees=12):
    """A point not set aligned whose handles still run in line (within `degrees`) is drawn smooth."""
    pos, hin, hout = (np.array(v) for v in cp[:3])
    a, b = pos - hin, hout - pos
    la, lb = np.linalg.norm(a), np.linalg.norm(b)
    if la < 1e-9 or lb < 1e-9:
        return False
    return float(np.dot(a, b) / (la * lb)) > math.cos(math.radians(degrees))


def crosses(p0, p1, p2, p3):
    """Whether handle p0-p1 crosses handle p3-p2."""
    def side(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    return side(p0, p1, p2) * side(p0, p1, p3) < 0 and side(p3, p2, p0) * side(p3, p2, p1) < 0


def fit_segment(p0, p3, pts, t0, t3, iters=6, fix1=None, fix2=None):
    """Handles p1, p2 for a cubic from p0 to p3 through pts (in order). t0: the direction the out handle
    must point (None: free); t3: the direction the in handle at p3 must point (None: free); fix1, fix2: a
    handle end that stays as it is (a coarser level's point, fitted already). Least squares with Newton
    reparameterisation (Schneider)."""
    if fix1 is not None and fix2 is not None:
        return np.asarray(fix1), np.asarray(fix2)
    if len(pts) < 2:
        return (np.asarray(fix1) if fix1 is not None else p0 + (p3 - p0) / 3,
                np.asarray(fix2) if fix2 is not None else p3 + (p0 - p3) / 3)
    cum = arclen(np.vstack([p0, pts, p3]))
    u = (cum / cum[-1])[1:-1] if cum[-1] > 0 else np.linspace(0, 1, len(pts) + 2)[1:-1]
    chord = np.linalg.norm(p3 - p0)
    p1, p2 = p0 + (p3 - p0) / 3, p3 + (p0 - p3) / 3
    for _ in range(iters):
        uu = u[:, None]
        b0, b1, b2, b3 = (1 - uu) ** 3, 3 * uu * (1 - uu) ** 2, 3 * uu * uu * (1 - uu), uu ** 3
        rhs = pts - (b0 + b1) * p0 - (b2 + b3) * p3  # what b1 (p1 - p0) + b2 (p2 - p3) must make
        if fix1 is not None:
            rhs = rhs - b1 * (np.asarray(fix1) - p0)
        if fix2 is not None:
            rhs = rhs - b2 * (np.asarray(fix2) - p3)
        cols, names = [], []
        if fix1 is not None:
            pass
        elif t0 is not None:
            cols.append(b1 * t0)
            names.append("a")
        else:
            cols += [b1 * np.array([1, 0]), b1 * np.array([0, 1])]
            names += ["x1", "y1"]
        if fix2 is not None:
            pass
        elif t3 is not None:
            cols.append(b2 * t3)
            names.append("b")
        else:
            cols += [b2 * np.array([1, 0]), b2 * np.array([0, 1])]
            names += ["x2", "y2"]
        A = np.stack([c.reshape(-1) for c in cols], axis=1)
        sol, *_ = np.linalg.lstsq(A, rhs.reshape(-1), rcond=None)
        v = dict(zip(names, sol))
        if fix1 is not None:
            p1 = np.asarray(fix1)
        elif t0 is not None:
            a = v["a"] if v["a"] > chord * 0.02 else chord / 3
            p1 = p0 + a * t0
        else:
            p1 = p0 + np.array([v["x1"], v["y1"]])
        if fix2 is not None:
            p2 = np.asarray(fix2)
        elif t3 is not None:
            b = v["b"] if v["b"] > chord * 0.02 else chord / 3
            p2 = p3 + b * t3
        else:
            p2 = p3 + np.array([v["x2"], v["y2"]])
        # Handles longer than 3/4 of the chord, or crossing, make loops and kinks: shorten them.
        limit = chord * 0.75
        for name in [n for n, f in (("p1", fix1), ("p2", fix2)) if f is None]:
            base = p0 if name == "p1" else p3
            h = (p1 if name == "p1" else p2) - base
            length = np.linalg.norm(h)
            if length > limit:
                h = h * (limit / length)
                if name == "p1":
                    p1 = base + h
                else:
                    p2 = base + h
        if crosses(p0, p1, p2, p3) and fix1 is None and fix2 is None:
            mid = (p1 + p2) / 2
            p1, p2 = p0 + (mid - p0) * 0.66, p3 + (mid - p3) * 0.66
        # Newton step on each sample's parameter.
        q, d1, d2 = bez(p0, p1, p2, p3, u), bez_d(p0, p1, p2, p3, u), bez_dd(p0, p1, p2, p3, u)
        num = ((q - pts) * d1).sum(1)
        den = (d1 * d1).sum(1) + ((q - pts) * d2).sum(1)
        u = np.clip(u - np.where(np.abs(den) > 1e-12, num / np.where(np.abs(den) > 1e-12, den, 1), 0), 0, 1)
        u = np.maximum.accumulate(u)
    return p1, p2


# --- Commands ------------------------------------------------------------------------------------------


def opt(args, name, default=None, cast=str):
    if name in args:
        i = args.index(name)
        value = args[i + 1]
        del args[i:i + 2]
        return cast(value)
    return default


def flag(args, name):
    if name in args:
        args.remove(name)
        return True
    return False


def opts_all(args, name):
    out = []
    while name in args:
        i = args.index(name)
        out.append(args[i + 1])
        del args[i:i + 2]
    return out


def size_of(text):
    w, h = text.lower().split("x")
    return int(w), int(h)


def cmd_snapshot(args):
    char = opt(args, "--char", "Clay")
    ref = opt(args, "--ref", "HeadTurn")
    size = opt(args, "--size", (1920, 1080), size_of)
    skip = opts_all(args, "--skip") or ["Hair_X"]
    frame = opt(args, "--frame", None, int)
    session, work = args[0], os.path.abspath(args[1])
    os.makedirs(work, exist_ok=True)
    clay_png, ref_png, state_json = (os.path.join(work, n) for n in ("clay.png", "ref.png", "state.json"))
    for p in (clay_png, ref_png):
        if os.path.exists(p):
            os.remove(p)
    visible = {int(k): v for k, v in oa_remote.run_json(session, top_visibility_code()).items()}
    opacity = None
    was_frame = None
    if frame is not None:  # render that frame, then put back the one the app was on (someone may be using it)
        was_frame = oa_remote.run_json(session, "import oanim, json\nd = oanim.data.document\nprint(json.dumps(d.frame_current))\n"
                                                "d.frame_current = %d" % frame)
    try:
        oa_remote.run(session, snapshot_code(char, ref, size, skip, clay_png, ref_png, state_json))
        if not oa_remote.wait_file(clay_png):
            sys.exit("The character's render didn't come")
        opacity = oa_remote.run_json(session, restore_and_ref_code(char, ref, size, ref_png))["opacity"]
        if not oa_remote.wait_file(ref_png):
            sys.exit("The reference's render didn't come")
    finally:
        oa_remote.run(session, put_back_code(visible, ref, opacity if opacity is not None else
                                             oa_remote.run_json(session, LAYER_PATHS + f"print(json.dumps(_by_path({ref!r}).opacity))")))
        if was_frame is not None:
            oa_remote.run(session, "import oanim\noanim.data.document.frame_current = %d" % was_frame)
    state = load_state(work)
    m = Mapping(state)
    with open(os.path.join(work, "mapping.json"), "w") as f:
        json.dump({"to_px": m.to_px_m.tolist(), "to_doc": m.to_doc_m.tolist(), "px_per_unit": m.px_per_unit}, f)
    overlay(work)
    print("frame", state["frame"], "action", state["action"], "| pixels per document unit %.1f" % m.px_per_unit)
    for l in state["layers"]:
        if l["type"] != "VECTOR" and l.get("masks", "Shows everything") != "Shows everything":
            print("  %-45s group, masks: %s" % (l["path"], l["masks"]))
        if l["type"] == "VECTOR":
            curves = len(l.get("curves", []))
            levels = ""
            if l.get("level_curves"):
                levels = "  detail levels 0-%d (edit level %s)" % (len(l["level_curves"]) - 1,
                                                                 ",".join(str(v) for v in sorted(set(l["detail"]["shape_levels"]))))
            print("  %-45s %s curves %d points %d%s%s%s" % (l["path"], "  " if l["visible"] else "H ", curves,
                  len(l.get("points", [])), levels, ("  masks: " + l["masks"]) if l.get("masks") else "",
                  ("  (reference of %s)" % l["reference_of"]) if l.get("reference_of") else ""))
    print("wrote", clay_png, ref_png, os.path.join(work, "overlay.png"))


def overlay(work):
    ref = Image.open(os.path.join(work, "ref.png")).convert("RGBA")
    clay = Image.open(os.path.join(work, "clay.png")).convert("RGBA")
    bg = Image.new("RGBA", ref.size, (255, 255, 255, 255))
    bg.alpha_composite(ref)
    c = np.array(clay).astype(float)
    c[..., 3] *= 0.5
    bg.alpha_composite(Image.fromarray(c.astype(np.uint8)))
    bg.convert("RGB").save(os.path.join(work, "overlay.png"))


def solo_code(char, paths, size, png):
    return LAYER_PATHS + f"""
char = _by_path({char!r})
keep = set()
for p in {paths!r}:
    l = _by_path(p)
    for x in _under(l):
        keep.add(x.id)
    while l is not None:
        keep.add(l.id)
        l = l.parent
was = {{}}
for l in oanim.data.layers.all():
    was[l.id] = l.visible
    if l.id not in keep:
        l.visible = False
oanim.wm.render_png({png!r}, {size[0]}, {size[1]}, transparent=True)
print(json.dumps({{str(k): v for k, v in was.items()}}))
"""


def cmd_solo(args):
    char = opt(args, "--char", "Clay")
    size = opt(args, "--size", None, size_of)
    session, work, name, paths = args[0], os.path.abspath(args[1]), args[2], args[3:]
    state = load_state(work)
    size = size or tuple(state["size"])
    png = os.path.join(work, name + ".png")
    if os.path.exists(png):
        os.remove(png)
    was = oa_remote.run_json(session, solo_code(char, paths, size, png))
    ok = oa_remote.wait_file(png)
    oa_remote.run(session, LAYER_PATHS + f"""
was = {was!r}
for l in oanim.data.layers.all():
    if str(l.id) in was:
        l.visible = was[str(l.id)]
""")
    print("wrote" if ok else "no render:", png)


def font(size):
    for name in ("arial.ttf", "segoeui.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def cmd_wires(args):
    on = opt(args, "--on", "ref")
    out = opt(args, "--out", None)
    pad = opt(args, "--pad", 40, int)
    zoom = opt(args, "--zoom", 2.0, float)
    only = opt(args, "--curves", None, lambda s: [int(x) for x in s.split(",")])
    edits = opts_all(args, "--edits")
    targets = opts_all(args, "--target")
    level = opt(args, "--level", None, int)
    work, paths = os.path.abspath(args[0]), args[1:]
    state = load_state(work)
    m = Mapping(state)
    base = Image.new("RGBA", tuple(state["size"]), (255, 255, 255, 255))
    if on in ("ref", "both"):
        base.alpha_composite(Image.open(os.path.join(work, "ref.png")).convert("RGBA"))
    if on in ("clay", "both"):
        c = np.array(Image.open(os.path.join(work, "clay.png")).convert("RGBA")).astype(float)
        if on == "both":
            c[..., 3] *= 0.45
        base.alpha_composite(Image.fromarray(c.astype(np.uint8)))
    colors = [(230, 20, 60), (20, 120, 230), (20, 170, 60), (200, 100, 0), (150, 40, 200), (0, 160, 160)]
    drawn = []
    for k, path in enumerate(paths):
        layer = find_layer(state, path)
        for ci in range(len(layer.get("curves", []))):
            if only is not None and ci not in only:
                continue
            curve, closed = shown_curve(layer, ci, level)
            drawn.append((colors[(k + ci) % len(colors)], ci, curve, closed))
    for e in edits:
        with open(e) as f:
            ed = json.load(f)
        layer = find_layer(state, ed["layer"])
        for ci, curve in ed.get("preview", {}).items():
            drawn.append(((255, 140, 0), int(ci), [(tuple(c[0]), tuple(c[1]), tuple(c[2]), c[3], False, c[4], c[5], c[6]) for c in curve],
                          ed["closed"][ci]))
    if not drawn:
        sys.exit("Nothing to draw")
    allpx = np.concatenate([m.to_px(sample_curve(c, cl)) for _, _, c, cl in drawn if len(c)])
    x0, y0 = np.maximum(allpx.min(0) - pad, 0).astype(int)
    x1, y1 = np.minimum(allpx.max(0) + pad, np.array(base.size) - 1).astype(int)
    crop = base.crop((x0, y0, x1, y1))
    crop = crop.resize((int((x1 - x0) * zoom), int((y1 - y0) * zoom)), Image.LANCZOS)
    img = Image.new("RGBA", crop.size)
    img.alpha_composite(crop)
    d = ImageDraw.Draw(img)
    f = font(max(11, int(7 * zoom)))

    def P(p):
        q = m.to_px([p])[0]
        return ((q[0] - x0) * zoom, (q[1] - y0) * zoom)

    for t in targets:
        poly, _ = load_target(work, t, m, None)
        d.line([P(p) for p in poly], fill=(0, 200, 230, 255), width=1)
    for color, ci, curve, closed in drawn:
        line = [P(p) for p in sample_curve(curve, closed)]
        if len(line) > 1:
            d.line(line, fill=color + (255,), width=2)
        for li, (pos, hin, hout, aligned, hidden, point, whole, lv) in enumerate(curve):
            a = P(pos)
            for h in (hin, hout):
                b = P(h)
                if abs(b[0] - a[0]) + abs(b[1] - a[1]) > 1:
                    d.line([a, b], fill=(90, 90, 90, 220), width=1)
                    d.ellipse([b[0] - 3, b[1] - 3, b[0] + 3, b[1] + 3], outline=(60, 60, 60, 255))
            r = 4 if lv == 0 else 3
            shape = d.ellipse if aligned else d.rectangle
            shape([a[0] - r, a[1] - r, a[0] + r, a[1] + r], fill=(255, 255, 255, 255) if not hidden else (200, 200, 200, 255),
                  outline=color + (255,))
            d.text((a[0] + 5, a[1] - 14), "%d:%d" % (ci, li) + ("" if point == li else " p%d" % point) + (" L%d" % lv if lv else ""),
                   fill=color + (255,), font=f,
                   stroke_width=2, stroke_fill=(255, 255, 255, 255))
    out = out or os.path.join(work, "wires_" + "_".join(p.split("/")[-1] for p in paths) + ".png")
    img.convert("RGB").save(out)
    print("wrote", out, "(crop %d,%d to %d,%d of the render, zoom %g; round = smooth point, square = corner)" % (x0, y0, x1, y1, zoom))


def cmd_region(args):
    tol = opt(args, "--tol", 28, float)
    image = opt(args, "--image", "ref")
    close = opt(args, "--close", 3, int)
    opening = opt(args, "--open", 2, int)
    grow = opt(args, "--grow", 0, int)
    import cv2
    work = os.path.abspath(args[0])
    seeds = [tuple(int(float(v)) for v in a.split(",")) for a in args[1:-1]]
    name = args[-1]
    state = load_state(work)
    m = Mapping(state)
    rgba = np.array(Image.open(os.path.join(work, image + ".png")).convert("RGBA"))
    bg = np.full(rgba.shape[:2] + (3,), 255, float)
    a = rgba[..., 3:4] / 255.0
    rgb = (rgba[..., :3] * a + bg * (1 - a)).astype(np.uint8)
    h, w = rgb.shape[:2]
    region = np.zeros((h, w), np.uint8)
    for sx, sy in seeds:
        mask = np.zeros((h + 2, w + 2), np.uint8)
        cv2.floodFill(rgb.copy(), mask, (sx, sy), 0, (tol,) * 3, (tol,) * 3,
                      4 | cv2.FLOODFILL_FIXED_RANGE | cv2.FLOODFILL_MASK_ONLY | (255 << 8))
        region |= mask[1:-1, 1:-1]
    if opening > 0:  # thin spurs where the fill ran along a line
        k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * opening + 1, 2 * opening + 1))
        region = cv2.morphologyEx(region, cv2.MORPH_OPEN, k)
    if close > 0:
        k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * close + 1, 2 * close + 1))
        region = cv2.morphologyEx(region, cv2.MORPH_CLOSE, k)
    if grow:
        k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * abs(grow) + 1,) * 2)
        region = (cv2.dilate if grow > 0 else cv2.erode)(region, k)
    contours, _ = cv2.findContours(region, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    if not contours:
        sys.exit("The fill found nothing")
    c = max(contours, key=cv2.contourArea)[:, 0, :].astype(float)
    # Smooth the pixel staircase a little (a moving average along the outline).
    kk = 5
    c = np.array([np.convolve(np.r_[c[-kk:, i], c[:, i], c[:kk, i]], np.ones(2 * kk + 1) / (2 * kk + 1), "valid") for i in (0, 1)]).T
    c = c[::2]
    doc_pts = m.to_doc(c)
    with open(os.path.join(work, name + ".json"), "w") as f:
        json.dump({"outline": doc_pts.tolist(), "closed": True, "pixels": c.tolist(), "seeds": seeds, "tol": tol}, f)
    Image.fromarray(region).save(os.path.join(work, name + "_mask.png"))
    prev = Image.fromarray(rgb).convert("RGBA")
    d = ImageDraw.Draw(prev)
    d.line([tuple(p) for p in c] + [tuple(c[0])], fill=(255, 0, 80, 255), width=2)
    for s in seeds:
        d.ellipse([s[0] - 4, s[1] - 4, s[0] + 4, s[1] + 4], fill=(0, 200, 255, 255))
    x0, y0 = np.maximum(c.min(0) - 30, 0).astype(int)
    x1, y1 = np.minimum(c.max(0) + 30, [w - 1, h - 1]).astype(int)
    prev.crop((x0, y0, x1, y1)).convert("RGB").save(os.path.join(work, name + ".png"))
    print("region %s: %d px, outline %d points, box %d,%d to %d,%d -> %s" % (
        name, int(region.sum() / 255), len(c), x0, y0, x1, y1, os.path.join(work, name + ".png")))


def cmd_stroke(args):
    """A drawn line's middle in the reference, as an open target: the dark pixels in a box, followed from one
    end to the other."""
    dark = opt(args, "--dark", 90, float)
    image = opt(args, "--image", "ref")
    work, box, name = os.path.abspath(args[0]), [int(float(v)) for v in args[1].split(",")], args[2]
    starts = opts_all(args, "--start")
    nots = opts_all(args, "--not")
    import cv2
    state = load_state(work)
    m = Mapping(state)
    rgb = np.array(Image.open(os.path.join(work, image + ".png")).convert("RGB")).astype(float)
    x0, y0, x1, y1 = box
    lum = rgb[y0:y1, x0:x1] @ np.array([0.299, 0.587, 0.114])
    mask = (lum < dark).astype(np.uint8)
    for name_not in nots:  # a region already traced (the nose a mouth line runs under), grown 3 pixels
        other = np.array(Image.open(os.path.join(work, name_not + "_mask.png"))) > 0
        other = cv2.dilate(other.astype(np.uint8), cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))) > 0
        mask[other[y0:y1, x0:x1]] = 0
    count, labels, stats, _ = cv2.connectedComponentsWithStats(mask, 8)
    if count < 2:
        sys.exit("No pixels darker than --dark %g in that box" % dark)
    keep = 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))
    ys, xs = np.nonzero(labels == keep)
    pts = np.stack([xs + x0, ys + y0], 1).astype(float)
    # Order along the line: from the end given (or the point farthest from the middle), each step to the
    # nearest pixels not yet passed, averaged into a middle line.
    start = np.array([float(v) for v in starts[0].split(",")]) if starts else pts[np.argmax(np.linalg.norm(pts - pts.mean(0), axis=1))]
    order = []
    here = pts[np.argmin(np.linalg.norm(pts - start, axis=1))]
    left = np.ones(len(pts), bool)
    step = 3.0
    while left.any():
        d = np.linalg.norm(pts - here, axis=1)
        near = left & (d <= step * 1.5)
        if not near.any():
            cand = np.where(left)[0]
            k = cand[np.argmin(d[cand])]
            if d[k] > step * 4:
                break
            near = left & (d <= d[k] + step)
        centre = pts[near].mean(0)
        order.append(centre)
        left &= ~(np.linalg.norm(pts - centre, axis=1) <= step * 1.5)
        left &= ~near
        here = centre
    line = np.array(order)
    if len(line) < 2:
        sys.exit("The line is too short")
    with open(os.path.join(work, name + ".json"), "w") as f:
        json.dump({"outline": m.to_doc(line).tolist(), "closed": False, "pixels": line.tolist()}, f)
    prev = Image.fromarray(rgb.astype(np.uint8)).convert("RGBA")
    d = ImageDraw.Draw(prev)
    d.line([tuple(p) for p in line], fill=(0, 220, 255, 255), width=1)
    d.ellipse([line[0][0] - 3, line[0][1] - 3, line[0][0] + 3, line[0][1] + 3], outline=(0, 200, 0, 255))
    prev.crop((x0 - 10, y0 - 10, x1 + 10, y1 + 10)).resize(((x1 - x0 + 20) * 3, (y1 - y0 + 20) * 3), Image.LANCZOS) \
        .convert("RGB").save(os.path.join(work, name + ".png"))
    print("stroke %s: %d points from %s to %s -> %s" % (name, len(line), line[0].round(), line[-1].round(),
                                                        os.path.join(work, name + ".png")))


def cmd_fit(args):
    rng = opt(args, "--range", None)
    part = opt(args, "--part", None, lambda s: [float(v) for v in s.split(",")])
    fixed = opt(args, "--fixed", [], lambda s: [int(v) for v in s.split(",") if v != ""])
    corners = opt(args, "--corner", [], lambda s: [int(v) for v in s.split(",") if v != ""])
    pins = opts_all(args, "--pin")
    handles_only = flag(args, "--handles-only")
    join_fixed = flag(args, "--join-fixed")
    assign = opt(args, "--assign", None)
    level = opt(args, "--level", None, int)
    smooth_px = opt(args, "--smooth-under", 3.0, float)
    drops = [[float(v) for v in d.split(",")] for d in opts_all(args, "--drop")]
    out = opt(args, "--out", None)
    work, path, ci, target = os.path.abspath(args[0]), args[1], int(args[2]), args[3]
    state = load_state(work)
    m = Mapping(state)
    layer = find_layer(state, path)
    if layer.get("reference_of"):
        print("Note: %s is a reference of %s; its points follow the original, so edit that one." % (layer["path"], layer["reference_of"]))
    top = detail_top(layer)
    if top == 0:
        level = None
    elif level is None:
        level = 0
        print("Note: %s has detail levels 0-%d: fitting level 0 (its own points; the detail rides along). "
              "Apply it, snapshot again, then --level 1." % (layer["path"], top))
    elif not 0 <= level <= top:
        sys.exit("%s has detail levels 0-%d" % (layer["path"], top))
    curve, closed = shown_curve(layer, ci, level)
    n = len(curve)
    if rng:
        a, b = (int(v) for v in rng.split(":"))
        idx = [a]
        while idx[-1] != b:
            idx.append((idx[-1] + 1) % n)
    else:
        idx = list(range(n))
    if level:
        # A finer level moves only its own points; the coarser ones, fitted already, stay with their handles.
        coarse = [i for i in idx if curve[i][7] < level]
        fixed = sorted(set(fixed) | set(coarse))
        if len(coarse) == len(idx):
            sys.exit("No level %d points in that range" % level)
    poly, tclosed = load_target(work, target, m, part, drops)
    if join_fixed and not tclosed:
        # The range's held ends are hidden (behind another layer): the outline runs on to them straight, so
        # the curve leaves the visible stretch heading where it goes on.
        for i in (idx[0], idx[-1]):
            if i in fixed:
                q = np.array(curve[i][0])
                if np.linalg.norm(poly[0] - q) < np.linalg.norm(poly[-1] - q):
                    poly = np.vstack([q, poly])
                else:
                    poly = np.vstack([poly, q])
    cum = arclen(poly)
    total = cum[-1]
    pos = {i: np.array(curve[i][0]) for i in range(n)}
    hin = {i: np.array(curve[i][1]) for i in range(n)}
    hout = {i: np.array(curve[i][2]) for i in range(n)}
    pinned = {}
    for p in pins:
        k, xy = p.split("=")
        pinned[int(k)] = m.to_doc([[float(v) for v in xy.split(",")]])[0]

    # Where along the outline each point goes (s: arc length). "share": every point keeps its share of
    # the curve's length, the whole set turned round the outline to where it fits best, so each point keeps
    # its role; then corner points take the outline's nearby corners. "nearest": each point's nearest place.
    assign = assign or ("between" if level else "share" if tclosed and closed and len(idx) == n else "nearest")
    if assign == "between":
        # Detail points keep their share of the way between the coarser points around them, which stay.
        s = {i: project(poly, cum, pos[i])[0] for i in idx}
        if tclosed:
            order = [s[i] for i in idx]
            forwards = sum((order[(k + 1) % len(order)] - order[k]) % total for k in range(len(order)))
            backwards = sum((order[k] - order[(k + 1) % len(order)]) % total for k in range(len(order)))
            if backwards < forwards:
                poly = poly[::-1].copy()
                cum = arclen(poly)
                s = {i: project(poly, cum, pos[i])[0] for i in idx}
        elif sum(1 for k in range(len(idx) - 1) if s[idx[k + 1]] >= s[idx[k]]) < (len(idx) - 1) / 2:
            poly = poly[::-1].copy()  # the outline runs the other way
            cum = arclen(poly)
            s = {i: project(poly, cum, pos[i])[0] for i in idx}
        lengths = {}
        for i, j in segments(curve, closed):
            q = bez(pos[i], np.array(curve[i][2]), np.array(curve[j][1]), pos[j], np.linspace(0, 1, 32))
            lengths[i] = arclen(q)[-1]
        ring = closed and len(idx) == n
        count = len(idx)
        for k, i in enumerate(idx):
            if i in fixed:
                continue
            back, ahead = k, k
            while (idx[back % count] not in fixed) if ring else (back >= 0 and idx[back] not in fixed):
                back -= 1
            while (idx[ahead % count] not in fixed) if ring else (ahead < count and idx[ahead] not in fixed):
                ahead += 1
            if not ring and (back < 0 or ahead >= count):
                continue  # an end of the range: its nearest place
            a, b = idx[back % count], idx[ahead % count]
            walk = [idx[t % count] for t in range(back, ahead)]
            along = sum(lengths[w] for w in walk[:k - back])
            span = sum(lengths[w] for w in walk)
            sa, sb = s[a], s[b]
            if tclosed:
                sb = sa + (sb - sa) % total
            s[i] = sa + (sb - sa) * (along / span if span > 0 else 0.5)
    elif assign == "share":
        if not (tclosed and closed and len(idx) == n):
            sys.exit("--assign share needs a closed curve fitted whole to a closed outline")
        lengths = []
        for i, j in segments(curve, closed):
            q = bez(pos[i], np.array(curve[i][2]), np.array(curve[j][1]), pos[j], np.linspace(0, 1, 32))
            lengths.append(arclen(q)[-1])
        frac = np.concatenate([[0], np.cumsum(lengths)])[:-1] / max(sum(lengths), 1e-12)
        here = np.array([pos[i] for i in idx])
        best = None
        for reverse in (False, True):
            pl = poly[::-1].copy() if reverse else poly
            cl = arclen(pl)
            for o in np.linspace(0, total, 720, endpoint=False):
                cost = float(((np.array([at_length(pl, (o + f * total) % total, cl) for f in frac]) - here) ** 2).sum())
                if best is None or cost < best[0]:
                    best = (cost, reverse, o)
        if best[1]:
            poly = poly[::-1].copy()
            cum = arclen(poly)
        s = {i: best[2] + frac[k] * total for k, i in enumerate(idx)}
        peaks = outline_corners(poly, cum)
        for k, i in enumerate(idx):
            if (curve[i][3] or smooth_looking(curve[i])) and i not in corners:
                continue
            gap = min((s[i] - s[idx[k - 1]]) % total, (s[idx[(k + 1) % len(idx)]] - s[i]) % total)
            if is_crease(curve, closed, i):
                # A crease (the outline turns inward there, as where a nose's lobe meets its bridge): the
                # outline's most inward turn nearby, however gentle.
                found = outline_crease(poly, cum, s[i], 0.45 * gap, winding(sample_curve(curve, closed)))
                if found is not None:
                    print("  %d:%d p%d: at the outline's crease" % (ci, i, curve[i][5]))
                    s[i] = found
                continue
            near = [p for p in peaks for p in (p, p + total, p - total) if abs(p - s[i]) < 0.45 * gap]
            if near:
                s[i] = min(near, key=lambda p: abs(p - s[i]))
        # A smooth point is where its shape reaches farthest out its way from the shape's middle (the middle
        # of a rounded end, the top of a hump): it takes the outline's farthest place that way nearby, the way
        # turned as much as the whole shape turned.
        turn = shape_turn(sample_curve(curve, closed), poly)
        rot = np.array([[math.cos(turn), -math.sin(turn)], [math.sin(turn), math.cos(turn)]])
        middle = sample_curve(curve, closed).mean(0)
        for k, i in enumerate(idx):
            if i in corners or not (curve[i][3] or smooth_looking(curve[i])):
                continue
            way = pos[i] - middle
            if np.linalg.norm(way) < 1e-12:
                continue
            way = way / np.linalg.norm(way)
            gap = min((s[i] - s[idx[k - 1]]) % total, (s[idx[(k + 1) % len(idx)]] - s[i]) % total)
            found = outline_reach(poly, cum, s[i], 0.45 * gap, rot @ way)
            if found is not None:
                print("  %d:%d p%d: where the outline reaches farthest its way" % (ci, i, curve[i][5]))
                s[i] = found
    else:
        s = {i: project(poly, cum, pos[i])[0] for i in idx}
        if tclosed:
            order = [s[i] for i in idx]
            steps = [(order[(k + 1) % len(order)] - order[k]) % total for k in range(len(order))]
            backwards = [(order[k] - order[(k + 1) % len(order)]) % total for k in range(len(order))]
            if sum(backwards) < sum(steps):  # the outline runs the other way round
                poly = poly[::-1].copy()
                cum = arclen(poly)
                s = {i: project(poly, cum, pos[i])[0] for i in idx}
        else:
            increasing = sum(1 for k in range(len(idx) - 1) if s[idx[k + 1]] >= s[idx[k]])
            if increasing < (len(idx) - 1) / 2:
                poly = poly[::-1].copy()
                cum = arclen(poly)
                s = {i: project(poly, cum, pos[i])[0] for i in idx}
    # A whole open line fitted to a whole traced stroke: its ends are the stroke's ends.
    if not closed and not tclosed and idx == list(range(n)) and not part and not drops and n > 1:
        s[idx[0]], s[idx[-1]] = 0.0, total
    # Pinned and fixed points: the outline's place nearest where they'll be.
    for i in idx:
        if i in pinned or i in fixed or handles_only:
            want = pinned[i] if i in pinned else pos[i]
            v = project(poly, cum, want)[0]
            if tclosed:
                v = min((v, v + total, v - total), key=lambda c: abs(c - s[i]))
            s[i] = v
    # In the curve's order along the outline.
    if tclosed:
        base, acc = s[idx[0]], 0.0
        for k, i in enumerate(idx):
            v = (s[i] - base) % total
            if k and v <= acc:
                v = acc + 1e-6
            acc = v
            s[i] = base + v
    else:
        for k in range(1, len(idx)):
            s[idx[k]] = max(s[idx[k]], s[idx[k - 1]] + 1e-6)

    def on(sv):
        return np.asarray(at_length(poly, sv % total if tclosed else sv, cum))

    before_samples = sample_curve(curve, closed, only=set(idx))
    new_pos = dict(pos)
    held = set(fixed) if level else set()  # coarser points: their handles stay too
    # A detail point whose coarser segment already lies on the outline (within --smooth-under pixels) goes
    # back onto it (Smooth to Lower Level: no offset of its own), rather than being fitted: an offset kept
    # from before the coarse curve was reshaped is what makes a lump.
    smooth = []
    if level:
        coarse_curve, _ = shown_curve(layer, ci, level - 1)
        at_coarse = {c[5]: k for k, c in enumerate(coarse_curve)}

        def neighbour(i, step):
            j = i
            while True:
                j += step
                if closed:
                    j %= n
                elif not 0 <= j < n:
                    return None
                if j == i:
                    return None
                if curve[j][7] < level:
                    return j

        for i in idx:
            if i in held:
                continue
            a, b = neighbour(i, -1), neighbour(i, 1)
            if a is None or b is None:
                continue
            ka, kb = at_coarse.get(curve[a][5]), at_coarse.get(curve[b][5])
            if ka is None or kb is None or (ka + 1) % len(coarse_curve) != kb:
                continue
            A, B = coarse_curve[ka], coarse_curve[kb]
            q = bez(np.array(A[0]), np.array(A[2]), np.array(B[1]), np.array(B[0]), np.linspace(0, 1, 40))
            err = float(dist_to_poly(q, poly).max() * m.px_per_unit)
            if err > smooth_px:
                print("  %d:%d p%d: the level %d curve is %.1f px off the outline there: fitting it" % (
                    ci, i, curve[i][5], level - 1, err))
            if err <= smooth_px:
                smooth.append(i)
                print("  %d:%d p%d: the level %d curve is within %.1f px of the outline there: smoothing onto it" % (
                    ci, i, curve[i][5], level - 1, err))
        held |= set(smooth)
    for i in idx:
        if i in fixed or handles_only or i in smooth:
            continue
        new_pos[i] = pinned[i] if i in pinned else on(s[i])
    window = total * 0.02
    tangent = {}
    for i in idx:
        aligned = (curve[i][3] or smooth_looking(curve[i])) and i not in corners
        if aligned:
            tangent[i] = tangent_at(poly, cum, s[i] % total if tclosed else s[i], window)
    new_in, new_out = dict(hin), dict(hout)
    pairs = [(i, j) for i, j in segments(curve, closed) if i in idx and j in idx]
    for i, j in pairs:
        s0, s1 = s[i], s[j]
        if s1 <= s0 and tclosed:
            s1 += total  # the segment closing the curve
        if s1 <= s0:
            continue
        ss = np.linspace(s0, s1, 40)[1:-1]
        pts = np.array([on(v) for v in ss])
        t0 = tangent.get(i)
        t3 = -tangent[j] if j in tangent else None
        if i in held and j in held:
            continue
        p1, p2 = fit_segment(new_pos[i], new_pos[j], pts, t0, t3,
                             fix1=hout[i] if i in held else None, fix2=hin[j] if j in held else None)
        new_out[i], new_in[j] = p1, p2
    # A smooth point at the ends of the range keeps its outside handle in line with the fitted one.
    for i in idx:
        if i in tangent and i not in held:
            for this, other, side in ((new_out, new_in, "in"), (new_in, new_out, "out")):
                fitted = any((i, j) in pairs for j in range(n)) if side == "in" else any((j, i) in pairs for j in range(n))
                if fitted and not any(((i, j) in pairs) if side == "out" else ((j, i) in pairs) for j in range(n)):
                    length = np.linalg.norm(np.array(curve[i][1 if side == "in" else 2]) - pos[i])
                    direction = -tangent[i] if side == "in" else tangent[i]
                    other[i] = new_pos[i] + direction * length
    # Untouched points' handles move with their point.
    for i in range(n):
        if i not in idx:
            continue
        shift = new_pos[i] - pos[i]
        if not any(i in pr for pr in pairs):
            new_in[i], new_out[i] = hin[i] + shift, hout[i] + shift
    after = [(tuple(new_pos[i]), tuple(new_in[i]), tuple(new_out[i]), curve[i][3], curve[i][4], curve[i][5], curve[i][6], curve[i][7])
             for i in range(n)]
    after_samples = sample_curve(after, closed, only=set(idx))
    ppu = m.px_per_unit
    e0 = dist_to_poly(before_samples, poly) * ppu
    e1 = dist_to_poly(after_samples, poly) * ppu
    edits = {"layer": layer["path"], "layer_id": layer["id"], "frame": state["frame"], "level": level,
             "smooth": [curve[i][5] for i in smooth],
             "points": [[curve[i][5], float(new_pos[i][0]), float(new_pos[i][1])] for i in idx if not np.allclose(new_pos[i], pos[i])],
             "handles": [], "preview": {str(ci): [[list(a[0]), list(a[1]), list(a[2]), a[3], a[5], a[6], a[7]] for a in after]},
             "closed": {str(ci): closed}}
    for i in idx:
        if i in held:
            continue
        for side, new, old in (("IN", new_in[i], hin[i]), ("OUT", new_out[i], hout[i])):
            if not np.allclose(new, old):
                edits["handles"].append([ci, curve[i][6], side, float(new[0]), float(new[1])])
    suffix = "" if level is None else "_L%d" % level
    out = out or os.path.join(work, "edits_%s_c%d%s.json" % (layer["path"].split("/")[-1], ci, suffix))
    with open(out, "w") as f:
        json.dump(edits, f)
    print("%s curve %d%s, points %s: off the outline before mean %.1f px, worst %.1f px; after mean %.1f px, worst %.1f px" % (
        layer["path"], ci, "" if level is None else " at detail level %d" % level, idx, e0.mean(), e0.max(), e1.mean(), e1.max()))
    moved = [(i, curve[i][5], float(np.linalg.norm(new_pos[i] - pos[i]) * ppu)) for i in idx]
    print("  moved (curve index, point number: px):", ", ".join("%d p%d: %.0f" % mv for mv in moved))
    print("wrote", out, "- look: vm.py wires WORK %s --curves %d%s --edits %s" % (
        layer["path"], ci, "" if level is None else " --level %d" % level, out))


APPLY = """
import oanim, json
edits = json.loads(%r)
class OA_OT_match_vectors(oanim.types.Operator):
    bl_idname = "oa.match_vectors"
    bl_label = %r
    bl_options = {"REGISTER", "UNDO"}
    def execute(self, context):
        ids = {l.id: l for l in oanim.data.layers.all()}
        # Coarsest level first. Each at its own edit level, so the finer detail rides along as it does
        # when that level is edited by hand; then each shape's edit level is put back.
        for e in sorted(edits, key=lambda e: -1 if e.get("level") is None else e["level"]):
            layer = ids[e["layer_id"]]
            level = e.get("level")
            if level is not None:
                was = layer.detail["shape_levels"]
                layer.set_detail_level(level)
            for point, x, y in e["points"]:
                layer.place_point(point, (x, y))
            for curve, index, side, x, y in e["handles"]:
                layer.place_handle(curve, index, side, (x, y), keep_other=True)
            if e.get("smooth"):
                # Smooth to Lower Level on just those points; the layer's selection and the active layer back after.
                layer.detail_sync()
                count = len(layer.points)
                picked = [i for i in range(count) if layer.points[i][2]]
                active = oanim.data.layers.active
                oanim.data.layers.active = layer
                layer.select_points(range(count), False)
                layer.select_points(e["smooth"], True)
                oanim.ops.mesh.detail_smooth(influence="1")
                layer.select_points(range(count), False)
                layer.select_points(picked, True)
                if active is not None:
                    oanim.data.layers.active = active
            if level is not None:
                layer.detail_sync()
                for shape, lv in enumerate(was):
                    layer.set_detail_level(lv, [shape])
                layer.detail_sync()
        return {"FINISHED"}
# The keys the edit made take the interpolation of the channel's other keys (the action's own: a
# reference retimed to even speed is keyed Linear), not auto-key's default.
def match_interpolation():
    frame = oanim.data.document.frame_current
    ids = {l.id: l for l in oanim.data.layers.all()}
    changed = 0
    for e in edits:
        layer = ids[e["layer_id"]]
        for point, x, y in e["points"]:
            path = "points[%%d].pos" %% point
            try:
                keys = layer.keyframes(path)
            except Exception:
                continue
            others = sorted((k for k in keys if k["frame"] != frame), key=lambda k: abs(k["frame"] - frame))
            here = [k for k in keys if k["frame"] == frame]
            if others and here and here[0]["interp"] != others[0]["interp"]:
                layer.keyframe_set(path, frame, interp=others[0]["interp"])
                changed += 1
    return changed
try:
    oanim.utils.unregister_class(OA_OT_match_vectors)
except Exception:
    pass
oanim.utils.register_class(OA_OT_match_vectors)
print(oanim.ops.oa.match_vectors(), oanim.data.document.frame_current)
oanim.data.document.frame_current = oanim.data.document.frame_current  # records the edit as keys
print("keys set to the action's interpolation:", match_interpolation())
"""


def cmd_apply(args):
    label = opt(args, "--label", "Match Vectors")
    set_frame = flag(args, "--set-frame")
    session, files = args[0], args[1:]
    edits = []
    for p in files:
        with open(p) as f:
            e = json.load(f)
        e.pop("preview", None)
        edits.append(e)
    frames = {e["frame"] for e in edits}
    now = oa_remote.run_json(session, "import oanim, json\nprint(json.dumps(oanim.data.document.frame_current))")
    if frames != {now} and not (set_frame and len(frames) == 1):
        sys.exit("The edits were made for frame %s but the app is on %s (--set-frame goes there for the edit "
                 "and back after)" % (sorted(frames), now))
    if frames != {now}:
        oa_remote.run(session, "import oanim\noanim.data.document.frame_current = %d" % list(frames)[0])
    try:
        print(oa_remote.run(session, APPLY % (json.dumps(edits), label)).strip())
    finally:
        if frames != {now}:
            oa_remote.run(session, "import oanim\noanim.data.document.frame_current = %d" % now)


def similarity(src, dst):
    """The move, turn and even scale taking src points onto dst best (least squares; no mirroring): a
    function of an (n, 2) array. One point pair: a move only."""
    src, dst = np.asarray(src, float), np.asarray(dst, float)
    if len(src) == 1:
        d = dst[0] - src[0]
        return lambda q: np.asarray(q, float) + d
    ms, md = src.mean(0), dst.mean(0)
    a, b = src - ms, dst - md
    cov = b.T @ a / len(src)
    u, sv, vt = np.linalg.svd(cov)
    e = np.diag([1.0, 1.0 if np.linalg.det(u @ vt) > 0 else -1.0])
    r = u @ e @ vt
    scale = float(np.trace(np.diag(sv) @ e) / max((a ** 2).sum() / len(src), 1e-24))
    return lambda q: (np.asarray(q, float) - ms) @ (scale * r).T + md


def cmd_follow(args):
    """Points that should keep their shape with the ones a fit moved (an ear's part behind the head, its inner
    line, an eyelid with its eye): moved, turned and scaled as those were, handles too."""
    out = opt(args, "--out", None)
    by = opt(args, "--by", None, lambda v: [int(x) for x in v.split(",")])
    work, edits_path, path = os.path.abspath(args[0]), args[1], args[2]
    which = args[3:]
    state = load_state(work)
    with open(edits_path) as f:
        edits = json.load(f)
    src_layer = find_layer(state, edits["layer"])
    old = {i: np.array(p) for i, p in enumerate(src_layer["points"])}
    pairs = [(old[pt], np.array([x, y])) for pt, x, y in edits["points"] if by is None or pt in by]
    if not pairs:
        sys.exit("The edits move none of those points")
    move = similarity([a for a, _ in pairs], [b for _, b in pairs])
    layer = find_layer(state, path)
    level = None if detail_top(layer) == 0 else 0
    moved_already = {pt for pt, _, _ in edits["points"]} if layer["path"] == src_layer["path"] else set()
    pick = None
    if which and which != ["all"]:
        pick = set()
        for w in which:
            if ":" in w:  # curve:index -> its point
                c, k = (int(v) for v in w.split(":"))
                pick.add(shown_curve(layer, c, level)[0][k][5])
            else:
                pick.add(int(w.lstrip("p")))
    result = {"layer": layer["path"], "layer_id": layer["id"], "frame": state["frame"], "level": level,
              "points": [], "handles": [], "preview": {}, "closed": {}}
    for ci in range(len(layer["curves"])):
        curve, closed = shown_curve(layer, ci, level)
        after = []
        for cp in curve:
            pt = cp[5]
            if (pick is None and pt not in moved_already) or (pick is not None and pt in pick):
                q, hi, ho = (move([v])[0] for v in (cp[0], cp[1], cp[2]))
                if [pt] not in [[r[0]] for r in result["points"]]:
                    result["points"].append([pt, float(q[0]), float(q[1])])
                result["handles"].append([ci, cp[6], "IN", float(hi[0]), float(hi[1])])
                result["handles"].append([ci, cp[6], "OUT", float(ho[0]), float(ho[1])])
                after.append([list(q), list(hi), list(ho), cp[3], cp[5], cp[6], cp[7]])
            else:
                after.append([list(cp[0]), list(cp[1]), list(cp[2]), cp[3], cp[5], cp[6], cp[7]])
        result["preview"][str(ci)] = after
        result["closed"][str(ci)] = closed
    out = out or os.path.join(work, "follow_%s.json" % layer["path"].split("/")[-1])
    with open(out, "w") as f:
        json.dump(result, f)
    print("%s: %d points follow (by %d moved points) -> %s" % (layer["path"], len(result["points"]), len(pairs), out))


def region_ellipse(work, name, scale=1.0):
    """The ellipse best fitting a region's outline (pixels): centre, the two axis vectors (half-lengths)."""
    import cv2
    path = name if os.path.exists(name) else os.path.join(work, name + ".json")
    with open(path) as f:
        data = json.load(f)
    px = np.array(data["pixels"], np.float32)
    (cx, cy), (w, h), angle = cv2.fitEllipse(px)
    t = math.radians(angle)
    u = np.array([math.cos(t), math.sin(t)]) * (w / 2) * scale
    v = np.array([-math.sin(t), math.cos(t)]) * (h / 2) * scale
    return np.array([cx, cy]), u, v


def ellipse_curve(centre, u, v, phases):
    """Points and exact-arc handles on an ellipse at angles `phases` (in order round the curve)."""
    n = len(phases)
    out = []
    for k, phi in enumerate(phases):
        nxt, prv = phases[(k + 1) % n], phases[k - 1]
        span_out = (nxt - phi + math.pi) % (2 * math.pi) - math.pi
        span_in = (phi - prv + math.pi) % (2 * math.pi) - math.pi
        if abs(span_out) < 1e-9:
            span_out = 2 * math.pi / n
        if abs(span_in) < 1e-9:
            span_in = 2 * math.pi / n
        pos = centre + math.cos(phi) * u + math.sin(phi) * v
        d = -math.sin(phi) * u + math.cos(phi) * v
        kout = 4.0 / 3.0 * math.tan(span_out / 4)
        kin = 4.0 / 3.0 * math.tan(span_in / 4)
        out.append((pos, pos - kin * d, pos + kout * d))
    return out


def cmd_ellipse(args):
    """A closed curve made a true ellipse fitted to a region: its points evenly round it, each keeping its
    place (the one at the top stays at the top), handles exact arcs. For eyes, irises, pupils, shines: a
    point-by-point fit to a pixel outline leaves ovals lumpy."""
    scale = opt(args, "--scale", 1.0, float)
    size_from = opt(args, "--size-from", None)
    out = opt(args, "--out", None)
    work, path, ci, region = os.path.abspath(args[0]), args[1], int(args[2]), args[3]
    state = load_state(work)
    m = Mapping(state)
    layer = find_layer(state, path)
    curve, closed = shown_curve(layer, ci, None if detail_top(layer) == 0 else 0)
    if not closed:
        sys.exit("ellipse is for closed curves")
    n = len(curve)
    centre, u, v = region_ellipse(work, region, scale)
    carry_px = None
    if size_from:
        # The part keeps its designed size (the rig's, in that snapshot: frame 0): the upright axis as long as
        # it was there, the level one no longer (a turn foreshortens it; nothing grows). The reference gives
        # only where it is and its tilt. The part is carried there whole (its points keep their roles), by a
        # map pairing the designed ellipse's axes with the new one's, level with level, upright with upright.
        import cv2
        base = load_state(size_from)
        bm = Mapping(base)
        bc, _ = shown_curve(find_layer(base, path), ci, None if detail_top(layer) == 0 else 0)
        bp = bm.to_px(sample_curve(bc, True))
        (bx, by), (bw, bh), bangle = cv2.fitEllipse(bp.astype(np.float32))
        bt = math.radians(bangle)
        bu = np.array([math.cos(bt), math.sin(bt)]) * bw / 2
        bv = np.array([-math.sin(bt), math.cos(bt)]) * bh / 2

        def upright(w):
            return abs(w[1]) / max(np.linalg.norm(w), 1e-12)

        b_up, b_level = (bu, bv) if upright(bu) >= upright(bv) else (bv, bu)
        n_up, n_level = (u, v) if upright(u) >= upright(v) else (v, u)
        n_up = n_up / np.linalg.norm(n_up) * np.linalg.norm(b_up)
        n_level = n_level / np.linalg.norm(n_level) * min(np.linalg.norm(n_level), np.linalg.norm(b_level))
        if np.dot(n_up, b_up) < 0:
            n_up = -n_up
        if np.dot(n_level, b_level) < 0:
            n_level = -n_level
        n_up, n_level = n_up * scale, n_level * scale
        amat = np.stack([n_level, n_up], 1) @ np.linalg.inv(np.stack([b_level, b_up], 1))
        bcentre = np.array([bx, by])
        carry_px = {"a": amat.tolist(), "from": bcentre.tolist(), "to": centre.tolist(), "base": size_from}
        u, v = n_level, n_up
    here = m.to_px([c[0] for c in curve])
    best = None
    for direction in (1, -1):
        for start in np.linspace(0, 2 * math.pi, 72, endpoint=False):
            phases = [start + direction * 2 * math.pi * k / n for k in range(n)]
            pts = np.array([centre + math.cos(f) * u + math.sin(f) * v for f in phases])
            cost = float(((pts - here) ** 2).sum())
            if best is None or cost < best[0]:
                best = (cost, phases)
    # Even round the ellipse, turned to where the points were; refined a little each way.
    phases = best[1]
    made = ellipse_curve(centre, u, v, phases)
    if carry_px is not None:
        amat, f0, t0 = np.array(carry_px["a"]), np.array(carry_px["from"]), np.array(carry_px["to"])
        made = [tuple(t0 + amat @ (bm.to_px([q])[0] - f0) for q in cp[:3]) for cp in bc]
    edits = {"layer": layer["path"], "layer_id": layer["id"], "frame": state["frame"],
             "level": None if detail_top(layer) == 0 else 0, "points": [], "handles": [], "preview": {}, "closed": {}}
    preview = []
    for cp, (pos, hin, hout) in zip(curve, made):
        dpos, din, dout = (m.to_doc([q])[0] for q in (pos, hin, hout))
        edits["points"].append([cp[5], float(dpos[0]), float(dpos[1])])
        edits["handles"].append([ci, cp[6], "IN", float(din[0]), float(din[1])])
        edits["handles"].append([ci, cp[6], "OUT", float(dout[0]), float(dout[1])])
        preview.append([list(dpos), list(din), list(dout), True, cp[5], cp[6], cp[7]])
    edits["preview"][str(ci)] = preview
    edits["closed"][str(ci)] = True
    if carry_px is not None:
        edits["carry_px"] = carry_px  # for carry: the same map takes the parts drawn with it
    out = out or os.path.join(work, "ellipse_%s_c%d.json" % (layer["path"].split("/")[-1], ci))
    with open(out, "w") as f:
        json.dump(edits, f)
    print("%s curve %d: an ellipse %.1f x %.1f px%s -> %s" % (layer["path"], ci, 2 * np.linalg.norm(u), 2 * np.linalg.norm(v),
                                                             "" if scale == 1 else " (scaled %g)" % scale, out))


def cmd_lid(args):
    """A lid's target: the reference's dark lid line measured round the eye white's ellipse (its thickness
    at each angle, from truly dark pixels only, not the soft halo), as a crescent whose drawn line touches the
    white without covering it: the curve's inner edge sits half the lid's line width outside the white."""
    dark = opt(args, "--dark", 80, float)
    band = opt(args, "--band", 9, float)
    work, path, white, name = os.path.abspath(args[0]), args[1], args[2], args[3]
    state = load_state(work)
    m = Mapping(state)
    layer = find_layer(state, path)
    style = layer["shapes"][0][2] if layer.get("shapes") else {}
    line_px = float(style.get("line_width", 0.0)) * m.px_per_unit if style.get("has_outline") else 0.0
    half = line_px / 2
    centre, u, v = region_ellipse(work, white)
    rgb = np.array(Image.open(os.path.join(work, "ref.png")).convert("RGB")).astype(float)
    lum = rgb @ np.array([0.299, 0.587, 0.114])

    def at(p):
        x, y = p
        x0, y0 = int(math.floor(x)), int(math.floor(y))
        fx, fy = x - x0, y - y0
        if not (0 <= x0 < lum.shape[1] - 1 and 0 <= y0 < lum.shape[0] - 1):
            return 255.0
        return float(lum[y0, x0] * (1 - fx) * (1 - fy) + lum[y0, x0 + 1] * fx * (1 - fy) +
                     lum[y0 + 1, x0] * (1 - fx) * fy + lum[y0 + 1, x0 + 1] * fx * fy)

    phis = np.linspace(-math.pi, math.pi, 360, endpoint=False)
    thick = []
    for f in phis:
        e = centre + math.cos(f) * u + math.sin(f) * v
        d = -math.sin(f) * u + math.cos(f) * v
        normal = np.array([d[1], -d[0]]) / np.linalg.norm(d)
        if np.dot(normal, e - centre) < 0:
            normal = -normal
        steps = np.arange(-1.0, band, 0.25)
        darks = [t for t in steps if at(e + normal * t) < dark]
        thick.append((min(darks), max(darks) + 0.25) if darks else None)
    upper = np.array([float((math.cos(f) * u + math.sin(f) * v)[1]) < 0 for f in phis])  # above the middle (pixels: y down)
    has = np.array([t is not None and t[0] < 3.0 for t in thick]) & upper
    if "--show" in sys.argv:
        for i in range(0, 360, 10):
            print("  %4.0f deg %s %s" % (math.degrees(phis[i]), "upper" if upper[i] else "     ", thick[i]))
    if not has.any():
        sys.exit("No dark line round the white (try a higher --dark)")
    # The run of angles with a lid that includes the darkest top (the one most dark around).
    k = int(np.argmax([0 if t is None or not upper[i] else t[1] - t[0] for i, t in enumerate(thick)]))
    lo = hi = k
    while has[(lo - 1) % 360] and (k - lo) < 359:
        lo -= 1
    while has[(hi + 1) % 360] and (hi - lo) < 359:
        hi += 1
    idxs = [i % 360 for i in range(lo, hi + 1)]
    outer_off = np.array([thick[i][1] for i in idxs])
    outer_off = np.convolve(np.r_[outer_off[:3][::-1], outer_off, outer_off[-3:][::-1]], np.ones(7) / 7, "valid")
    inner, outer = [], []
    for i, off in zip(idxs, outer_off):
        f = phis[i]
        e = centre + math.cos(f) * u + math.sin(f) * v
        d = -math.sin(f) * u + math.cos(f) * v
        normal = np.array([d[1], -d[0]]) / np.linalg.norm(d)
        if np.dot(normal, e - centre) < 0:
            normal = -normal
        inner.append(e + normal * half)
        outer.append(e + normal * max(off - half, half + 0.3))
    poly = np.array(inner + outer[::-1])
    with open(os.path.join(work, name + ".json"), "w") as f:
        json.dump({"outline": m.to_doc(poly).tolist(), "closed": True, "pixels": poly.tolist()}, f)
    print("lid %s: round the white from %.0f to %.0f degrees, %.1f to %.1f px thick (line %.1f px) -> %s" % (
        name, math.degrees(phis[idxs[0]]), math.degrees(phis[idxs[-1]]), float(outer_off.min()), float(outer_off.max()),
        line_px, os.path.join(work, name + ".json")))


def affine_fit(src, dst):
    """The affine map (stretch, shear, turn, move) taking src points onto dst best: a function of points."""
    src, dst = np.asarray(src, float), np.asarray(dst, float)
    a = np.hstack([src, np.ones((len(src), 1))])
    sol, *_ = np.linalg.lstsq(a, dst, rcond=None)
    return lambda q: np.hstack([np.asarray(q, float).reshape(-1, 2), np.ones((len(np.asarray(q).reshape(-1, 2)), 1))]) @ sol


def cmd_carry(args):
    """Carry parts drawn to go with a fitted one (an eye's lid, iris, pupil and shine with its white) from
    another frame (the setup, frame 0: how the rig is designed) by the stretch, turn and move that took the
    fitted part there to where the fit put it. Keeps the design when the reference is too blurry to trace
    small parts. --shift-to REGION --shift-by PATH: then move them all so PATH sits in REGION's middle (an
    iris where the reference's pupil is: the gaze)."""
    shift_to = opt(args, "--shift-to", None)
    shift_by = opt(args, "--shift-by", None)
    work, edits_path, base_dir = os.path.abspath(args[0]), args[1], os.path.abspath(args[2])
    paths = args[3:]
    state, base = load_state(work), load_state(base_dir)
    m = Mapping(state)
    with open(edits_path) as f:
        edits = json.load(f)
    guide = find_layer(base, edits["layer"])
    src = [guide["points"][pt] for pt, _, _ in edits["points"]]
    dst = [[x, y] for _, x, y in edits["points"]]
    if "carry_px" in edits:
        amat, f0, t0 = np.array(edits["carry_px"]["a"]), np.array(edits["carry_px"]["from"]), np.array(edits["carry_px"]["to"])
        bm = Mapping(base)

        def move(q):
            px = bm.to_px(np.asarray(q, float).reshape(-1, 2))
            return m.to_doc((px - f0) @ amat.T + t0)
    else:
        move = affine_fit(src, dst) if len(src) >= 3 else similarity(src, dst)
    shift = np.zeros(2)
    if shift_to:
        layer = find_layer(base, shift_by)
        moved = move(np.array([c[0] for c in shown_curve(layer, 0)[0]]))
        centre_px, _, _ = region_ellipse(work, shift_to)
        shift = m.to_doc([centre_px])[0] - moved.mean(0)
    for path in paths:
        layer, now = find_layer(base, path), find_layer(state, path)
        level = None if detail_top(layer) == 0 else 0
        result = {"layer": now["path"], "layer_id": now["id"], "frame": state["frame"], "level": level,
                  "points": [], "handles": [], "preview": {}, "closed": {}}
        done = set()
        for ci in range(len(layer["curves"])):
            curve, closed = shown_curve(layer, ci, level)
            after = []
            for cp in curve:
                q, hi, ho = (move([v])[0] + shift for v in cp[:3])
                if cp[5] not in done:
                    result["points"].append([cp[5], float(q[0]), float(q[1])])
                    done.add(cp[5])
                result["handles"].append([ci, cp[6], "IN", float(hi[0]), float(hi[1])])
                result["handles"].append([ci, cp[6], "OUT", float(ho[0]), float(ho[1])])
                after.append([list(q), list(hi), list(ho), cp[3], cp[5], cp[6], cp[7]])
            result["preview"][str(ci)] = after
            result["closed"][str(ci)] = closed
        out = os.path.join(work, "carry_%s.json" % now["path"].split("/")[-1])
        with open(out, "w") as f:
            json.dump(result, f)
        print("%s: carried from frame %s -> %s" % (now["path"], base["frame"], out))


def cmd_restore(args):
    """Edits putting layers back as an earlier snapshot had them (points and handles where they showed then),
    to undo one part's changes without undoing later steps for other parts."""
    work, base_dir, paths = os.path.abspath(args[0]), os.path.abspath(args[1]), args[2:]
    state, base = load_state(work), load_state(base_dir)
    for path in paths:
        layer, now = find_layer(base, path), find_layer(state, path)
        level = None if detail_top(layer) == 0 else 0
        result = {"layer": now["path"], "layer_id": now["id"], "frame": state["frame"], "level": level,
                  "points": [], "handles": [], "preview": {}, "closed": {}}
        done = set()
        for ci in range(len(layer["curves"])):
            curve, closed = shown_curve(layer, ci, level)
            for cp in curve:
                if cp[5] not in done:
                    result["points"].append([cp[5], float(cp[0][0]), float(cp[0][1])])
                    done.add(cp[5])
                result["handles"].append([ci, cp[6], "IN", float(cp[1][0]), float(cp[1][1])])
                result["handles"].append([ci, cp[6], "OUT", float(cp[2][0]), float(cp[2][1])])
        out = os.path.join(work, "restore_%s.json" % now["path"].split("/")[-1])
        with open(out, "w") as f:
            json.dump(result, f)
        print("%s: as in %s -> %s" % (now["path"], base_dir, out))


def cmd_score(args):
    import cv2
    work, name, target = os.path.abspath(args[0]), args[1], args[2]
    state = load_state(work)
    m = Mapping(state)
    solo = np.array(Image.open(os.path.join(work, name + ".png")).convert("RGBA"))[..., 3] > 128
    path = target if os.path.exists(target) else os.path.join(work, target + ".json")
    with open(path) as f:
        data = json.load(f)
    px = np.array(data["pixels"] if "pixels" in data else m.to_px(data["outline"]), np.int32)
    want = np.zeros(solo.shape, np.uint8)
    cv2.fillPoly(want, [px], 1)
    want = want.astype(bool)
    iou = (solo & want).sum() / max((solo | want).sum(), 1)
    contours, _ = cv2.findContours(solo.astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    c = max(contours, key=cv2.contourArea)[:, 0, :].astype(float)
    d = dist_to_poly(c[::3], np.vstack([px, px[:1]]).astype(float))
    print("%s vs %s: IoU %.3f, outline off by mean %.1f px, worst %.1f px; extra %d px, missing %d px" % (
        name, target, iou, d.mean(), d.max(), (solo & ~want).sum(), (want & ~solo).sum()))
    vis = np.full(solo.shape + (3,), 255, np.uint8)
    vis[solo & want] = (200, 200, 200)
    vis[solo & ~want] = (230, 60, 60)
    vis[want & ~solo] = (60, 90, 230)
    Image.fromarray(vis).save(os.path.join(work, "score_%s.png" % name))
    print("wrote", os.path.join(work, "score_%s.png" % name), "(red: drawn but not in the reference, blue: missing)")


if __name__ == "__main__":
    commands = {"snapshot": cmd_snapshot, "solo": cmd_solo, "wires": cmd_wires, "region": cmd_region, "stroke": cmd_stroke, "follow": cmd_follow, "ellipse": cmd_ellipse,
                "lid": cmd_lid, "carry": cmd_carry, "restore": cmd_restore,
                "fit": cmd_fit, "apply": cmd_apply, "score": cmd_score}
    if len(sys.argv) < 2 or sys.argv[1] not in commands:
        sys.exit(__doc__)
    commands[sys.argv[1]](sys.argv[2:])
