"""Control points on motion paths (Alt click with the Motion Path tool's Edit, as Toon Boom's): they shape a stretch's
path only, with no key and no frame: the dots go the same share of the way each frame as before. They stay where they
are as the keys move, are picked like path handles (or with keys: Shift click, L), X deletes them, and they are saved. A key added on a path takes the
path's way and speed there, so adding it creases nothing.

Run with:  OpenAnimate --background --python tests/test_motion_path_controls.py
"""

import math
import os
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm


def close(a, b, eps=1e-2):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def pos(layer, f):
    doc.frame_current = f
    return (layer.location_x, layer.location_y)


def key(layer, frame):
    return next(k for k in layer.keyframes("position") if k["frame"] == frame)


def frames(layer):
    return [k["frame"] for k in layer.keyframes("position")]


def shares(points):
    """How far along the path (by length, 0-1) each point is."""
    run = [0.0]
    for a, b in zip(points, points[1:]):
        run.append(run[-1] + math.dist(a, b))
    return [r / run[-1] for r in run]


def mover(keys):
    ops.layer.new()
    lay = layers.active
    lay.add_curve([(0.0, 0.0), (0.1, 0.0)])
    doc.frame_current = 0
    lay.location_x, lay.location_y = -5.0, -5.0
    for other in layers:
        other.selected = other.id == lay.id
    layers.active = lay
    for f, (x, y) in sorted(keys.items()):
        assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), layer=str(lay.id)) == {'FINISHED'}
    return lay


doc.auto_key = False
doc.motion_paths = True
assert ops.wm.tool_motion_path() == {'FINISHED'}

# --- Added on the path: nothing moves, no key, the control point picked alone -----------------------------------------
ball = mover({10: (0.0, 0.0), 30: (4.0, 0.0)})
ball.keyframe_set("position", 30, interp="EASE_IN_OUT")
args = dict(layer=str(ball.id))
before = [pos(ball, f) for f in range(10, 31)]
assert ops.anim.motion_path_select(frame="30", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_add(frame="20", **args) == {'FINISHED'}
assert frames(ball) == [0, 10, 30], ("no key made", frames(ball))
assert len(key(ball, 30)["path_controls"]) == 1, key(ball, 30)
assert [(p["frame"], p["control"]) for p in wm.motion_path_handle_picks()] == [(30, 0)], wm.motion_path_handle_picks()
assert not any(k["selected"] for k in ball.keyframes("position")), "the key let go: picked on its own"
after = [pos(ball, f) for f in range(10, 31)]
assert all(close(a, b) for a, b in zip(before, after)), ("added where the path is: nothing moves", before, after)
assert ops.anim.motion_path_control_add(frame="10", **args) == {'CANCELLED'}, "never on a key"

# --- Dragged: the path bends through it, the keys and the timing stay -------------------------------------------------
assert ops.anim.motion_path_control_move(frame="30", index="0", x="2", y="2", **args) == {'FINISHED'}
bent = [pos(ball, f) for f in range(10, 31)]
assert close(bent[0], before[0]) and close(bent[-1], before[-1]), "the keys stay put"
assert bent[10][1] > 1.5, ("the path goes up through it", bent[10])
assert max(abs(a - b) for a, b in zip(shares(before), shares(bent))) < 0.03, \
    ("each frame the same share of the way: the ease kept", shares(before), shares(bent))
assert bent[1][1] > 0.0 and bent[19][1] > 0.0, "a smooth arc, not a corner"
assert frames(ball) == [0, 10, 30]

# --- It stays where it is as the keys move: the curve's shape changes toward it -------------------------------------
stored = key(ball, 30)["path_controls"][0]
assert close(stored, (2.0, 2.0), 1e-4), ("stored where it is", stored)
assert ops.anim.motion_path_move(frame="30", x="8", y="0", **args) == {'FINISHED'}
assert close(key(ball, 30)["path_controls"][0], stored, 1e-5), "left as it was"
c = next(c for c in wm.motion_path_controls() if c["end"] == 30)
assert close(c["at"], (2.0, 2.0), 1e-3), ("not carried with the key", c["at"])
assert ops.ed.undo() == {'FINISHED'}

# --- A file from before format 6 kept them relative to the keys: read where they were --------------------------------
import json
import zipfile
old_file = os.path.join(tempfile.mkdtemp(), "relative.oanim")
assert ops.wm.save_as_mainfile(filepath=old_file) == {'FINISHED'}
with zipfile.ZipFile(old_file) as z:
    entries = {n: z.read(n) for n in z.namelist()}
stored_doc = json.loads(entries["document.json"])
stored_doc["version"] = 5


def to_relative(node):
    """Every key's control points and handles as format 5 kept them: along and across the line between the keys."""
    if isinstance(node, dict):
        for v in node.values():
            to_relative(v)
    elif isinstance(node, list) and node and all(isinstance(k, dict) and "frame" in k and "value" in k for k in node):
        ks = sorted(node, key=lambda k: k["frame"])
        for i in range(1, len(ks)):
            if "path_controls" not in ks[i]:
                continue
            a, b = ks[i - 1]["value"], ks[i]["value"]
            d = (b[0] - a[0], b[1] - a[1])
            l2 = d[0] * d[0] + d[1] * d[1]
            rel = lambda v: [((v[0] - a[0]) * d[0] + (v[1] - a[1]) * d[1]) / l2,
                             ((v[1] - a[1]) * d[0] - (v[0] - a[0]) * d[1]) / l2]
            ks[i]["path_controls"] = [rel(c) for c in ks[i]["path_controls"]]
    elif isinstance(node, list):
        for v in node:
            to_relative(v)


to_relative(stored_doc)
entries["document.json"] = json.dumps(stored_doc).encode()
with zipfile.ZipFile(old_file, "w") as z:
    for n, data in entries.items():
        z.writestr(n, data)
assert ops.wm.open_mainfile(filepath=old_file) == {'FINISHED'}
ball = next(l for l in layers if l.name == ball.name)
args = dict(layer=str(ball.id))
for l in layers:
    l.selected = l.id == ball.id
layers.active = ball
assert close(key(ball, 30)["path_controls"][0], (2.0, 2.0), 1e-3), ("where it was", key(ball, 30)["path_controls"])

# --- Picked on its own; G moves it ------------------------------------------------------------------------------------
assert ops.anim.motion_path_handle_select(frame="30", control="0", **args) == {'FINISHED'}
assert ops.anim.motion_path_select(frame="30", extend="1", **args) == {'FINISHED'}
assert len(wm.motion_path_handle_picks()) == 1, "a key Shift clicked keeps it (keys and control points go together)"
assert ops.anim.motion_path_select(frame="30", **args) == {'FINISHED'}
assert wm.motion_path_handle_picks() == [], "a key clicked lets go of it"
assert ops.anim.motion_path_handle_select(frame="30", control="0", **args) == {'FINISHED'}
assert not any(k["selected"] for k in ball.keyframes("position"))
y20 = pos(ball, 20)[1]
assert ops.anim.motion_path_keys_transform(kind="G", x="0", y="1") == {'FINISHED'}
assert pos(ball, 20)[1] > y20 + 0.5, (y20, pos(ball, 20))
assert close(pos(ball, 10), before[0]) and close(pos(ball, 30), before[-1]), "G moves the control point alone"
assert ops.ed.undo() == {'FINISHED'}

# --- Saved -----------------------------------------------------------------------------------------------------------
path = os.path.join(tempfile.mkdtemp(), "controls.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
ball = next(l for l in layers if l.name == ball.name)
assert len(key(ball, 30)["path_controls"]) == 1, "saved"
assert close(pos(ball, 20), bent[10]), (pos(ball, 20), bent[10])
assert ops.wm.tool_motion_path() == {'FINISHED'}
args = dict(layer=str(ball.id))

# --- X deletes it: the path as it was --------------------------------------------------------------------------------
assert ops.anim.motion_path_handle_select(frame="30", control="0", **args) == {'FINISHED'}
assert oanim.wm.event_simulate("X", area="Viewport")
assert key(ball, 30)["path_controls"] == [], key(ball, 30)
assert frames(ball) == [0, 10, 30], "no key deleted"
assert all(close(pos(ball, f), b) for f, b in zip(range(10, 31), before)), "back as before"
assert wm.motion_path_handle_picks() == []

# --- A key added on a curved path creases nothing --------------------------------------------------------------------
arc = mover({10: (0.0, 0.0), 20: (2.0, 2.0), 30: (4.0, 0.0)})
curve = [pos(arc, f) for f in range(10, 21)]
assert ops.anim.motion_path_key_add(frame="15", type="KEYFRAME", layer=str(arc.id)) == {'FINISHED'}
k = key(arc, 15)
assert k["path_handles"] == "ALIGNED", k
# Along the path's way there (the curve's own tangent: the chord through the dots either side is near it), in line,
# and the curve split there, every dot staying.
way = (curve[6][0] - curve[4][0], curve[6][1] - curve[4][1])
cross = way[0] * k["path_out"][1] - way[1] * k["path_out"][0]
assert abs(cross) < 2e-2 * math.hypot(*way) * math.hypot(*k["path_out"]) + 1e-6, ("along the path's way", way, k)
crease = k["path_in"][0] * k["path_out"][1] - k["path_in"][1] * k["path_out"][0]
assert abs(crease) < 1e-4 * math.hypot(*k["path_in"]) * math.hypot(*k["path_out"]) + 1e-9, ("in line", k)
assert all(close(pos(arc, f), c, 1e-4) for f, c in zip(range(10, 21), curve)), "the dots stay"


def turn(points, i):
    """How sharply the path turns at point i (degrees between the steps into and out of it)."""
    (ax, ay), (bx, by), (cx, cy) = points[i - 1], points[i], points[i + 1]
    t = math.atan2(cy - by, cx - bx) - math.atan2(by - ay, bx - ax)
    return abs(math.degrees(math.remainder(t, 2 * math.pi)))


now = [pos(arc, f) for f in range(10, 21)]
assert turn(now, 5) < turn(curve, 5) + 2.0, ("no crease at the new key", turn(curve, 5), turn(now, 5))
assert all(close(n, c, 0.08) for n, c in zip(now, curve)), ("the path much as it was", now, curve)

print("Motion path control points: ok")
