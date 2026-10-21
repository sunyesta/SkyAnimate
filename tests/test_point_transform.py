"""Point and handle keys hold the change from the setup pose (frame 0): a point's Transform and a
handle's Angle and Length. Reshaping frame 0 keeps every pose's motion; going through frames adds no
keys; a point added to an animated curve takes its neighbours' motion; saving keeps it; files from
before (format 2, the points' own values in their keys, as the Moho converter writes) open
converted.

Run with:  SkyAnimate --background --python tests/test_point_transform.py
"""

import json
import math
import pathlib
import tempfile
import zipfile

import sky

doc = sky.data.document
layers = sky.data.layers
doc.frame_current = 0
doc.auto_key = True


def rotate(v, a):
    c, s = math.cos(a), math.sin(a)
    return (v[0] * c - v[1] * s, v[0] * s + v[1] * c)


def close(a, b, eps=1e-4):
    if isinstance(a, (tuple, list)):
        return all(abs(x - y) < eps for x, y in zip(a, b))
    return abs(a - b) < eps


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1])


layer = layers.active
layer.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
sky.ops.mesh.select_none()


def keys(path):
    return [(k["frame"], tuple(k["value"])) for k in layer.keyframes(path)]


def where(i):
    return tuple(layer.points[i][:2])


def out_handle(i):
    """Point i's out handle, as an offset from the point."""
    return sub(layer.curve_handles(0)[i][2], where(i))


# --- A point's keys hold its Transform ------------------------------------------------------------
doc.frame_current = 10
layer.set_point(1, (1.0, 0.5))
doc.frame_current = 11
k = keys("points[1].pos")
assert [f for f, v in k] == [0, 10] and close(k[0][1], (0, 0)) and close(k[1][1], (0, 0.5)), k

# --- Reshaping frame 0 keeps the motion ---------------------------------------------------------------
doc.frame_current = 0
layer.set_point(1, (1.0, -1.0))
doc.frame_current = 10
assert close(where(1), (1.0, -0.5)), ("moved up from its new setup", where(1))
assert close(keys("points[1].pos")[1][1], (0, 0.5)), "the key stayed the change"

# --- A handle's keys hold its Angle and Length -------------------------------------------------------
doc.frame_current = 0
setup_out = out_handle(1)
assert math.hypot(*setup_out) > 1e-3, setup_out
doc.frame_current = 10
here = where(1)
turned = rotate(setup_out, math.pi / 2)
layer.move_handle(0, 1, "OUT", (here[0] + turned[0], here[1] + turned[1]))
doc.frame_current = 11
k = keys("curves[0].points[1].handle_out")
assert [f for f, v in k] == [0, 10], k
assert close(k[0][1], (0, 0)) and close(k[1][1], (math.pi / 2, 0)), ("turned a quarter, same length", k)
# Halfway it swings along an arc, keeping its length.
doc.frame_current = 5
assert close(math.hypot(*out_handle(1)), math.hypot(*setup_out), 1e-3), "an arc, not a straight cut"

# Lengthening it on frame 0 lengthens every pose's handle; the turn stays.
doc.frame_current = 0
here = where(1)
layer.move_handle(0, 1, "OUT", (here[0] + 2 * setup_out[0], here[1] + 2 * setup_out[1]))
doc.frame_current = 10
assert close(out_handle(1), rotate((2 * setup_out[0], 2 * setup_out[1]), math.pi / 2), 1e-3), out_handle(1)

# --- Going through frames adds no keys ---------------------------------------------------------------
for f in (3, 7, 10, 0, 13, 5, 0, 10):
    doc.frame_current = f
assert [f for f, v in keys("points[1].pos")] == [0, 10]
assert [f for f, v in keys("curves[0].points[1].handle_out")] == [0, 10]
assert not keys("points[0].pos") and not keys("points[2].pos"), "still points have no keys"

# --- A point added to an animated curve takes its neighbours' motion ----------------------------------
doc.frame_current = 0
new = layer.split_segment(0, 0, 0.5)
doc.frame_current = 1
doc.frame_current = 0
rest = where(new)
doc.frame_current = 10
moved = sub(where(new), rest)
assert 0.15 < moved[1] < 0.35 and abs(moved[0]) < 0.05, ("between a still point and one moving 0.5", moved)
assert [f for f, v in keys("points[%d].pos" % new)] == [0, 10]

# --- Saved and opened ------------------------------------------------------------------------------------
doc.frame_current = 0
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_point_transform_")) / "points.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = next(l for l in layers.all() if l.point_count > 0)
p1 = 1 if new != 1 else 2
doc.frame_current = 10
shown_10 = where(p1)
assert close(keys("points[%d].pos" % p1)[1][1], (0, 0.5))

# --- Format 2 files (the points' own values in their keys) open converted -----------------------------------
with zipfile.ZipFile(path) as z:
    entries = {n: z.read(n) for n in z.namelist()}
root = json.loads(entries["document.json"])
root["version"] = 2


def drawings(nodes):
    for n in nodes:
        if "drawing" in n:
            yield n["drawing"]
        yield from drawings(n.get("children", []))


for d in drawings(root["document"]["layers"]):
    for p in d["points"]:
        setup = p.pop("setup_pos", p["pos"])
        for k in p.get("pos_keys", []):
            k["value"] = [setup[0] + k["value"][0], setup[1] + k["value"][1]]
    for c in d["curves"]:
        for cp in c["points"]:
            for side in ("in", "out"):
                setup = cp.pop("setup_" + side, cp.get("handle_" + side, [0.0, 0.0]))
                for k in cp.get("handle_%s_keys" % side, []):
                    turn, grow = k["value"]
                    length = math.hypot(*setup)
                    angle = math.atan2(setup[1], setup[0]) + turn
                    k["value"] = [math.cos(angle) * (length + grow), math.sin(angle) * (length + grow)]
entries["document.json"] = json.dumps(root).encode("utf-8")
with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
    for n, data in entries.items():
        z.writestr(n, data)
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = next(l for l in layers.all() if l.point_count > 0)
assert close(keys("points[%d].pos" % p1)[0][1], (0, 0)) and close(keys("points[%d].pos" % p1)[1][1], (0, 0.5))
# The split put the new point before it on the curve: point 1 is the curve's third point now.
k = keys("curves[0].points[2].handle_out")
assert close(k[0][1], (0, 0)) and close(k[1][1], (math.pi / 2, 0)), k
doc.frame_current = 10
assert close(where(p1), shown_10), "it plays as it did"

# --- Alt G and Alt R away from frame 0: back to the setup pose ------------------------------------------
layers.active = layer
doc.frame_current = 0
rest_point, rest_handle = where(p1), sub(layer.curve_handles(0)[2][2], where(p1))
sky.ops.mesh.select_none()
layer.select_points([p1])
assert not sky.wm.event_simulate("G", alt=True, area="Viewport"), "frame 0 is the setup already"
doc.frame_current = 10
assert not close(where(p1), rest_point) and not close(sub(layer.curve_handles(0)[2][2], where(p1)), rest_handle)
turned_handle = sub(layer.curve_handles(0)[2][2], where(p1))
# Alt G: the point goes back; its handles keep their turn.
assert sky.wm.event_simulate("G", alt=True, area="Viewport")
assert close(where(p1), rest_point), (where(p1), rest_point)
assert close(sub(layer.curve_handles(0)[2][2], where(p1)), turned_handle), "Alt G leaves the handles"
# Alt R: the handles go back to their setup shape.
assert sky.wm.event_simulate("R", alt=True, area="Viewport")
assert close(sub(layer.curve_handles(0)[2][2], where(p1)), rest_handle)
doc.frame_current = 11
assert close(keys("points[%d].pos" % p1)[1][1], (0, 0)), "auto-key keyed it back"
assert close(keys("curves[0].points[2].handle_out")[1][1], (0, 0))

print("Point transform tests passed")
