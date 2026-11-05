"""Curve handles: every handle is free (stored, moved only by hand); Align keeps a point's two
handles in line. New points get handles once, smooth or straight; Peak and Smooth set them again.
Saving, older files, animation and mirror editing.

Run with:  OpenAnimate --background --python tests/test_handles.py
"""

import json
import math
import pathlib
import tempfile
import zipfile

import oanim


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1])


def length(v):
    return math.hypot(v[0], v[1])


def collinear_opposite(a, b):
    """True when vectors a and b point exactly away from each other."""
    cross = a[0] * b[1] - a[1] * b[0]
    dot = a[0] * b[0] + a[1] * b[1]
    return abs(cross) < 1e-4 and dot < 0


doc = oanim.data.document
layer = oanim.data.layers.active
pts = [(0.0, 0.0), (1.0, 0.5), (2.0, 0.0)]
curve = layer.add_curve(pts)
oanim.ops.mesh.select_none()  # an undo step: the new points get their handles here

# --- New points: smooth, aligned handles that then stay put --------------------------------
kind, h_in, h_out = layer.curve_handles(curve)[1]
assert kind == "ALIGNED", "a smooth new point's handles start in line"
assert collinear_opposite(sub(h_in, pts[1]), sub(h_out, pts[1]))
layer.set_point(2, (2.0, 1.0))
oanim.ops.mesh.select_none()
_, after_in, after_out = layer.curve_handles(curve)[1]
assert close(after_in, h_in) and close(after_out, h_out), "moving a neighbour doesn't touch free handles"
layer.set_point(2, (2.0, 0.0))

# --- Align and Unalign ---------------------------------------------------------------------------
oanim.ops.mesh.select_none()
assert oanim.ops.mesh.handles_unalign() == {'CANCELLED'}, "nothing selected"
layer.select_points([1])
assert oanim.ops.mesh.handles_unalign() == {'FINISHED'}
assert layer.curve_handles(curve)[1][0] == "FREE"
layer.move_handle(curve, 1, "IN", (0.6, 0.9))
layer.move_handle(curve, 1, "OUT", (1.5, 0.9))
_, free_in, free_out = layer.curve_handles(curve)[1]
assert close(free_in, (0.6, 0.9)) and close(free_out, (1.5, 0.9)), "free handles move on their own"

# With one handle picked, that handle turns into line; the other stays put.
for side, other in (("OUT", free_in), ("IN", free_out)):
    layer.move_handle(curve, 1, "IN", free_in)
    layer.move_handle(curve, 1, "OUT", free_out)
    oanim.ops.mesh.select_none()
    layer.select_handle(curve, 1, side)
    assert oanim.ops.mesh.handles_align() == {'FINISHED'}
    kind, a_in, a_out = layer.curve_handles(curve)[1]
    assert kind == "ALIGNED"
    assert close(a_in if side == "OUT" else a_out, other), (side, "the other handle stays put")
    assert collinear_opposite(sub(a_in, pts[1]), sub(a_out, pts[1])), side
    oanim.ops.mesh.select_none()
    layer.select_points([1])
    oanim.ops.mesh.handles_unalign()

# The point picked: both handles turn the same angle, onto the line halfway between, each keeping its length.
oanim.ops.mesh.select_none()
layer.select_points([1])
layer.move_handle(curve, 1, "IN", free_in)
layer.move_handle(curve, 1, "OUT", free_out)
d_in, d_out = sub(free_in, pts[1]), sub(free_out, pts[1])
assert oanim.ops.mesh.handles_align() == {'FINISHED'}
kind, h_in, h_out = layer.curve_handles(curve)[1]
assert kind == "ALIGNED" and collinear_opposite(sub(h_in, pts[1]), sub(h_out, pts[1]))
n_in, n_out = sub(h_in, pts[1]), sub(h_out, pts[1])
assert abs(length(n_in) - length(d_in)) < 1e-4 and abs(length(n_out) - length(d_out)) < 1e-4, "lengths kept"


def turn(a, b):
    """The angle from vector a to b, -pi..pi."""
    return math.atan2(a[0] * b[1] - a[1] * b[0], a[0] * b[0] + a[1] * b[1])


t_in, t_out = turn(d_in, n_in), turn(d_out, n_out)
assert abs(t_in) > 1e-3 and abs(t_in + t_out) < 1e-4, ("both turned as far, toward each other", t_in, t_out)

# Folded back on each other (both pointing the same way): along the way from the point before to the one after.
oanim.ops.mesh.handles_unalign()
layer.move_handle(curve, 1, "IN", (pts[1][0], pts[1][1] + 0.4))
layer.move_handle(curve, 1, "OUT", (pts[1][0], pts[1][1] + 0.3))
assert oanim.ops.mesh.handles_align() == {'FINISHED'}
_, f_in, f_out = layer.curve_handles(curve)[1]
run = sub(pts[2], pts[0])
along = (run[0] / length(run), run[1] / length(run))
o = sub(f_out, pts[1])
assert abs(turn(along, o)) < 1e-4 and abs(length(o) - 0.3) < 1e-4, ("toward the point after", o, along)
oanim.ops.mesh.handles_unalign()
layer.move_handle(curve, 1, "IN", free_in)
layer.move_handle(curve, 1, "OUT", free_out)
assert oanim.ops.mesh.handles_align() == {'FINISHED'}

# Aligned: moving one handle turns the other, keeping its length.
assert oanim.ops.mesh.handles_align() == {'FINISHED'}
_, old_in, _ = layer.curve_handles(curve)[1]
layer.move_handle(curve, 1, "OUT", (1.5, 1.0))
kind, n_in, n_out = layer.curve_handles(curve)[1]
assert kind == "ALIGNED" and close(n_out, (1.5, 1.0))
assert collinear_opposite(sub(n_in, pts[1]), sub(n_out, pts[1]))
assert abs(length(sub(n_in, pts[1])) - length(sub(old_in, pts[1]))) < 1e-4


# --- Peak and Smooth -----------------------------------------------------------------------------
oanim.ops.mesh.select_none()
layer.select_points([1])
assert oanim.ops.mesh.peak() == {'FINISHED'}
kind, p_in, p_out = layer.curve_handles(curve)[1]
assert kind == "FREE"
assert close(p_in, (1.0 - 1 / 3, 0.5 - 0.5 / 3)) and close(p_out, (1.0 + 1 / 3, 0.5 - 0.5 / 3)), \
    "Peak points the handles a third of the way to the neighbours"
assert oanim.ops.mesh.smooth() == {'FINISHED'}
kind, s_in, s_out = layer.curve_handles(curve)[1]
assert kind == "ALIGNED" and collinear_opposite(sub(s_in, pts[1]), sub(s_out, pts[1]))

# --- Box corners: straight sides -----------------------------------------------------------------
assert layer.add_primitive("BOX", [(3.0, 0.0), (4.0, 1.0)])
oanim.ops.mesh.select_none()
box_curve = layer.curve_count - 1
assert [h[0] for h in layer.curve_handles(box_curve)] == ["FREE"] * 4
outline, closed = layer.shape_outline(layer.shape_count - 1, 8)
assert closed
for x, y in outline:
    assert min(abs(x - 3.0), abs(x - 4.0), abs(y - 0.0), abs(y - 1.0)) < 1e-4, f"box sides stay straight ({x}, {y})"

# --- Saved with the document ---------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_handles_")) / "handles.oanim")
saved = layer.curve_handles(curve)
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = oanim.data.layers.active
loaded = layer.curve_handles(curve)
assert [h[0] for h in loaded] == [h[0] for h in saved]
for a, b in zip(loaded, saved):
    assert close(a[1], b[1]) and close(a[2], b[2]), (a, b)

# --- Older files: Automatic and Vector handles are worked out once --------------------------------
with zipfile.ZipFile(path) as z:
    data = json.loads(z.read("document.json"))
for curve_json in data["document"]["layers"][0]["drawing"]["curves"]:
    for i, cp in enumerate(curve_json["points"]):
        for name in ("handle_in", "handle_out", "aligned"):
            cp.pop(name, None)
        cp["handle_type"] = "VECTOR" if len(curve_json["points"]) == 4 else "AUTO"
old_path = str(pathlib.Path(path).with_name("old.oanim"))
with zipfile.ZipFile(old_path, "w") as z:
    z.writestr("document.json", json.dumps(data))
assert oanim.ops.wm.open_mainfile(filepath=old_path, confirmed="1") == {'FINISHED'}
layer = oanim.data.layers.active
kind, o_in, o_out = layer.curve_handles(curve)[1]
assert kind == "ALIGNED" and collinear_opposite(sub(o_in, pts[1]), sub(o_out, pts[1])), "Automatic -> smooth, aligned"
outline, _ = layer.shape_outline(layer.shape_count - 1, 8)
assert all(min(abs(x - 3.0), abs(x - 4.0), abs(y), abs(y - 1.0)) < 1e-4 for x, y in outline), "Vector -> straight"

# --- Animated: handles first moved on a later frame keep their setup shape on frame 0 ------------
doc = oanim.data.document
doc.frame_current = 0
oanim.ops.layer.new()
anim = oanim.data.layers.active
anim.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True, fill=True)
oanim.ops.mesh.select_none()
setup_outline, _ = anim.shape_outline(0, 16)
doc.frame_current = 20
anim.set_point(1, (1.3, -0.2))
anim.move_handle(0, 1, "OUT", (1.6, 0.3))
doc.frame_current = 21


def same_outline(a, b):
    return all(abs(p[0] - q[0]) < 1e-4 and abs(p[1] - q[1]) < 1e-4 for p, q in zip(a, b))


doc.frame_current = 0
assert same_outline(anim.shape_outline(0, 16)[0], setup_outline), "frame 0 keeps its shape"

# Files saved before the fix (zero-length handles keyed on frame 0) are repaired on opening.
path2 = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_handles_")) / "stale.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path2) == {'FINISHED'}
with zipfile.ZipFile(path2) as z:
    data = json.loads(z.read("document.json"))
for layer_json in data["document"]["layers"]:
    for curve_json in (layer_json.get("drawing") or {}).get("curves", []):
        for cp in curve_json["points"]:
            for name in ("handle_in_keys", "handle_out_keys"):
                for key in cp.get(name, []):
                    if key["frame"] == 0:
                        key["value"] = [0.0, 0.0]
with zipfile.ZipFile(path2, "w") as z:
    z.writestr("document.json", json.dumps(data))
assert oanim.ops.wm.open_mainfile(filepath=path2, confirmed="1") == {'FINISHED'}
oanim.data.document.frame_current = 0
anim = next(l for l in oanim.data.layers if l.name == anim.name)
assert same_outline(anim.shape_outline(0, 16)[0], setup_outline), "repaired on opening"

# --- Mirror editing: Align on one side aligns the mirror image too -----------------------------
oanim.data.document.frame_current = 0
oanim.ops.layer.new()
sym = oanim.data.layers.active
sym.add_curve([(-1.0, 0.0), (-0.5, 0.5), (0.0, 0.6), (0.5, 0.5), (1.0, 0.0)])
oanim.ops.mesh.select_none()
sym.select_points([1])
oanim.ops.mesh.peak()  # both sides free (Peak mirrors too once mirror editing is on, below)
oanim.wm.set_mirror_editing(x=True)
sym.move_handle(0, 1, "OUT", (-0.3, 0.9))
assert oanim.ops.mesh.handles_align() == {'FINISHED'}
_, l_in, l_out = sym.curve_handles(0)[1]
kind, r_in, r_out = sym.curve_handles(0)[3]
assert kind == "ALIGNED"
assert close(r_out, (-l_in[0], l_in[1])) and close(r_in, (-l_out[0], l_out[1])), (l_in, l_out, r_in, r_out)
oanim.wm.set_mirror_editing()
oanim.ops.mesh.handles_unalign()
assert sym.curve_handles(0)[3][0] == "ALIGNED", "with mirror editing off, only the selection changes"

print("Handle tests passed")
