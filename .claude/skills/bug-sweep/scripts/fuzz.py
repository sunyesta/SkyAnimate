"""Seeded random operations on a file's bones and points, with invariants checked as it goes. Runs inside the app:

    set OA_SWEEP_FILE=<private copy.oanim>  OA_FUZZ_SEED=7  OA_FUZZ_OPS=300  OA_FUZZ_OUT=<folder>
    OpenAnimate.exe --background --python fuzz.py

Every op is drawn with concrete arguments first (the layer, the bones or points it picks, the numbers), then applied,
and logged to <OUT>/fuzz_<seed>.json, so a failure can be replayed exactly (OA_FUZZ_REPLAY=<that json>) and shrunk
(shrink.py). Exits 1 on the first failure, writing the ops up to it and what failed.

Ops (about 45 kinds, weighted): frames, auto-key, undo and redo; bone moves, turns, scales and lengths (Python, and the
G R S operators with Without Children and Only Bones), knobs inside and past their box (and snapped), add, delete,
duplicate, extrude, subdivide, parent set and clear (and by name), convert types, clear transforms; actions entered,
left and made; points moved (Python and G R S), deleted, dissolved, subdivided, split, added, extruded; weights
assigned, removed, automatic, cleaned, normalized and cleared; Parent Bone with its deform modes; a new drawing in the
rig; keys inserted and deleted.

Checks after every op: no exception, finite numbers, an operator's change comes with an undo step, CANCELLED changes
nothing, and entering an action and leaving it at once changes nothing. Every OA_FUZZ_EVERY ops (default 25): undoing
the last steps and redoing them gives the same document, frames reached in different orders show the same pose,
save/open changes nothing, and a point weighted wholly to one plain bone moves rigidly with it when the bone turns.
Python data edits (bone_set, set_point, frame changes) are not undo steps by design, so the undo checks wait until an
operator has pushed after them.

A watchdog (OA_FUZZ_HANG seconds per op, default 150) dumps every thread's stack to <OUT>/fuzz_hang_<seed>.txt and
exits. The log is written before each op naming it as a "crash-or-hang" (removed once it finishes), so a crash or a
hang leaves a log that shrink.py shrinks like any other failure.
"""
import faulthandler
import json
import math
import os
import random
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import *  # noqa: E402,F403
import harness  # noqa: E402

SEED = int(os.environ.get("OA_FUZZ_SEED", "1"))
COUNT = int(os.environ.get("OA_FUZZ_OPS", "200"))
EVERY = int(os.environ.get("OA_FUZZ_EVERY", "25"))
OUT = os.environ.get("OA_FUZZ_OUT", harness.TMP)
REPLAY = os.environ.get("OA_FUZZ_REPLAY", "")
FRAMES = int(os.environ.get("OA_FUZZ_FRAMES", "60"))
HANG = int(os.environ.get("OA_FUZZ_HANG", "150"))
QUIET = os.environ.get("OA_FUZZ_QUIET", "1") == "1"


class Failure(Exception):
    pass


def rigs():
    return [l for l in L.all() if l.type == "BONE"]


def editable(layer):
    """A reference's points are its original's, locked unless its Edit Points is on."""
    return not layer.reference_source or bool(layer.reference.get("edit_points"))


def drawings():
    return [l for l in L.all() if l.type == "VECTOR" and l.point_count > 0 and editable(l)]


def find_layer(lid):
    return next((l for l in L.all() if l.id == lid), None)


def bone_names(rig):
    return [b["name"] for b in rig.bones]


def bone(rig, name):
    return next((b for b in rig.bones if b["name"] == name), None)


def frame(rng):
    x = rng.random()
    return 0 if x < 0.3 else rng.choice([1, 2]) if x < 0.4 else rng.randint(1, FRAMES)


# --- Drawing ops: concrete arguments from the document as it is ------------------------------------------------------
# Each entry: name -> (weight, kind, draw(rng) -> args dict or None). Kinds: "op" an operator (an undo step expected
# with a change), "py" a Python data edit (no step), "undo", "meta" (checks its own invariant).
TABLE = {}


def op(name, weight, kind="op"):
    def deco(fn):
        TABLE[name] = (weight, kind, fn)
        return fn
    return deco


def a_rig(rng):
    rs = rigs()
    return rng.choice(rs) if rs else None


def a_pick(rng, rig, most=3):
    """Bones to pick on `rig` for an operator: usually one, sometimes a few, sometimes a head or tail only."""
    names = bone_names(rig)
    if not names:
        return None
    k = 1 if rng.random() < 0.7 else min(len(names), rng.randint(2, most))
    return {"rig": rig.id, "bones": rng.sample(names, k), "part": rng.choice(["BONE"] * 6 + ["HEAD", "TAIL"])}


def a_points(rng, most=6):
    ds = drawings()
    if not ds:
        return None
    d = rng.choice(ds)
    return {"layer": d.id, "idx": sorted(rng.sample(range(d.point_count), rng.randint(1, min(most, d.point_count))))}


@op("frame", 10, "py")
def _(rng):
    return {"f": frame(rng)}


@op("auto_key", 2, "py")
def _(rng):
    return {"on": rng.random() < 0.75}


@op("undo", 5, "undo")
def _(rng):
    return {"n": rng.choice([1, 1, 1, 2, 3, 5])}


@op("redo", 3, "undo")
def _(rng):
    return {"n": rng.choice([1, 1, 2, 4])}


@op("bone_set", 10, "py")
def _(rng):
    rig = a_rig(rng)
    if not rig or not rig.bones:
        return None
    what = rng.choice(["position", "angle", "angle", "scale", "length"])
    v = {"position": lambda: [rng.gauss(0, 0.06), rng.gauss(0, 0.06)], "angle": lambda: rng.uniform(-1.5, 1.5),
         "scale": lambda: [rng.uniform(0.5, 1.6), rng.uniform(0.5, 1.6)], "length": lambda: rng.uniform(0.6, 1.5)}[what]()
    return {"rig": rig.id, "bone": rng.choice(bone_names(rig)), "what": what, "v": v}


@op("knob", 6, "py")
def _(rng):
    ks = [(r.id, b["name"], b.get("controller_box")) for r in rigs() for b in r.bones if b.get("type") == "CONTROLLER_2D"]
    if not ks:
        return None
    rid, name, box = rng.choice(ks)
    (lx, ly), (hx, hy) = box if box else ((-80, -80), (80, 80))
    past = rng.choice([1.0, 1.0, 1.0, 1.6])
    return {"rig": rid, "bone": name, "k": [rng.uniform(lx, hx) * past, rng.uniform(ly, hy) * past],
            "snap": rng.random() < 0.15}


@op("G", 7)
def _(rng):
    rig = a_rig(rng)
    p = rig and a_pick(rng, rig)
    return p and dict(p, d=[rng.uniform(-0.1, 0.1), rng.uniform(-0.1, 0.1)], wc=rng.random() < 0.2,
                      ob=rng.random() < 0.15)


@op("R", 6)
def _(rng):
    rig = a_rig(rng)
    p = rig and a_pick(rng, rig)
    return p and dict(p, deg=rng.uniform(-120, 120), wc=rng.random() < 0.2)


@op("S", 4)
def _(rng):
    rig = a_rig(rng)
    p = rig and a_pick(rng, rig)
    return p and dict(p, s=rng.choice([rng.uniform(0.5, 1.8), rng.uniform(0.5, 1.8), -1.0, 0.0001]))


@op("rotate_bones", 2, "py")
def _(rng):
    rig = a_rig(rng)
    p = rig and a_pick(rng, rig)
    return p and dict(p, a=rng.uniform(-1, 1))


@op("bone_add", 2)
def _(rng):
    rig = a_rig(rng)
    if not rig:
        return None
    h = [rng.uniform(-1, 1), rng.uniform(-1, 1)]
    return {"rig": rig.id, "head": h, "tail": [h[0] + rng.uniform(-0.4, 0.4), h[1] + rng.uniform(0.05, 0.4)],
            "parent": rng.choice(bone_names(rig) + [""])}


for _name, _w in (("bone_delete", 1.5), ("bone_duplicate", 1.5), ("bone_extrude", 1), ("bone_subdivide", 1),
                  ("parent_clear", 1.5), ("convert", 2), ("clear_transform", 1.5), ("key_insert_bones", 3),
                  ("key_delete_bones", 2)):
    def _make(name):
        def draw_it(rng):
            rig = a_rig(rng)
            p = rig and a_pick(rng, rig)
            if not p:
                return None
            extra = {"bone_subdivide": {"n": rng.randint(1, 3)}, "parent_clear": {"t": rng.choice(["CLEAR", "DISCONNECT"])},
                     "convert": {"t": rng.choice(["REGULAR", "PIN", "CONTROLLER_2D"])},
                     "clear_transform": {"w": rng.choice(["clear_location", "clear_rotation", "clear_scale"])}}
            return dict(p, **extra.get(name, {}))
        return draw_it
    op(_name, _w)(_make(_name))


@op("parent_set", 1.5)
def _(rng):
    rig = a_rig(rng)
    names = bone_names(rig) if rig else []
    if len(names) < 2:
        return None
    b, to = rng.sample(names, 2)
    return {"rig": rig.id, "bones": [b], "part": "BONE", "to": to, "t": rng.choice(["OFFSET", "CONNECTED"])}


@op("parent_by_name", 1.5)
def _(rng):
    rig = a_rig(rng)
    names = bone_names(rig) if rig else []
    if not names:
        return None
    return {"rig": rig.id, "bone": rng.choice(names), "parent": rng.choice(names) if rng.random() < 0.8 else ""}


def actions_of(rig):
    try:
        return [a["name"] for a in rig.actions if a.get("kind") in ("ACTION", "ACTION_BONE")]
    except Exception:
        return []


@op("action_set", 3)
def _(rng):
    rig = a_rig(rng)
    if not rig:
        return None
    if doc.action and rng.random() < 0.6:
        return {"rig": rig.id, "name": ""}
    acts = actions_of(rig)
    return {"rig": rig.id, "name": rng.choice(acts)} if acts else None


@op("action_new", 0.5)
def _(rng):
    rig = a_rig(rng)
    return rig and {"rig": rig.id, "name": rng.choice(["fz", "wave"])}


@op("action_peek", 2, "meta")
def _(rng):
    rig = a_rig(rng)
    acts = actions_of(rig) if rig else []
    return {"rig": rig.id, "name": rng.choice(acts)} if acts and not doc.action else None


@op("point_move", 6, "py")
def _(rng):
    p = a_points(rng, 1)
    return p and dict(p, d=[rng.gauss(0, 0.05), rng.gauss(0, 0.05)])


@op("mG", 5)
def _(rng):
    p = a_points(rng)
    return p and dict(p, d=[rng.uniform(-0.05, 0.05), rng.uniform(-0.05, 0.05)])


@op("mR", 3)
def _(rng):
    p = a_points(rng)
    return p and dict(p, deg=rng.uniform(-90, 90))


@op("mS", 3)
def _(rng):
    p = a_points(rng)
    return p and dict(p, s=[rng.uniform(0.5, 1.5), rng.uniform(0.5, 1.5)])


for _name, _w in (("point_delete", 2), ("point_dissolve", 1), ("point_subdivide", 1.5), ("point_extrude", 1),
                  ("weight_remove", 1.5), ("weight_automatic", 1), ("weight_other", 1), ("key_insert", 3),
                  ("key_delete", 2), ("align_handles", 1)):
    def _make(name):
        def draw_it(rng):
            p = a_points(rng)
            if not p:
                return None
            extra = {"point_subdivide": {"n": rng.randint(1, 2)}, "weight_remove": {"keep": rng.random() < 0.5},
                     "weight_other": {"w": rng.choice(["clean", "normalize", "clear", "remove_all"])}}
            return dict(p, **extra.get(name, {}))
        return draw_it
    op(_name, _w)(_make(_name))


@op("weight_assign", 4)
def _(rng):
    rig, p = a_rig(rng), a_points(rng)
    if not rig or not p or not rig.bones:
        return None
    return dict(p, rig=rig.id, bone=rng.choice(bone_names(rig)), w=rng.choice(["1", "1", "0.5", "0", str(rng.random())]))


@op("split", 1.5, "py")
def _(rng):
    ds = [d for d in drawings() if d.curve_count > 0]
    if not ds:
        return None
    d = rng.choice(ds)
    return {"layer": d.id, "c": rng.randrange(d.curve_count), "s": rng.randint(0, 3), "t": rng.uniform(0.2, 0.8)}


@op("point_add", 1)
def _(rng):
    ds = drawings()
    return ds and {"layer": rng.choice(ds).id, "at": [rng.uniform(-0.3, 0.3), rng.uniform(0.3, 0.9)]}


@op("parent_bone", 2)
def _(rng):
    rig, ds = a_rig(rng), drawings()
    if not rig or not ds or not rig.bones:
        return None
    return {"layer": rng.choice(ds).id, "bone": rng.choice(bone_names(rig)) if rng.random() < 0.8 else "",
            "keep": rng.random() < 0.3, "deform": rng.choice(["", "", "LAYER", "POINTS"])}


@op("new_drawing", 0.7)
def _(rng):
    rig = a_rig(rng)
    if not rig:
        return None
    c = [rng.uniform(-0.3, 0.3), rng.uniform(0.4, 0.8)]
    pts = [[c[0] + rng.uniform(-0.05, 0.05) + 0.04 * k, c[1] + rng.uniform(-0.05, 0.05)] for k in range(rng.randint(2, 5))]
    return {"rig": rig.id, "pts": pts, "closed": rng.random() < 0.4}


def draw(rng):
    names = list(TABLE)
    weights = [TABLE[n][0] for n in names]
    for _ in range(50):
        name = rng.choices(names, weights)[0]
        args = TABLE[name][2](rng)
        if args:
            return dict(args, op=name)
    return {"op": "frame", "f": frame(rng)}


# --- Applying them: "skip" where the document no longer has what the op names (shrunk replays) -------------------------
def pick_bones(op):
    """Makes the op's rig active with just its bones picked; False when one is gone."""
    rig = find_layer(op["rig"])
    if rig is None or any(b not in bone_names(rig) for b in op.get("bones", [])):
        return None
    L.active = rig
    ops.armature.select_none()
    for k, b in enumerate(op.get("bones", [])):
        rig.select_bone(b, extend=k > 0, part=op.get("part", "BONE"))
    return rig


def pick_points(op):
    layer = find_layer(op["layer"])
    if layer is None:
        return None
    idx = [i for i in op["idx"] if i < layer.point_count]
    if not idx:
        return None
    L.active = layer
    ops.mesh.select_none()
    layer.select_points(idx)
    return layer


def apply(op):
    o = op["op"]
    if o == "frame":
        doc.frame_current = op["f"]
        return None
    if o == "auto_key":
        doc.auto_key = op["on"]
        return None
    if o in ("undo", "redo"):
        for _ in range(op["n"]):
            (ops.ed.undo if o == "undo" else ops.ed.redo)()
        return None
    if o in ("bone_set", "knob", "parent_by_name"):
        rig = find_layer(op["rig"])
        b = rig and bone(rig, op["bone"])
        if not b:
            return "skip"
        if o == "knob":
            if b.get("type") != "CONTROLLER_2D" or doc.frame_current == 0:
                return "skip"   # (on frame 0 a knob is at rest, refused by design)
            kw = {"knob_snap": True} if op["snap"] else {}
            rig.bone_set(op["bone"], knob=tuple(op["k"]), **kw)
            return None
        if o == "parent_by_name":
            if op["parent"] and not bone(rig, op["parent"]):
                return "skip"
            L.active = rig
            return ops.armature.bone_parent_set(bone=op["bone"], parent=op["parent"])
        w, v = op["what"], op["v"]
        if w == "position":
            rig.bone_set(op["bone"], position=(b["position"][0] + v[0], b["position"][1] + v[1]))
        elif w == "angle":
            rig.bone_set(op["bone"], angle=b["angle"] + v)
        elif w == "scale":
            s = b.get("scale", (1.0, 1.0))
            rig.bone_set(op["bone"], scale=(s[0] * v[0], s[1] * v[1]))
        else:
            rig.bone_set(op["bone"], length=b["length"] * v)
        return None
    if o in ("action_set", "action_new", "action_peek"):
        rig = find_layer(op["rig"])
        if rig is None:
            return "skip"
        L.active = rig
        if o == "action_new":
            return ops.anim.action_new(name=op["name"])
        if o == "action_set":
            return ops.anim.action_set(name=op["name"])
        if op["name"] not in actions_of(rig) or doc.action:
            return "skip"
        before = snap()
        ops.anim.action_set(name=op["name"])
        ops.anim.action_set(name="")
        d = [x for x in edits(before, snap()) if not x.startswith(".action")]
        if d:
            raise Failure(f"action-peek: entering {op['name']!r} and leaving at once changed {d[:4]}")
        return None
    if o == "bone_add":
        rig = find_layer(op["rig"])
        if rig is None or (op["parent"] and not bone(rig, op["parent"])):
            return "skip"
        L.active = rig
        ops.armature.select_none()
        return ops.armature.bone_add(head_x=str(op["head"][0]), head_y=str(op["head"][1]), tail_x=str(op["tail"][0]),
                                     tail_y=str(op["tail"][1]), parent=op["parent"])
    if o == "new_drawing":
        rig = find_layer(op["rig"])
        if rig is None:
            return "skip"
        L.active = rig
        r = ops.layer.new()
        if r != {'FINISHED'}:
            return r
        L.active.add_curve([tuple(p) for p in op["pts"]], closed=op["closed"], fill=op["closed"])
        return ops.layer.move_to_group(group=str(rig.id))
    if "bones" in op:
        rig = pick_bones(op)
        if rig is None:
            return "skip"
        if o == "parent_set":
            if not bone(rig, op["to"]):
                return "skip"
            rig.select_bone(op["to"], extend=True)
            return ops.armature.parent_set(type=op["t"])
        if o == "rotate_bones":
            rig.rotate_bones(op["a"])
            return None
        call = {
            "G": lambda: ops.armature.translate(move_x=str(op["d"][0]), move_y=str(op["d"][1]),
                                                without_children="1" if op["wc"] else "0",
                                                only_bones="1" if op["ob"] else "0"),
            "R": lambda: ops.armature.rotate(angle=str(op["deg"]), without_children="1" if op["wc"] else "0"),
            "S": lambda: ops.armature.resize(scale=str(op["s"])),
            "bone_delete": lambda: ops.armature.bone_delete(),
            "bone_duplicate": lambda: ops.armature.duplicate_move(),
            "bone_extrude": lambda: ops.armature.extrude(),
            "bone_subdivide": lambda: ops.armature.subdivide(number_cuts=str(op["n"])),
            "parent_clear": lambda: ops.armature.parent_clear(type=op["t"]),
            "convert": lambda: ops.armature.bone_convert(type=op["t"]),
            "clear_transform": lambda: getattr(ops.armature, op["w"])(),
            "key_insert_bones": lambda: ops.anim.keyframe_insert(),
            "key_delete_bones": lambda: ops.anim.keyframe_delete(),
        }[o]
        return call()
    if o in ("point_move", "split", "point_add", "parent_bone"):
        layer = find_layer(op["layer"])
        if layer is None:
            return "skip"
        if o == "point_move":
            i = op["idx"][0]
            if i >= layer.point_count:
                return "skip"
            x, y = layer.points[i][:2]
            layer.set_point(i, (x + op["d"][0], y + op["d"][1]))
            return None
        if o == "split":
            if op["c"] >= layer.curve_count or len(layer.curve_points(op["c"])) < 2:
                return "skip"
            layer.split_segment(op["c"], op["s"] % (len(layer.curve_points(op["c"])) - 1), op["t"])
            return None
        L.active = layer
        if o == "point_add":
            return ops.mesh.primitive_point_add(x=op["at"][0], y=op["at"][1])
        if op["bone"] and not any(bone(r, op["bone"]) for r in rigs()):
            return "skip"
        kw = {"bone": op["bone"]}
        if op["keep"]:
            kw["keep_animation"] = "1"
        if op["deform"]:
            kw["deform"] = op["deform"]
        return ops.layer.parent_bone_set(**kw)
    if o == "weight_assign":
        rig = find_layer(op["rig"])
        if rig is None or not bone(rig, op["bone"]):
            return "skip"
        L.active = rig
        ops.armature.select_none()
        rig.select_bone(op["bone"])
    layer = pick_points(op)
    if layer is None:
        return "skip"
    call = {
        "mG": lambda: ops.mesh.translate(move_x=str(op["d"][0]), move_y=str(op["d"][1])),
        "mR": lambda: ops.mesh.rotate(angle=str(op["deg"])),
        "mS": lambda: ops.mesh.resize(scale_x=str(op["s"][0]), scale_y=str(op["s"][1])),
        "point_delete": lambda: ops.mesh.delete(),
        "point_dissolve": lambda: ops.mesh.dissolve_points(),
        "point_subdivide": lambda: ops.mesh.subdivide(number_cuts=str(op["n"])),
        "point_extrude": lambda: ops.mesh.extrude(),
        "weight_assign": lambda: ops.weight.assign(weight=op["w"]),
        "weight_remove": lambda: ops.weight.remove(keep_animation="1" if op["keep"] else "0"),
        "weight_automatic": lambda: ops.weight.automatic(),
        "weight_other": lambda: getattr(ops.weight, op["w"])(),
        "key_insert": lambda: ops.anim.keyframe_insert(),
        "key_delete": lambda: ops.anim.keyframe_delete(),
        "align_handles": lambda: ops.mesh.handles_align(),
    }[o]
    return call()


# --- The checks -------------------------------------------------------------------------------------------------------
def check_op(op, r, before, step_before):
    after, step_after = snap(), active_step()
    bn = bad_numbers(after)
    if bn:
        raise Failure(f"non-finite numbers at {bn[:3]}")
    if TABLE[op["op"]][1] == "op":
        changed = edits(before, after)
        if r == {'CANCELLED'} and changed:
            raise Failure(f"cancel-changed: {op['op']} returned CANCELLED but changed {changed[:3]}")
        if changed and step_after == step_before:
            raise Failure(f"no-step change: {op['op']} changed the document without an undo step {changed[:3]}")
    return after


def check_undo_chain(depth=6):
    names, at = wm.undo_history()
    k = min(depth, at)
    if k <= 0:
        return
    f = doc.frame_current
    top = snap()
    for _ in range(k):
        ops.ed.undo()
    for _ in range(k):
        ops.ed.redo()
    doc.frame_current = f
    d = edits(top, snap())   # (selection is never an undo step)
    if d:
        raise Failure(f"undo-chain: undo {k} then redo {k} differs: {d[:4]}")


def check_order():
    harness.FOUND.clear()
    order_check("order", frames=(1, 7, 19, 33))
    if harness.FOUND:
        raise Failure("order: " + harness.FOUND[0][1])


def check_roundtrip():
    harness.FOUND.clear()
    roundtrip()
    if harness.FOUND:
        raise Failure("roundtrip: " + harness.FOUND[0][1])


def check_follow(rng):
    """A point weighted 1 to one plain bone (no constraints), in a drawing right inside the rig with only its Armature
    modifier: turning the bone with R moves the point rigidly with it. Undone after."""
    if doc.frame_current == 0 or doc.action:
        return
    found = []
    for rig in rigs():
        for l in rig.children:
            if l.type != "VECTOR" or l.point_count == 0 or l.parent_bone:
                continue
            if [m["type"] for m in l.modifiers] != ["ARMATURE"]:
                continue
            for i, w in enumerate(l.weights):
                if len(w) == 1:
                    (name, value), = w.items()
                    b = bone(rig, name)
                    if value == 1.0 and b and b.get("type") == "REGULAR" and not rig.bone_constraints(name):
                        found.append((rig, l, i, name))
    if not found:
        return
    rig, l, i, name = found[rng.randrange(len(found))]
    L.active = rig
    ops.armature.select_none()
    rig.select_bone(name)
    h0, t0, p0 = bone(rig, name)["head"], bone(rig, name)["tail"], l.shown()[0][i]
    if ops.armature.rotate(angle="37") != {'FINISHED'}:
        return
    h1, t1, p1 = bone(rig, name)["head"], bone(rig, name)["tail"], l.shown()[0][i]
    H0, T0 = complex(*rig.to_document(h0)), complex(*rig.to_document(t0))
    H1, T1 = complex(*rig.to_document(h1)), complex(*rig.to_document(t1))
    ops.ed.undo()
    if abs(T0 - H0) < 1e-9:
        return
    want = H1 + (T1 - H1) / (T0 - H0) * (complex(*p0) - H0)
    if abs(want - complex(*p1)) > 1e-4:
        raise Failure(f"follow: point {i} of {l.name!r} (weight 1 to {name}) after R 37 on frame {doc.frame_current} is "
                      f"{abs(want - complex(*p1)):.5f} off where the bone takes it")


def main():
    load()
    if REPLAY:
        plan = json.load(open(REPLAY, encoding="utf-8"))["ops"]
        rng = None
    else:
        plan, rng = [], random.Random(SEED)
    fail_path = os.path.join(OUT, f"fuzz_fail_{SEED}.json" if not REPLAY else "fuzz_replay_fail.json")
    hang_file = open(os.path.join(OUT, f"fuzz_hang_{SEED if not REPLAY else 'replay'}.txt"), "w")
    checks = random.Random(SEED + 1)
    log = []
    dirty = False   # Python data edits since the last operator pushed a step

    def write_fail(text):
        json.dump({"seed": SEED, "file": harness.FILE, "failure": text, "ops": log}, open(fail_path, "w", encoding="utf-8"),
                  indent=1)

    total = len(plan) if REPLAY else COUNT
    try:
        for n in range(total):
            op = plan[n] if REPLAY else draw(rng)
            log.append(op)
            # Written first, so a hang (the watchdog exits) leaves a log naming it; replaced or removed below.
            write_fail(f"crash-or-hang: {op['op']} crashed or did not finish in {HANG}s")
            faulthandler.dump_traceback_later(HANG, exit=True, file=hang_file)
            before, step_before = snap(), active_step()
            try:
                r = apply(op)
            except Failure:
                raise
            except Exception as e:
                raise Failure(f"exception: {op['op']} raised {type(e).__name__}: {e}\n{traceback.format_exc()}")
            if r != "skip":
                check_op(op, r, before, step_before)
                kind = TABLE[op["op"]][1]
                if kind == "op" and active_step() != step_before:
                    dirty = False
                elif kind == "py":
                    dirty = True
                if not QUIET:
                    print(f"{n:4d} f{doc.frame_current:<3d} {op['op']:16s} -> {r}", flush=True)
            if (n + 1) % EVERY == 0 or n + 1 == total:
                # (and after the last op, so a shrunk replay still reaches the checks)
                write_fail(f"crash-or-hang: the checks after op {n} crashed or did not finish in {HANG}s")
                if not dirty:
                    check_undo_chain()
                check_order()
                check_roundtrip()
                check_follow(checks)
                dirty = False   # reopened: the base is what's there now
            faulthandler.cancel_dump_traceback_later()
            os.remove(fail_path)
        print(f"fuzz seed {SEED}: {len(log)} ops clean", flush=True)
    except Failure as f:
        faulthandler.cancel_dump_traceback_later()
        write_fail(str(f))
        print(f"!!! FUZZ FAIL after {len(log)} ops: {f}\n    log: {fail_path}", flush=True)
        sys.exit(1)
    if not REPLAY:
        json.dump({"seed": SEED, "file": harness.FILE, "ops": log},
                  open(os.path.join(OUT, f"fuzz_{SEED}.json"), "w", encoding="utf-8"), indent=1)


main()
