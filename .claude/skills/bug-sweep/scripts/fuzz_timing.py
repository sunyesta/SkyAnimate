"""Seeded random operations on Path Timing curves (Key::timing), with invariants checked as it goes. Runs in the app:

    set OA_FUZZ_SEED=7  OA_FUZZ_OPS=300  OA_FUZZ_OUT=<folder>
    OpenAnimate.exe --background --python fuzz_timing.py

The scene is built here (no file): line layers with position keys (adjacent frames, frame 1 next to the setup frame,
a long stretch, a stretch on 2s, one on 3s, a breakdown), a layer keyed on the same frames as another (Time Together),
a Key Whole Drawing layer, and a bone. Every op is drawn with concrete arguments, logged to <OUT>/tfuzz_<seed>.json
(written before each op as "crash-or-hang"), so a failure replays exactly (OA_FUZZ_REPLAY=<json>) and shrinks
(shrink_timing.py).

Checks after every op: no exception, finite numbers, a change only with an undo step, CANCELLED changes nothing, every
stored timing curve valid (>= 2 points, finite, x strictly increasing, ends (0,0) and (1,1), handles pointing the right
way); after every undo/redo the document is what it was when that step was pushed. Every OA_FUZZ_EVERY ops: undo k
then redo k exact; frames played finite, keys arrive on their frames, a frame shows the same whatever frame came
before; save/open exact.
"""
import faulthandler
import json
import math
import os
import random
import sys
import tempfile
import traceback
import zipfile

import oanim

ops, wm = oanim.ops, oanim.wm
L = oanim.data.layers
SEED = int(os.environ.get("OA_FUZZ_SEED", "1"))
COUNT = int(os.environ.get("OA_FUZZ_OPS", "200"))
EVERY = int(os.environ.get("OA_FUZZ_EVERY", "10"))
OUT = os.environ.get("OA_FUZZ_OUT", tempfile.gettempdir())
REPLAY = os.environ.get("OA_FUZZ_REPLAY", "")
HANG = int(os.environ.get("OA_FUZZ_HANG", "120"))
QUIET = os.environ.get("OA_FUZZ_QUIET", "1") == "1"
REACH = os.environ.get("OA_FUZZ_REACH", "0") == "1"   # stored handles past their segment fail too
VIEW = {'frame_current', 'ui_open', 'ui_panel_order', 'viewport_view', 'texts', 'name'}
TMP = OUT
# What undo leaves as it is (view_state_restore in builtin_ops.cc).
UNDO_KEEPS = {"texts", "overlays", "ui_open", "view", "motion_paths", "auto_key", "auto_key_mode", "time_together"}


def D():
    return oanim.data.document


class Failure(Exception):
    pass


# --- Snapshots -----------------------------------------------------------------------------------------------------
def refresh():
    """Evaluates the frame shown again (a frame away and back): a document's shown values are the frame's."""
    doc = D()
    f = doc.frame_current
    doc.frame_current = f + 1
    doc.frame_current = f


STALE = []


def snap(fresh=True):
    if fresh:
        raw = snap(False)
        refresh()
        now = snap(False)
        d = diff(raw, now)
        if d and len(STALE) < 20:
            STALE.append((LAST[0] + (" TT" if D().time_together else " noTT"), d[:3]))
        return now
    p = os.path.join(TMP, f"tf_snap_{os.getpid()}.oanim")
    ops.wm.save_copy(filepath=p)
    with zipfile.ZipFile(p) as z:
        d = json.loads(z.read('document.json'))['document']
    for k in VIEW:
        d.pop(k, None)
    return d


def diff(a, b, path="", out=None, eps=1e-5):
    out = [] if out is None else out
    if len(out) > 8:
        return out
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b), key=str):
            if k not in a or k not in b:
                out.append(f"{path}.{k}: {'missing before' if k not in a else 'missing after'}")
            else:
                diff(a[k], b[k], f"{path}.{k}", out, eps)
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append(f"{path}: len {len(a)} vs {len(b)}")
        for i, (x, y) in enumerate(zip(a, b)):
            diff(x, y, f"{path}[{i}]", out, eps)
    elif isinstance(a, float) or isinstance(b, float):
        try:
            if not (abs(a - b) <= eps * max(1.0, abs(a), abs(b))):
                out.append(f"{path}: {a} vs {b}")
        except TypeError:
            out.append(f"{path}: {a!r} vs {b!r}")
    elif a != b:
        out.append(f"{path}: {str(a)[:80]!r} vs {str(b)[:80]!r}")
    return out


def edits(a, b):
    return [d for d in diff(a, b) if not any(w in d.split(":")[0] for w in ("select", "active", "picked"))]


def bad_numbers(d, path="", out=None):
    out = [] if out is None else out
    if isinstance(d, dict):
        for k, v in d.items():
            bad_numbers(v, f"{path}.{k}", out)
    elif isinstance(d, list):
        for i, v in enumerate(d):
            bad_numbers(v, f"{path}[{i}]", out)
    elif isinstance(d, float) and not math.isfinite(d):
        out.append(path)
    return out


def curves_in(d, path="", out=None):
    """Every stored timing curve: (path, points)."""
    out = [] if out is None else out
    if isinstance(d, dict):
        if isinstance(d.get("timing"), list) and d["timing"] and isinstance(d["timing"][0], list):
            out.append((path, d["timing"], d))
        for k, v in d.items():
            curves_in(v, f"{path}.{k}", out)
    elif isinstance(d, list):
        for i, v in enumerate(d):
            curves_in(v, f"{path}[{i}]", out)
    return out


def curve_problems(d):
    probs = []
    for path, pts, key in curves_in(d):
        where = f"{path} (frame {key.get('frame')})"
        if len(pts) < 2:
            probs.append(f"curve-invalid: {where} has {len(pts)} points")
            continue
        if any(not math.isfinite(v) for p in pts for v in p[:6]):
            probs.append(f"curve-invalid: {where} not finite {pts}")
            continue
        for i in range(1, len(pts)):
            if not pts[i][0] > pts[i - 1][0]:
                probs.append(f"curve-invalid: {where} x not increasing at {i}: {pts[i-1][0]} then {pts[i][0]}")
        if abs(pts[0][0]) > 1e-4 or abs(pts[0][1]) > 1e-4 or abs(pts[-1][0] - 1) > 1e-4 or abs(pts[-1][1] - 1) > 1e-4:
            probs.append(f"curve-invalid: {where} ends {pts[0][:2]} {pts[-1][:2]}")
        for i, p in enumerate(pts):
            if i > 0 and p[2] > 1e-6:
                probs.append(f"handle-back: {where} point {i}'s in handle points forward {p[2:4]}")
            if i + 1 < len(pts) and p[4] < -1e-6:
                probs.append(f"handle-back: {where} point {i}'s out handle points back {p[4:6]}")
        for i in range(1, len(pts) - 1):
            p = pts[i]
            if len(p) > 6 and p[6]:
                a, b = (p[2], p[3]), (p[4], p[5])
                la, lb = math.hypot(*a), math.hypot(*b)
                if la > 1e-6 and lb > 1e-6:
                    cross = (a[0] * b[1] - a[1] * b[0]) / (la * lb)
                    dot = (a[0] * b[0] + a[1] * b[1]) / (la * lb)
                    if abs(cross) > 2e-3 or dot > 0:
                        probs.append(f"aligned-off: {where} point {i} is aligned but its handles {a} {b} are not in "
                                     f"line (cross {cross:.4f}, dot {dot:.4f})")
        if REACH:
            for i in range(len(pts) - 1):
                dx = pts[i + 1][0] - pts[i][0]
                reach = max(p for p in (pts[i][4], -pts[i + 1][2]))
                if reach > dx * (1 + 1e-3) + 1e-6:
                    probs.append(f"handle-reach: {where} segment {i} dx {dx:.6g}, handle reaches {reach:.6g}")
        if probs:
            break
    return probs


def active_step():
    return wm.undo_history()[1]


# --- The scene -----------------------------------------------------------------------------------------------------
def pick(layer):
    for other in L:
        other.selected = other.id == layer.id
    L.active = layer


def keyed_layer(name, keys, interp=None, setup=(-1.0, 0.0)):
    ops.layer.new()
    layer = L.active
    layer.name = name
    layer.add_curve([(0.0, 0.0), (0.1, 0.0)])
    layer.key_whole_drawing = False
    D().frame_current = 0
    layer.location_x, layer.location_y = setup
    pick(layer)
    for f, (x, y) in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(layer.id)) == {'FINISHED'}, (name, f)
        if f not in [k["frame"] for k in layer.keyframes("position")]:
            D().frame_current = f
            layer.keyframe_insert("position", f)
        if interp:
            layer.keyframe_set("position", f, interp=interp)
    return layer


def setup_tool():
    D().motion_paths = True
    ops.wm.tool_motion_path()
    ops.anim.motion_path_brush_set(brush="EDIT", proportional="0")


def build():
    assert ops.wm.read_homefile() == {'FINISHED'}
    doc = D()
    doc.motion_paths = True
    doc.auto_key = False
    doc.frame_end = 120
    setup_tool()
    targets = []
    a = keyed_layer("A", {10: (0.0, 0.0), 11: (0.05, 0.0), 40: (1.0, 0.0), 70: (1.4, 0.0)}, interp="LINEAR",
                    setup=(-1.0, 0.0))
    a.keyframe_set("position", 11, interval=2)
    targets.append({"layer": a.id})
    b = keyed_layer("B", {5: (0.0, 0.0), 20: (1.0, 1.0), 27: (1.5, 0.5), 35: (2.0, 0.0), 60: (3.0, 1.0)},
                    setup=(-1.0, -1.0))
    b.keyframe_set("position", 27, type="BREAKDOWN")
    b.keyframe_set("position", 35, interval=3)
    targets.append({"layer": b.id})
    e = keyed_layer("E", {1: (0.2, 0.3), 3: (0.5, 0.2), 30: (1.0, 1.0), 31: (1.1, 1.0)}, setup=(0.0, 0.0))
    targets.append({"layer": e.id})
    f = keyed_layer("F", {10: (0.0, 1.0), 11: (0.1, 1.2), 40: (1.0, 2.0), 70: (2.0, 1.0)}, interp="LINEAR",
                    setup=(-1.0, 1.0))
    targets.append({"layer": f.id})
    # Key Whole Drawing: two points keyed at different frames.
    doc.auto_key = True
    doc.auto_freeze_keys = False
    ops.layer.new()
    w = L.active
    w.name = "W"
    assert w.key_whole_drawing
    doc.frame_current = 0
    w.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
    doc.frame_current = 10
    w.set_point(0, (0.0, 0.1))
    doc.frame_current = 30
    w.set_point(0, (1.0, 0.1))
    w.set_point(1, (1.0, 1.0))
    doc.frame_current = 45
    w.set_point(2, (2.5, 0.5))
    doc.frame_current = 0
    doc.auto_key = False
    targets.append({"layer": w.id, "drawing": 1})
    # A bone.
    ops.layer.new_bone()
    rig = L.active
    rig.name = "R"
    pick(rig)
    assert ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="") == {'FINISHED'}
    rig.select_bone("arm")
    for fr, ang in ((10, 0.2), (25, 1.0), (50, 0.4)):
        assert ops.anim.motion_path_move(frame=str(fr), x=str(math.cos(ang)), y=str(math.sin(ang)), layer=str(rig.id),
                                         bone="arm") == {'FINISHED'}
    targets.append({"layer": rig.id, "bone": "arm"})
    for o in L:
        o.selected = o.id in (a.id, b.id, e.id, f.id, w.id, rig.id)
    L.active = a
    doc.frame_current = 0
    return targets


def reopen(path):
    assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
    setup_tool()


def rebase():
    p = os.path.join(TMP, f"tf_base_{os.getpid()}.oanim")
    ops.wm.save_copy(filepath=p)
    f = D().frame_current
    reopen(p)
    D().frame_current = f


# --- Reading targets ---------------------------------------------------------------------------------------------
def layer_of(t):
    return next((l for l in L.all() if l.id == t["layer"]), None)


def channels(t):
    layer = layer_of(t)
    if layer is None:
        return []
    if "bone" in t:
        return [f'bones["{t["bone"]}"].angle']
    if t.get("drawing"):
        return [f"points[{i}].pos" for i in range(layer.point_count)]
    return ["position"]


def key_frames(t):
    layer = layer_of(t)
    fs = set()
    for c in channels(t):
        try:
            fs |= {k["frame"] for k in layer.keyframes(c)}
        except Exception:
            pass
    return sorted(fs)


def curve_size(t, frame):
    """Points on the curve of the stretch frame is in (2 when even), best guess."""
    layer = layer_of(t)
    best = 2
    for c in channels(t):
        try:
            ks = layer.keyframes(c)
        except Exception:
            continue
        after = [k for k in ks if k["frame"] > frame]
        if after:
            best = max(best, len(after[0].get("timing", [])) or 2)
    return best


def targs(t):
    a = {"layer": str(t["layer"])}
    if "bone" in t:
        a["bone"] = t["bone"]
    if t.get("drawing"):
        a["drawing"] = "1"
    return a


def where(t, frame):
    """Document place of what t traces on `frame` (the frame restored)."""
    doc = D()
    keep = doc.frame_current
    doc.frame_current = frame
    layer = layer_of(t)
    try:
        if "bone" in t:
            b = next(b for b in layer.bones if b["name"] == t["bone"])
            p = tuple(b["tail"][:2])
        elif t.get("drawing"):
            p = tuple(layer.points[0][:2])
        else:
            p = (layer.location_x, layer.location_y)
    finally:
        doc.frame_current = keep
    return p


# --- Drawing ops ---------------------------------------------------------------------------------------------------
TARGETS = []
TABLE = {}
LAST = ["start"]
EMPTY = []
PLAYS = [0]


def op(name, weight, kind="op"):
    def deco(fn):
        TABLE[name] = (weight, kind, fn)
        return fn
    return deco


ODD = ["nan", "inf", "-inf", "", "abc", "1e30", "-1e30", "0", "1", "1e-7", "0.9999999", "-0.5", "1.5"]


ODD_SHARE = float(os.environ.get("OA_FUZZ_ODD", "0.08"))


def num(rng, lo, hi, odd=None):
    odd = ODD_SHARE if odd is None else max(odd, ODD_SHARE)
    if rng.random() < odd:
        return rng.choice(ODD)
    return repr(round(rng.uniform(lo, hi), 4))


def a_target(rng):
    return rng.randrange(len(TARGETS))


def a_frame(rng, ti):
    fs = key_frames(TARGETS[ti])
    if rng.random() < 0.12 or len(fs) < 2:
        return rng.randint(0, 80)
    i = rng.randrange(len(fs) - 1)
    s, e = fs[i], fs[i + 1]
    if e - s >= 2 and rng.random() < 0.9:
        return rng.randint(s + 1, e - 1)
    return rng.choice([s, e])


def base(rng):
    ti = a_target(rng)
    return ti, a_frame(rng, ti)


@op("frame", 6, "py")
def _(rng):
    return {"f": rng.randint(0, 80)}


@op("time_together", 2, "py")
def _(rng):
    return {"on": rng.random() < 0.5}


@op("undo", 6, "undo")
def _(rng):
    return {"n": rng.choice([1, 1, 1, 2, 3])}


@op("redo", 4, "undo")
def _(rng):
    return {"n": rng.choice([1, 1, 2])}


@op("save_open", 1, "meta")
def _(rng):
    return {}


@op("point_add", 10)
def _(rng):
    ti, f = base(rng)
    a = {"x": num(rng, 0.0, 1.0)}
    if rng.random() < 0.4:
        a["y"] = num(rng, -0.3, 1.3)
    if rng.random() < 0.2:
        a["to_x"] = num(rng, -0.1, 1.1)
    return {"t": ti, "frame": f, "a": a}


@op("point_move", 8)
def _(rng):
    ti, f = base(rng)
    n = curve_size(TARGETS[ti], f)
    return {"t": ti, "frame": f, "a": {"index": str(rng.randint(-1, n)), "x": num(rng, -0.1, 1.1),
                                       "y": num(rng, -0.3, 1.3)}}


@op("point_delete", 5)
def _(rng):
    ti, f = base(rng)
    n = curve_size(TARGETS[ti], f)
    if rng.random() < 0.5:
        return {"t": ti, "frame": f, "a": {"index": str(rng.randint(0, n))}}
    return {"t": None, "frame": f, "a": {}}


@op("handle_move", 8)
def _(rng):
    ti, f = base(rng)
    n = curve_size(TARGETS[ti], f)
    return {"t": ti, "frame": f, "a": {"index": str(rng.randint(0, n - 1)), "side": rng.choice(["IN", "OUT", "OUT", "UP"]),
                                       "x": num(rng, -0.6, 0.6), "y": num(rng, -0.6, 0.6),
                                       "free": rng.choice(["0", "0", "1"])}}


@op("select", 10, "sel")
def _(rng):
    ti, f = base(rng)
    n = curve_size(TARGETS[ti], f)
    r = rng.random()
    if r < 0.5:
        a = {"index": str(rng.randint(0, n)), "part": rng.choice(["POINT", "POINT", "IN", "OUT"])}
        if rng.random() < 0.4:
            a["extend"] = "1"
    elif r < 0.75:
        x0, x1 = sorted([rng.uniform(-0.1, 1.1), rng.uniform(-0.1, 1.1)])
        y0, y1 = sorted([rng.uniform(-0.3, 1.3), rng.uniform(-0.3, 1.3)])
        a = {"box": "%g,%g,%g,%g" % (x0, y0, x1, y1), "mode": rng.choice(["SET", "ADD", "SUB"])}
        if rng.random() < 0.05:
            a["box"] = rng.choice(["1,2,3", "a,b,c,d", "0,0,1,1,", "nan,0,1,1"])
    else:
        a = {"action": rng.choice(["ALL", "ALL", "NONE", "BAD"])}
    return {"t": ti, "frame": f, "a": a}


@op("select_all", 2, "sel")
def _(rng):
    return {"all": rng.random() < 0.5}


def a_pre(rng):
    """Often a pick first (in the same op: picking is no undo step): points, a handle, or all."""
    if rng.random() < 0.3:
        return None
    ti, f = base(rng)
    n = curve_size(TARGETS[ti], f)
    picks = []
    for j in range(rng.choice([1, 1, 2, 3])):
        if rng.random() < 0.2:
            picks.append({"action": "ALL"})
        else:
            p = {"index": str(rng.randint(0, n - 1)), "part": rng.choice(["POINT", "POINT", "IN", "OUT"])}
            if j:
                p["extend"] = "1"
            picks.append(p)
    return {"t": ti, "frame": f, "picks": picks}


@op("transform", 12)
def _(rng):
    mode = rng.choice(["MOVE", "MOVE", "ROTATE", "SCALE", "SPIN"])
    a = {"mode": mode}
    if mode == "MOVE":
        a["x"] = num(rng, -6, 6)
        a["y"] = num(rng, -0.4, 0.4)
    elif mode == "ROTATE":
        a["angle"] = num(rng, -200, 200)
    else:
        a["factor"] = num(rng, -1.5, 3.0)
        if rng.random() < 0.4:
            a["axis"] = rng.choice(["X", "Y", "Z"])
    if rng.random() < 0.4:
        a["aspect"] = num(rng, 0.0, 40, odd=0.2)
    return {"a": a, "pre": a_pre(rng)}


@op("handles_type", 6)
def _(rng):
    a = {"type": rng.choice(["ALIGNED", "BROKEN", "FLAT", "AUTO"])}
    if rng.random() < 0.4:
        a["aspect"] = num(rng, 0, 40, odd=0.2)
    return {"a": a, "pre": a_pre(rng)}


@op("decimate", 5)
def _(rng):
    ti, f = base(rng)
    return {"t": ti, "frame": f, "a": {"ratio": num(rng, 0, 1, odd=0.15)}}


@op("sculpt", 12)
def _(rng):
    ti, f = base(rng)
    fs = key_frames(TARGETS[ti])
    s = max([k for k in fs if k < f], default=0)
    e = min([k for k in fs if k > f], default=s + 10)
    pts = []
    x, y = rng.uniform(s - 1, e + 1), rng.uniform(-0.1, 1.1)
    for _ in range(rng.randint(1, 5)):
        pts.append("%g,%g" % (x, y))
        x += rng.uniform(-3, 3)
        y += rng.uniform(-0.2, 0.2)
    path = ";".join(pts)
    if rng.random() < 0.04:
        path = rng.choice(["", "1", "a,b", "5,nan", "5,0.5;inf,0.5"])
    a = {"brush": rng.choice(["GRAB", "GRAB", "SMOOTH", "PINCH"]), "path": path, "radius": num(rng, 0.3, 10),
         "strength": num(rng, 0, 1), "lock_x": rng.choice(["0", "1"]), "dyntopo": rng.choice(["0", "1"])}
    if rng.random() < 0.7:
        a["aspect"] = num(rng, 1, 40)
    return {"t": ti, "frame": f, "a": a}


@op("shape", 6)
def _(rng):
    ti, f = base(rng)
    return {"t": ti, "frame": f, "a": {"shape": rng.choice(["EVEN", "EASE_IN", "EASE_OUT", "EASE_IN_OUT", "WOBBLE"])}}


@op("speed_stroke", 6)
def _(rng):
    ti, f = base(rng)
    brush = rng.choice(["BLEND_SPEED", "PINCH", "SLIDE"])
    try:
        p = where(TARGETS[ti], f)
    except Exception:
        p = (0.0, 0.0)
    pts = []
    x, y = p
    for _ in range(rng.randint(1, 3) if brush != "SLIDE" else 2):
        pts.append("%g,%g" % (x + rng.uniform(-0.05, 0.05), y + rng.uniform(-0.05, 0.05)))
        x += rng.uniform(-0.2, 0.2)
    a = {"brush": brush, "points": ";".join(pts), "radius": num(rng, 0.05, 0.6), "strength": num(rng, 0, 1)}
    if rng.random() < 0.3:
        a["invert"] = "1"
    return {"t": ti, "frame": f, "a": a}


@op("slide", 8)
def _(rng):
    ti, f = base(rng)
    a = {"progress": num(rng, -0.2, 1.2), "proportional": rng.choice(["0", "1"])}
    if a["proportional"] == "1":
        a["size"] = num(rng, 1, 12)
    return {"t": ti, "frame": f, "a": a}


@op("scale_spacing", 6)
def _(rng):
    ti, f = base(rng)
    a = {"factor": num(rng, 0.2, 2.5), "proportional": rng.choice(["0", "1", "1"])}
    if rng.random() < 0.5:
        a["size"] = num(rng, 1, 15)
    return {"t": ti, "frame": f, "a": a}


@op("even", 3)
def _(rng):
    ti, f = base(rng)
    return {"t": ti, "frame": f, "a": {}}


@op("key_add", 4)
def _(rng):
    ti, f = base(rng)
    a = {}
    if rng.random() < 0.5:
        a["type"] = "KEYFRAME"
    return {"t": ti, "frame": f, "a": a}


@op("key_delete", 3)
def _(rng):
    ti = a_target(rng)
    fs = key_frames(TARGETS[ti])
    f = rng.choice(fs) if fs and rng.random() < 0.85 else rng.randint(0, 80)
    return {"t": ti, "frame": f, "a": {}}


@op("key_interp", 4)
def _(rng):
    ti = a_target(rng)
    fs = key_frames(TARGETS[ti])
    f = rng.choice(fs) if fs else rng.randint(0, 80)
    a = {}
    r = rng.random()
    if r < 0.4:
        a["interp"] = rng.choice(["LINEAR", "SMOOTH", "EASE_IN_OUT", "STEP", "BEZIER"])
    elif r < 0.7:
        a["type"] = rng.choice(["KEYFRAME", "BREAKDOWN"])
    else:
        a["interval"] = rng.choice(["1", "2", "3"])
    return {"t": ti, "frame": f, "a": a}


@op("keys_move", 5)
def _(rng):
    ti = a_target(rng)
    fs = key_frames(TARGETS[ti])
    if not fs:
        return None
    k = rng.choice(fs)
    a = {"frames": str(rng.choice([-5, -2, -1, 1, 1, 2, 3, 7]))}
    if rng.random() < 0.2:
        a["ripple"] = "1"
    elif rng.random() < 0.2:
        a["slide"] = "1"
    return {"t": ti, "k0": k, "k1": k if rng.random() < 0.8 else rng.choice(fs), "a": a}


# --- Applying ----------------------------------------------------------------------------------------------------
def draw(rng):
    names = list(TABLE)
    while True:
        name = rng.choices(names, weights=[TABLE[n][0] for n in names])[0]
        args = TABLE[name][2](rng)
        if args is not None:
            args["op"] = name
            return args


def apply(o):
    name = o["op"]
    doc = D()
    if name == "frame":
        doc.frame_current = o["f"]
        return None
    if name == "time_together":
        doc.time_together = o["on"]
        return None
    if name in ("undo", "redo"):
        r = None
        for _ in range(o["n"]):
            r = (ops.ed.undo if name == "undo" else ops.ed.redo)()
        return r
    if name == "save_open":
        return None   # handled by the loop
    if name == "select_all":
        return (ops.anim.timing_select_all if o["all"] else ops.anim.timing_deselect_all)()
    if name in ("transform", "handles_type"):
        pre = o.get("pre")
        if pre and layer_of(TARGETS[pre["t"]]) is not None:
            for p in pre["picks"]:
                ops.anim.timing_select(frame=str(pre["frame"]), **targs(TARGETS[pre["t"]]), **p)
        return (ops.anim.timing_transform if name == "transform" else ops.anim.timing_handles_type)(**o["a"])
    a = dict(o.get("a", {}))
    if o.get("t") is not None:
        t = TARGETS[o["t"]]
        if layer_of(t) is None:
            return "skip"
        a.update(targs(t))
    if name == "keys_move":
        layer = layer_of(TARGETS[o["t"]])
        ops.anim.keys_deselect_all()
        lo, hi = sorted([o["k0"], o["k1"]])
        ops.anim.keys_select(frame_start=str(lo), frame_end=str(hi), layers=str(layer.id))
        return ops.anim.keys_offset(**o["a"])
    if "frame" in o:
        a["frame"] = str(o["frame"])
    fn = {
        "point_add": ops.anim.timing_point_add,
        "point_move": ops.anim.timing_point_move,
        "point_delete": ops.anim.timing_point_delete,
        "handle_move": ops.anim.timing_handle_move,
        "select": ops.anim.timing_select,
        "decimate": ops.anim.timing_decimate,
        "sculpt": ops.anim.timing_sculpt,
        "shape": ops.anim.motion_path_stretch_shape,
        "speed_stroke": ops.anim.motion_path_stroke,
        "slide": ops.anim.motion_path_slide,
        "scale_spacing": ops.anim.motion_path_scale_spacing,
        "even": ops.anim.motion_path_even_spacing,
        "key_add": ops.anim.motion_path_key_add,
        "key_delete": ops.anim.motion_path_key_delete,
        "key_interp": ops.anim.motion_path_key_interp,
    }[name]
    if name == "point_delete" and o.get("t") is None:
        a.pop("frame", None)
    if name == "speed_stroke":
        a.pop("frame", None)   # (the frame was set by prep, before the snapshot)
    return fn(**a)


def prep(o):
    """What an op needs set first that isn't its own change (taken before the 'before' snapshot)."""
    if o["op"] == "speed_stroke":
        D().frame_current = o["frame"]


# --- The checks --------------------------------------------------------------------------------------------------
def bez_y(pts, u):
    """The stored curve at time u, when each segment's handles lie within it (else None: the curve clamps them)."""
    i = 0
    while i + 2 < len(pts) and pts[i + 1][0] <= u:
        i += 1
    a, b = pts[i], pts[i + 1]
    dx = b[0] - a[0]
    if dx <= 0 or a[4] < 0 or b[2] > 0 or a[4] - b[2] > dx * (1 + 1e-6):
        return None
    p0, p1, p2, p3 = (a[0], a[1]), (a[0] + a[4], a[1] + a[5]), (b[0] + b[2], b[1] + b[3]), (b[0], b[1])

    def at(t):
        v = 1 - t
        return [p0[k] * v * v * v + p1[k] * 3 * v * v * t + p2[k] * 3 * v * t * t + p3[k] * t * t * t for k in (0, 1)]
    lo, hi = 0.0, 1.0
    for _ in range(50):
        mid = 0.5 * (lo + hi)
        if at(mid)[0] < u:
            lo = mid
        else:
            hi = mid
    return at(0.5 * (lo + hi))[1]


def curve_plays(seen):
    """A (straight along x): a Custom stretch between plain keys plays its stored curve, every frame."""
    a = layer_of(TARGETS[0])
    if a is None:
        return
    ks = sorted(a.keyframes("position"), key=lambda k: k["frame"])
    xs = [k["value"][0] for k in ks]
    if any(abs(k["value"][1]) > 1e-6 for k in ks) or any(y <= x for x, y in zip(xs, xs[1:])):
        return
    for k0, k1 in zip(ks, ks[1:]):
        s, e = k0["frame"], k1["frame"]
        if s < 0 or e > 81 or e - s < 2 or k1.get("interp") != "CUSTOM" or k0.get("interval", 1) != 1:
            continue
        if k0.get("type") == "BREAKDOWN" or k1.get("type") == "BREAKDOWN" or k0.get("hold") or k1.get("hold"):
            continue
        pts = k1.get("timing")
        if not pts:
            continue
        pts = [(p[0], p[1], p[2][0], p[2][1], p[3][0], p[3][1]) for p in pts]   # (the API's (x, y, in, out, aligned))
        if any(abs(v) > 20 for p in pts for v in p):
            continue   # (a curve put way out by an odd number: its floats say little)
        x0, x1 = k0["value"][0], k1["value"][0]
        for f in range(s + 1, e):
            y = bez_y(pts, (f - s) / (e - s))
            if y is None or not 0.0 <= y <= 1.0:
                continue
            want = x0 + (x1 - x0) * y
            PLAYS[0] += 1
            got = seen[(a.id, f)][0]
            if abs(got - want) > 2e-3 * max(1.0, abs(x1 - x0)):
                raise Failure(f"curve-plays: A on frame {f} (stretch {s}-{e}) is at x {got:.5f}, its curve says "
                              f"{want:.5f} (progress {y:.5f}, {len(pts)} points)")


def play_check():
    """Frames played: finite, keys arrive on their frames, the same whatever frame came before."""
    doc = D()
    keep = doc.frame_current
    layers = [layer_of(t) for t in TARGETS if "bone" not in t and not t.get("drawing")]
    layers = [l for l in layers if l is not None]
    seen = {}
    for f in range(0, 82):
        doc.frame_current = f
        for l in layers:
            p = (l.location_x, l.location_y)
            if not all(math.isfinite(v) for v in p):
                raise Failure(f"play-nonfinite: {l.name} on frame {f} at {p}")
            seen[(l.id, f)] = p
    for l in layers:
        for k in l.keyframes("position"):
            f = k["frame"]
            if f <= 0 or f > 81:
                continue
            v = k["value"]
            p = seen[(l.id, f)]
            if max(abs(p[0] - v[0]), abs(p[1] - v[1])) > 1e-4:
                raise Failure(f"key-missed: {l.name} on its key frame {f} is at {p}, the key says {tuple(v)}")
    curve_plays(seen)
    for f in (2, 7, 15, 23, 33, 41, 55, 66):
        for before in (0, f + 17):
            doc.frame_current = before
            doc.frame_current = f
            for l in layers:
                p = (l.location_x, l.location_y)
                q = seen[(l.id, f)]
                if max(abs(p[0] - q[0]), abs(p[1] - q[1])) > 1e-5:
                    raise Failure(f"order: {l.name} on frame {f} after frame {before} is {p}, played in order {q}")
    doc.frame_current = keep


def main():
    global TARGETS
    plan = json.load(open(REPLAY, encoding="utf-8"))["ops"] if REPLAY else None
    rng = random.Random(SEED)
    TARGETS = build()
    rebase()
    fail_path = os.path.join(OUT, f"tfuzz_fail_{SEED}.json" if not REPLAY else "tfuzz_replay_fail.json")
    hang_file = open(os.path.join(OUT, f"tfuzz_hang_{SEED if not REPLAY else 'replay'}.txt"), "w")
    log = []
    pushed = {active_step(): (snap(), D().frame_current)}   # the document as each undo step left it
    dirty = False
    notes = {}

    def write_fail(text):
        json.dump({"seed": SEED, "failure": text, "ops": log}, open(fail_path, "w", encoding="utf-8"), indent=1)

    total = len(plan) if REPLAY else COUNT
    try:
        for n in range(total):
            o = plan[n] if REPLAY else draw(rng)
            log.append(o)
            LAST[0] = f"{n}:{o['op']}"
            write_fail(f"crash-or-hang: {o['op']} crashed or did not finish in {HANG}s")
            faulthandler.dump_traceback_later(HANG, exit=True, file=hang_file)
            prep(o)
            before, step_before = snap(), active_step()
            kind = TABLE[o["op"]][1]
            if o["op"] == "save_open":
                p = os.path.join(TMP, f"tf_rt_{os.getpid()}.oanim")
                ops.wm.save_copy(filepath=p)
                f = D().frame_current
                reopen(p)
                D().frame_current = f
                d = diff(before, snap())
                if d:
                    raise Failure(f"roundtrip: reopened differs {d[:4]}")
                pushed, dirty = {active_step(): (snap(), D().frame_current)}, False
                r = None
            else:
                try:
                    r = apply(o)
                except Failure:
                    raise
                except Exception as e:
                    raise Failure(f"exception: {o['op']} raised {type(e).__name__}: {e}\n{traceback.format_exc()}")
            if r != "skip" and o["op"] != "save_open":
                after, step_after = snap(), active_step()
                bn = bad_numbers(after)
                if bn:
                    raise Failure(f"non-finite: after {o['op']} at {bn[:3]}")
                cp = curve_problems(after)
                if cp:
                    raise Failure(cp[0] + f" (after {o['op']})")
                if kind in ("op", "sel"):
                    changed = edits(before, after)
                    if r == {'CANCELLED'} and changed:
                        raise Failure(f"cancel-changed: {o['op']} returned CANCELLED but changed {changed[:3]}")
                    if changed and step_after == step_before:
                        raise Failure(f"no-step change: {o['op']} changed the document without an undo step {changed[:3]}")
                    if kind == "sel" and changed:
                        raise Failure(f"select-changed: {o['op']} picking changed the document {changed[:3]}")
                    if step_after != step_before:
                        if not changed and r != {'CANCELLED'}:
                            notes[o["op"]] = notes.get(o["op"], 0) + 1
                            EMPTY.append(o)
                        pushed = {k: v for k, v in pushed.items() if k < step_after}
                        pushed[step_after] = (after, D().frame_current)
                        dirty = False
                elif kind == "py":
                    if edits(before, after):
                        dirty = True
                elif kind == "undo":
                    if step_after in pushed:
                        want, at = pushed[step_after]
                        keep = D().frame_current
                        D().frame_current = at
                        now = snap()
                        D().frame_current = keep
                        d = [x for x in edits(want, now) if x.split(":")[0].split(".")[1].split("[")[0] not in UNDO_KEEPS]
                        if d:
                            raise Failure(f"undo-state: after {o['op']} to step {step_after} the document isn't as "
                                          f"that step left it: {d[:4]}")
                    dirty = False
                if not QUIET:
                    print(f"{n:4d} f{D().frame_current:<3d} {o['op']:14s} -> {r}", flush=True)
            if (n + 1) % EVERY == 0 or n + 1 == total:
                write_fail(f"crash-or-hang: the checks after op {n} crashed or did not finish in {HANG}s")
                names, at = wm.undo_history()
                k = min(5, at)
                if not dirty and k > 0:
                    f = D().frame_current
                    top = snap()
                    for _ in range(k):
                        ops.ed.undo()
                    for _ in range(k):
                        ops.ed.redo()
                    D().frame_current = f
                    d = edits(top, snap())
                    if d:
                        raise Failure(f"undo-chain: undo {k} then redo {k} differs: {d[:4]}")
                play_check()
                s1, f = snap(), D().frame_current
                p = os.path.join(TMP, f"tf_rt_{os.getpid()}.oanim")
                ops.wm.save_copy(filepath=p)
                reopen(p)
                D().frame_current = f
                d = diff(s1, snap())
                if d:
                    raise Failure(f"roundtrip: reopened differs {d[:4]}")
                pushed, dirty = {active_step(): (snap(), D().frame_current)}, False
            faulthandler.cancel_dump_traceback_later()
            os.remove(fail_path)
        print(f"tfuzz seed {SEED}: {len(log)} ops clean; empty steps {notes}; curve frames checked {PLAYS[0]}", flush=True)
        if EMPTY:
            json.dump(EMPTY, open(os.path.join(OUT, f"tfuzz_empty_{SEED}.json"), "w"), indent=0)
        for w, d in STALE:
            print(f"   stale shown values after {w}: {d}", flush=True)
    except Failure as f:
        faulthandler.cancel_dump_traceback_later()
        write_fail(str(f))
        print(f"!!! FUZZ FAIL after {len(log)} ops: {f}\n    log: {fail_path}", flush=True)
        sys.exit(1)
    if not REPLAY:
        json.dump({"seed": SEED, "ops": log}, open(os.path.join(OUT, f"tfuzz_{SEED}.json"), "w", encoding="utf-8"),
                  indent=1)


try:
    main()
finally:
    for kind in ("snap", "base", "rt"):
        try:
            os.remove(os.path.join(TMP, f"tf_{kind}_{os.getpid()}.oanim"))
        except OSError:
            pass
