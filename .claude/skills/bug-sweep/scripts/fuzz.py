"""Seeded random operations on a file's bones and points, with invariants checked as it goes. Runs inside the app:

    set OA_SWEEP_FILE=<private copy.oanim>  OA_FUZZ_SEED=7  OA_FUZZ_OPS=300  OA_FUZZ_OUT=<folder>
    OpenAnimate.exe --background --python fuzz.py

Every op is drawn with concrete arguments first, then applied, and logged to <OUT>/fuzz_<seed>.json, so a failure can
be replayed exactly (OA_FUZZ_REPLAY=<that json>) and shrunk (shrink.py). Exits 1 on the first failure, writing the ops
up to it and what failed.

Checks: after every op, no exception, finite numbers, an operator's change comes with an undo step, CANCELLED changes
nothing; every OA_FUZZ_EVERY ops (default 25), undoing the last steps and redoing them gives the same document, frames
reached in different orders show the same pose, and save/open changes nothing. Python data edits (bone_set,
set_point, frame changes) are not undo steps by design, so the undo checks wait until an operator has pushed after them.
"""
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


class Failure(Exception):
    pass


def rigs():
    return [l for l in L.all() if l.type == "BONE"]


def editable(layer):
    """A reference's points are its original's, locked unless its Edit Points is on."""
    try:
        ref = layer.reference
    except Exception:
        return True
    return not ref or bool(ref.get("edit_points", True))


def drawings():
    return [l for l in L.all() if l.type == "VECTOR" and l.point_count > 0 and editable(l)]


def find_layer(lid):
    return next((l for l in L.all() if l.id == lid), None)


def bone_names(rig):
    return [b["name"] for b in rig.bones]


# --- Drawing ops: concrete arguments from the document as it is ------------------------------------------------------
def draw(rng):
    r = rng.random()
    rs, ds = rigs(), drawings()
    f = rng.choice([0, 0, 0] + list(range(1, FRAMES + 1)))
    if r < 0.12 or not (rs or ds):
        return {"op": "frame", "f": f}
    if r < 0.15:
        return {"op": "auto_key", "on": rng.random() < 0.7}
    if r < 0.22:
        return {"op": rng.choice(["undo", "redo"]), "n": rng.randint(1, 4)}
    if ds and r < 0.42:
        d = rng.choice(ds)
        i = rng.randrange(d.point_count)
        kind = rng.random()
        if kind < 0.6:
            return {"op": "point_move", "layer": d.id, "i": i, "d": (rng.gauss(0, 0.05), rng.gauss(0, 0.05))}
        if kind < 0.75:
            return {"op": "point_delete", "layer": d.id, "i": i}
        if kind < 0.9 and rs:
            rig = rng.choice(rs)
            names = bone_names(rig)
            if names:
                return {"op": "weight", "layer": d.id, "i": i, "rig": rig.id, "bone": rng.choice(names),
                        "w": rng.choice(["1", "0.5", "0"])}
        return {"op": "key_insert", "layer": d.id}
    if not rs:
        return {"op": "frame", "f": f}
    rig = rng.choice(rs)
    names = bone_names(rig)
    if not names:
        return {"op": "bone_add", "rig": rig.id, "head": (0.0, 0.0), "tail": (0.0, 0.5), "parent": ""}
    name = rng.choice(names)
    kind = rng.random()
    b = next(x for x in rig.bones if x["name"] == name)
    if kind < 0.3:
        return {"op": "bone_turn", "rig": rig.id, "bone": name, "a": rng.gauss(0, 0.4)}
    if kind < 0.5:
        return {"op": "bone_move", "rig": rig.id, "bone": name, "d": (rng.gauss(0, 0.1), rng.gauss(0, 0.1))}
    if kind < 0.58:
        return {"op": "bone_scale", "rig": rig.id, "bone": name, "s": rng.choice([0.5, 0.8, 1.25, 2.0])}
    if kind < 0.66 and b.get("type") == "CONTROLLER_2D":
        return {"op": "knob", "rig": rig.id, "bone": name, "k": (rng.uniform(-80, 80), rng.uniform(-80, 80))}
    if kind < 0.7:
        h = (rng.uniform(-1, 1), rng.uniform(-1, 1))
        return {"op": "bone_add", "rig": rig.id, "head": h, "tail": (h[0] + rng.uniform(-0.4, 0.4), h[1] + 0.3),
                "parent": rng.choice(names + [""])}
    if kind < 0.74:
        return {"op": "bone_delete", "rig": rig.id, "bone": name}
    if kind < 0.78:
        return {"op": "bone_duplicate", "rig": rig.id, "bone": name}
    if kind < 0.83 and len(names) > 1:
        return {"op": "parent_set", "rig": rig.id, "bone": name, "to": rng.choice([n for n in names if n != name]),
                "type": rng.choice(["OFFSET", "CONNECTED"])}
    if kind < 0.86:
        return {"op": "parent_clear", "rig": rig.id, "bone": name}
    if kind < 0.9:
        return {"op": "convert", "rig": rig.id, "bone": name, "type": rng.choice(["REGULAR", "PIN", "CONTROLLER_2D"])}
    if kind < 0.95:
        return {"op": "key_insert", "layer": rig.id}
    return {"op": "key_delete", "layer": rig.id}


# --- Applying them: "skip" where the document no longer has what the op names (shrunk replays) -------------------------
OPERATORS = {"bone_add", "bone_delete", "bone_duplicate", "parent_set", "parent_clear", "convert", "key_insert",
             "key_delete", "point_delete", "weight"}


def pick_bone(rig, name):
    L.active = rig
    ops.armature.select_none()
    rig.select_bone(name)


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
    layer = find_layer(op.get("layer", op.get("rig")))
    if layer is None:
        return "skip"
    if "bone" in op and op["bone"] not in bone_names(layer if "rig" in op else find_layer(op["rig"])):
        return "skip"
    if o in ("point_move", "point_delete", "weight") and op["i"] >= layer.point_count:
        return "skip"
    if o == "point_move":
        x, y = layer.points[op["i"]][:2]
        layer.set_point(op["i"], (x + op["d"][0], y + op["d"][1]))
        return None
    if o == "point_delete":
        L.active = layer
        layer.select_points([op["i"]])
        return ops.mesh.delete()
    if o == "weight":
        rig = find_layer(op["rig"])
        pick_bone(rig, op["bone"])
        L.active = layer
        ops.mesh.select_none()
        layer.select_points([op["i"]])
        return ops.weight.assign(weight=op["w"])
    if o == "key_insert":
        L.active = layer
        return ops.anim.keyframe_insert()
    if o == "key_delete":
        L.active = layer
        return ops.anim.keyframe_delete()
    b = next(x for x in layer.bones if x["name"] == op["bone"]) if "bone" in op else None
    if o == "bone_turn":
        layer.bone_set(op["bone"], angle=b["angle"] + op["a"])
        return None
    if o == "bone_move":
        layer.bone_set(op["bone"], position=(b["position"][0] + op["d"][0], b["position"][1] + op["d"][1]))
        return None
    if o == "bone_scale":
        s = b.get("scale", (1.0, 1.0))
        layer.bone_set(op["bone"], scale=(s[0] * op["s"], s[1] * op["s"]))
        return None
    if o == "knob":
        layer.bone_set(op["bone"], knob=tuple(op["k"]))
        return None
    if o == "bone_add":
        L.active = layer
        ops.armature.select_none()
        if op["parent"] and op["parent"] not in bone_names(layer):
            return "skip"
        return ops.armature.bone_add(head_x=str(op["head"][0]), head_y=str(op["head"][1]), tail_x=str(op["tail"][0]),
                                     tail_y=str(op["tail"][1]), parent=op["parent"])
    if o == "bone_delete":
        pick_bone(layer, op["bone"])
        return ops.armature.bone_delete()
    if o == "bone_duplicate":
        pick_bone(layer, op["bone"])
        return ops.armature.duplicate_move()
    if o == "parent_set":
        if op["to"] not in bone_names(layer):
            return "skip"
        pick_bone(layer, op["bone"])
        layer.select_bone(op["to"], extend=True)
        return ops.armature.parent_set(type=op["type"])
    if o == "parent_clear":
        pick_bone(layer, op["bone"])
        return ops.armature.parent_clear(type="CLEAR")
    if o == "convert":
        pick_bone(layer, op["bone"])
        return ops.armature.bone_convert(type=op["type"])
    raise ValueError(f"unknown op {o}")


# --- The checks -------------------------------------------------------------------------------------------------------
def check_op(op, before, step_before):
    after, step_after = snap(), active_step()
    bn = bad_numbers(after)
    if bn:
        raise Failure(f"non-finite numbers at {bn[:3]}")
    if op["op"] in OPERATORS:
        changed = edits(before, after)
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
    d = diff(top, snap())
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


def main():
    load()
    if REPLAY:
        plan = json.load(open(REPLAY, encoding="utf-8"))["ops"]
        rng = None
    else:
        plan, rng = [], random.Random(SEED)
    log = []
    dirty = False   # Python data edits since the last operator pushed a step
    total = len(plan) if REPLAY else COUNT
    try:
        for n in range(total):
            op = plan[n] if REPLAY else draw(rng)
            log.append(op)
            before, step_before = snap(), active_step()
            try:
                r = apply(op)
            except Failure:
                raise
            except Exception as e:
                raise Failure(f"exception: {op['op']} raised {type(e).__name__}: {e}\n{traceback.format_exc()}")
            if r == "skip":
                continue
            check_op(op, before, step_before)
            if op["op"] in OPERATORS and active_step() != step_before:
                dirty = False
            elif op["op"] not in OPERATORS and op["op"] not in ("undo", "redo"):
                dirty = True
            if (n + 1) % EVERY == 0 or n + 1 == total:
                # (and after the last op, so a shrunk replay still reaches the checks)
                if not dirty:
                    check_undo_chain()
                check_order()
                check_roundtrip()
                dirty = False   # reopened: the base is what's there now
        print(f"fuzz seed {SEED}: {len(log)} ops clean", flush=True)
    except Failure as f:
        path = os.path.join(OUT, f"fuzz_fail_{SEED}.json" if not REPLAY else "fuzz_replay_fail.json")
        json.dump({"seed": SEED, "file": harness.FILE, "failure": str(f), "ops": log}, open(path, "w", encoding="utf-8"),
                  indent=1)
        print(f"!!! FUZZ FAIL after {len(log)} ops: {f}\n    log: {path}", flush=True)
        sys.exit(1)
    if not REPLAY:
        json.dump({"seed": SEED, "file": harness.FILE, "ops": log},
                  open(os.path.join(OUT, f"fuzz_{SEED}.json"), "w", encoding="utf-8"), indent=1)


main()
