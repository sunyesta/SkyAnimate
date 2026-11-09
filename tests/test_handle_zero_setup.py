"""An aligned point whose handles have no length in the setup pose: pulled out on a later frame, the two handles stay in
line on every frame between (the in handle turns from pointing back, the out handle from pointing on). A file saved
before format 8 opens with each key showing the handle it did.

Run with:  OpenAnimate --background --python tests/test_handle_zero_setup.py
"""

import json
import math
import os
import shutil
import tempfile
import zipfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers


def wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def handles(layer, f):
    doc.frame_current = f
    kind, i, o = layer.curve_handles(0)[1]
    q = layer.points[1][:2]
    return kind, (i[0] - q[0], i[1] - q[1]), (o[0] - q[0], o[1] - q[1])


def bend(layer, f):
    """How far out of line the point's handles are on `f` (degrees), and the shorter one's length."""
    kind, i, o = handles(layer, f)
    return abs(math.degrees(wrap(math.atan2(i[1], i[0]) - math.atan2(-o[1], -o[0])))), min(math.hypot(*i),
                                                                                          math.hypot(*o))


def zero_handled():
    ops.wm.read_homefile(confirmed="1")
    doc.auto_key = True
    doc.frame_current = 0
    layer = layers.active
    layer.add_curve([(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)])
    ops.mesh.select_none()
    q = layer.points[1][:2]
    layer.move_handle(0, 1, "IN", q)
    layer.move_handle(0, 1, "OUT", q)
    ops.mesh.select_none()
    assert layer.curve_handles(0)[1][0] == "ALIGNED"
    doc.frame_current = 20
    layer.move_handle(0, 1, "IN", (q[0] - 0.3, q[1] + 0.2))
    layer.move_handle(0, 1, "OUT", (q[0] + 0.3, q[1] - 0.25))
    ops.mesh.select_none()
    doc.frame_current = 30
    layer.move_handle(0, 1, "OUT", (q[0] - 0.1, q[1] - 0.35))
    ops.mesh.select_none()
    return layer


# --- Pulled out of nothing, an aligned pair stays in line between the keys -------------------------------------------
layer = zero_handled()
for f in (5, 10, 15, 20, 25, 30):
    b, shortest = bend(layer, f)
    assert shortest > 0.01, (f, shortest)
    assert b < 0.01, "aligned handles %.2f degrees out of line on frame %d" % (b, f)

# --- A file from before format 8 opens as it was at its keys -----------------------------------------------------------
layer = zero_handled()
keyed = {f: handles(layer, f) for f in (0, 20, 30)}
folder = tempfile.mkdtemp()
path = os.path.join(folder, "new.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
with zipfile.ZipFile(path) as z:
    data = json.loads(z.read("document.json"))
assert data["version"] == 8, data["version"]
data["version"] = 7
shifted = 0


def old_turns(node):
    """The in handles' keys as format 7 measured them: their turns from angle 0, not from pointing back."""
    global shifted
    if isinstance(node, dict):
        for k, v in node.items():
            if k == "handle_in_keys" and isinstance(v, list):
                for key in v:
                    if key["value"][1] > 0.0:
                        key["value"][0] = wrap(key["value"][0] + math.pi)
                        shifted += 1
            else:
                old_turns(v)
    elif isinstance(node, list):
        for v in node:
            old_turns(v)


old_turns(data)
assert shifted == 2, shifted
old = os.path.join(folder, "old.oanim")
with zipfile.ZipFile(old, "w") as z:
    z.writestr("document.json", json.dumps(data))
ops.wm.open_mainfile(filepath=old, confirmed="1")
layer = next(l for l in layers.all() if l.points)
for f, was in keyed.items():
    now = handles(layer, f)
    assert all(abs(a - b) < 1e-5 for a, b in zip(now[1] + now[2], was[1] + was[2])), (f, now, was)
for f in (10, 25):
    b, _ = bend(layer, f)
    assert b < 0.01, "after opening, %.2f degrees out of line on frame %d" % (b, f)
shutil.rmtree(folder, ignore_errors=True)

print("Handle zero setup tests passed")
