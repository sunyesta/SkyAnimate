"""Bug-sweep harness for Open Animate: run inside the app (OpenAnimate.exe --background --python your_probe.py).

    import sys; sys.path.insert(0, r"<this folder>")
    from harness import *
    load()                      # opens $OA_SWEEP_FILE (a private copy, never the user's file)
    run("G on Head_X", lambda: ops.armature.select_all())   # undo/redo exactness, bad numbers, no-step changes
    order_check("after G")      # frames visited in different orders give the same pose
    roundtrip()                 # save a copy, open it, nothing differs
    summary()                   # exits 1 if anything was found (so a probe is also a repro)

Everything is compared through the saved document (snap(): a save_copy's document.json), so it sees what a save
would keep. Keys in VIEW are view state that undo deliberately leaves alone and are ignored.
"""
import json
import math
import os
import sys
import tempfile
import traceback
import zipfile

import oanim

doc, L, ops, wm = oanim.data.document, oanim.data.layers, oanim.ops, oanim.wm
FILE = os.environ.get("OA_SWEEP_FILE", "")
TMP = os.environ.get("OA_SWEEP_TMP", tempfile.gettempdir())
VIEW = {'frame_current', 'ui_open', 'ui_panel_order', 'viewport_view', 'texts', 'name'}
FOUND = []


def load(path=None):
    """Opens the sweep file (OA_SWEEP_FILE, or `path`)."""
    p = path or FILE
    assert p, "set OA_SWEEP_FILE to a private copy of the file to sweep"
    assert ops.wm.open_mainfile(filepath=p) == {'FINISHED'}, p


def snap():
    p = os.path.join(TMP, f"oa_sweep_{os.getpid()}.oanim")
    ops.wm.save_copy(filepath=p)
    with zipfile.ZipFile(p) as z:
        d = json.loads(z.read('document.json'))['document']
    for k in VIEW:
        d.pop(k, None)
    return d


def diff(a, b, path="", out=None, eps=1e-5):
    """Where two snaps differ (at most ~9 places), relative tolerance eps on floats."""
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
    """diff without selection and the active layer or bone: picking things is never an undo step (as Blender)."""
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


def bug(name, what):
    FOUND.append((name, what))
    print(f"!!! BUG? {name}: {what}", flush=True)


def active_step():
    """The undo stack's active step index: it changes exactly when an operator pushes a step (or undo/redo moves)."""
    return wm.undo_history()[1]


def rebase():
    """Python data edits (bone_set, set_point, keyframe_insert on a layer) are not undo steps: undoing the next
    operator takes them back too, which looks like an undo bug and isn't. Before checking an operator after such
    edits, save and reopen so they are the undo base (the Motion Path tool, if it was on, is turned on again)."""
    f, path_tool = doc.frame_current, oanim.wm.motion_paths() if hasattr(oanim.wm, "motion_paths") else None
    p = os.path.join(TMP, f"oa_base_{os.getpid()}.oanim")
    ops.wm.save_copy(filepath=p)
    assert ops.wm.open_mainfile(filepath=p) == {'FINISHED'}
    if path_tool:
        ops.wm.tool_motion_path()
    doc.frame_current = f


def run(name, fn, undo=True, expect_change=None):
    """Runs fn (an operator call). Checks: no exception, finite numbers, a change only with an undo step, undo back
    to before exactly and redo to after exactly. Returns fn's result."""
    try:
        s0, h0, f0 = snap(), active_step(), doc.frame_current
        r = fn()
        s1, h1, f1 = snap(), active_step(), doc.frame_current
    except Exception as e:
        bug(name, f"raised {type(e).__name__}: {e}")
        traceback.print_exc()
        return None
    bn = bad_numbers(s1)
    if bn:
        bug(name, f"non-finite numbers at {bn[:4]}")
    changed, pushed = bool(diff(s0, s1)), h1 != h0
    if changed and not pushed:
        bug(name, f"changed with no undo step (r={r}) {diff(s0, s1)[:3]}")
    if pushed and not changed and r != {'CANCELLED'}:
        print(f"    note {name}: an undo step that changes nothing (r={r})")
    if r == {'CANCELLED'} and changed:
        bug(name, f"returned CANCELLED but changed the document {diff(s0, s1)[:3]}")
    if expect_change is not None and changed != expect_change:
        bug(name, f"expected change={expect_change}, got {changed} r={r}")
    if undo and pushed:
        for _ in range(h1 - h0):
            ops.ed.undo()
        doc.frame_current = f0
        d = diff(s0, snap())
        if d:
            bug(name, f"undo doesn't restore: {d[:4]}")
        for _ in range(h1 - h0):
            ops.ed.redo()
        doc.frame_current = f1
        d = diff(s1, snap())
        if d:
            bug(name, f"redo doesn't restore: {d[:4]}")
    print(f"  ok {name} -> {r} changed={changed} pushed={pushed}", flush=True)
    return r


def roundtrip(name="save/open"):
    s1, f = snap(), doc.frame_current
    p = os.path.join(TMP, f"oa_rt_{os.getpid()}.oanim")
    ops.wm.save_copy(filepath=p)
    assert ops.wm.open_mainfile(filepath=p) == {'FINISHED'}
    doc.frame_current = f
    d = diff(s1, snap())
    if d:
        bug(name, f"reopened differs: {d[:4]}")


def pose():
    """What the frame shows: every bone's head and tail (and a knob's), every layer's shown points."""
    out = {}
    for layer in L.all():
        if layer.type == "BONE":
            for b in layer.bones:
                out[(layer.id, b["name"])] = tuple(b.get("head", ())) + tuple(b.get("tail", ())) + \
                    tuple(b.get("knob_shown", ()))
        try:
            out[(layer.id, "shown")] = [tuple(p) for curve in layer.shown() for p in curve]
        except Exception:
            pass
    return out


def flat(v):
    """Every number in a nest of lists and tuples, in order."""
    if isinstance(v, (list, tuple)):
        return [x for item in v for x in flat(item)]
    return [v] if isinstance(v, (int, float)) and not isinstance(v, bool) else []


def pose_diff(a, b, eps=1e-4):
    worst, where = 0.0, None
    for k in a:
        if k not in b:
            return float("inf"), k
        flat_x, flat_y = flat(a[k]), flat(b[k])
        if len(flat_x) != len(flat_y):
            return float("inf"), k
        for u, v in zip(flat_x, flat_y):
            if abs(u - v) > worst:
                worst, where = abs(u - v), k
    return worst, where


def order_check(name, frames=(1, 5, 12, 24, 37), eps=1e-4):
    """Each frame's pose reached from frame 0, from the frame before it and from far away must agree."""
    keep = doc.frame_current
    for f in frames:
        seen = []
        for before in (0, max(0, f - 1), f + 20):
            doc.frame_current = before
            doc.frame_current = f
            seen.append(pose())
        for other in seen[1:]:
            w, k = pose_diff(seen[0], other)
            if w > eps:
                bug(name, f"frame {f}'s pose depends on the frame before it: {w:.5f} at {k}")
                break
    doc.frame_current = keep


def summary():
    print("=== FOUND", len(FOUND), flush=True)
    for n, w in FOUND:
        print(" *", n, "::", w)
    if FOUND:
        sys.exit(1)
