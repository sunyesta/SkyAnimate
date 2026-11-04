"""Show Mesh (2026-11-03): Curve, Cage and Mesh Deform draw an even grid over what they reach, bent as they bend that
space now.

- Off by default, on the deforms that bend only (not Follow Deform, not Mirror), saved with the file.
- The grid covers the shape and the drawing at rest, with a margin; square cells, 16 across the longer side.
- On the setup frame it is the even grid; on a bent frame each place goes where the drawing's own point there goes, and
  Strength scales the move.

Run with:  OpenAnimate --background --python tests/test_deform_show_mesh.py
"""
import json
import math
import os
import tempfile
import zipfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


ops.wm.read_homefile(confirmed="1")
doc.frame_current = 0
line = new_layer("Line")
line.add_curve([(-0.35, 0.0), (0.0, 0.06), (0.35, 0.0)])
art = new_layer("Art")
ART = [(-0.25, 0.1), (-0.1, 0.12), (0.05, -0.08), (0.2, 0.05), (0.1, 0.3), (-0.15, -0.3)]
art.add_curve(ART)
assert ops.object.modifier_add(type="CURVE_DEFORM") == {'FINISHED'}
art.modifier_set(0, layer="Line", shape=line.shapes[0][0], precision=0.0)
assert art.modifiers[0]["curve_method"] == "ENVELOPE", art.modifiers[0]
assert art.modifiers[0]["show_mesh"] is False, art.modifiers[0]
art.modifier_set(0, show_mesh=True)
assert art.modifiers[0]["show_mesh"] is True

# Only the deforms that bend have it.
assert ops.object.modifier_add(type="MIRROR") == {'FINISHED'}
assert "show_mesh" not in art.modifiers[1], art.modifiers[1]
try:
    art.deform_grid_view(1)
    raise AssertionError("a Mirror gave a grid")
except IndexError:
    pass
art.modifier_set(1, show_viewport=False)

# --- Setup frame: the even grid over the shape and the drawing --------------------------------------------------------
lines, columns, rows, start, cell, bent = art.deform_grid_view(0)
assert not bent
assert max(columns, rows) == 16 and len(lines) == columns + rows + 2, (columns, rows, len(lines))
xs = [p[0] for p in ART] + [-0.35, 0.35]
ys = [p[1] for p in ART] + [0.0, 0.06]
assert start[0] < min(xs) and start[1] < min(ys), (start, min(xs), min(ys))
assert start[0] + columns * cell > max(xs) and start[1] + rows * cell > max(ys)
for i in range(columns + 1):
    assert len(lines[i]) == rows * 6 + 1
    for k, p in enumerate(lines[i]):
        assert math.dist(p, (start[0] + i * cell, start[1] + k * cell / 6)) < 1e-5, (i, k, p)
for j in range(rows + 1):
    row = lines[columns + 1 + j]
    assert len(row) == columns * 6 + 1
    for k, p in enumerate(row):
        assert math.dist(p, (start[0] + k * cell / 6, start[1] + j * cell)) < 1e-5, (j, k, p)
REST = lines

# --- A bent frame: each place goes where a drawing point there goes ---------------------------------------------------
doc.frame_current = 8
line.set_point(1, (0.05, 0.25))
line.set_point(2, (0.3, -0.15))
doc.frame_current = 9
doc.frame_current = 8
lines, columns, rows, start, cell, bent = art.deform_grid_view(0)
assert bent
moved = max(math.dist(p, q) for a, b in zip(lines, REST) for p, q in zip(a, b))
assert moved > 0.05, moved
# A probe drawing with a point on every 3rd place of the middle column and row, bent by the same deform alone.
probe_places = [(0, i) for i in range(0, len(REST[columns // 2]), 3)] + \
               [(1, i) for i in range(0, len(REST[columns + 1 + rows // 2]), 3)]
doc.frame_current = 0
probe = new_layer("Probe")
at = [REST[columns // 2][i] if which == 0 else REST[columns + 1 + rows // 2][i] for which, i in probe_places]
probe.add_curve(at)
assert ops.object.modifier_add(type="CURVE_DEFORM") == {'FINISHED'}
probe.modifier_set(0, layer="Line", shape=line.shapes[0][0], precision=0.0)
doc.frame_current = 8
got = probe.evaluated()[0]
grid = [lines[columns // 2][i] if which == 0 else lines[columns + 1 + rows // 2][i] for which, i in probe_places]
worst = max(math.dist(p, q) for p, q in zip(got, grid))
assert worst < 1e-4, ("grid and drawing disagree", worst)

# Strength scales the move.
art.modifier_set(0, strength=0.5)
half = art.deform_grid_view(0)[0]
for a, b, c in zip(half, lines, REST):
    for p, q, r in zip(a, b, c):
        assert math.dist(p, ((q[0] + r[0]) / 2, (q[1] + r[1]) / 2)) < 1e-5, (p, q, r)
art.modifier_set(0, strength=1.0)

# Cage and Mesh Deform have it too; Follow Deform doesn't.
loop = new_layer("Loop")
loop.add_curve([(-0.3, -0.3), (0.3, -0.3), (0.3, 0.3), (-0.3, 0.3)], closed=True)
for kind in ("CAGE_DEFORM", "MESH_DEFORM"):
    layer = new_layer(kind)
    layer.add_curve(ART)
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    layer.modifier_set(0, layer="Loop", shape=loop.shapes[0][0], show_mesh=True)
    assert layer.modifiers[0]["show_mesh"] is True and layer.deform_grid_view(0) is not None, kind
follow = new_layer("Follow")
follow.add_curve(ART)
assert ops.object.modifier_add(type="FOLLOW_DEFORM") == {'FINISHED'}
assert "show_mesh" not in follow.modifiers[0], follow.modifiers[0]

# --- Saved with the file -----------------------------------------------------------------------------------------------
path = os.path.join(tempfile.mkdtemp(), "mesh.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
with zipfile.ZipFile(path) as z:
    text = z.read("document.json").decode()
assert '"show_mesh": true' in text.replace('":true', '": true'), "show_mesh not saved"
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
by_name = {l.name: l for l in layers.all()}
assert by_name["Art"].modifiers[0]["show_mesh"] is True
assert by_name["Probe"].modifiers[0]["show_mesh"] is False
assert "show_mesh" not in by_name["Art"].modifiers[1]

print("Deform Show Mesh tests passed")
