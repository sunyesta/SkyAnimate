"""A small G move (or Sculpt Grab) of a point bent by a mesh squashed nearly flat (folded back over itself, or flattened along a slant)
lands under the mouse, or as near as the deform lets it: never farther off than where it was, never flung away. The
plain skin undone goes far for little where the deform squashes, and from there the point can show anywhere.

Run with:  OpenAnimate --background --python tests/test_deform_settle_squashed.py
"""

import math

import oanim

ops = oanim.ops
layers = oanim.data.layers
doc = oanim.data.document
doc.auto_key = True
anchor = layers.active


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


# Name: (grid columns and rows, the squash, whether G itself must land nearer than where the point was).
SQUASHES = {
    # The right column folded back over the middle one.
    "folded": (2, lambda x, y: (-0.5 if x > 0.5 else x, y), True),
    # Everything flattened onto the diagonal, a hair apart across it.
    "flattened": (2, lambda x, y: ((x + y) * 0.5 + 0.002 * (x - y), (x + y) * 0.5 - 0.002 * (x - y)), True),
    # A finer grid flattened onto a curve: a Sculpt Grab dab's skin, fixed at the stroke's start, sends the stored point
    # where settling can't find its way back from, unless it also tries where the point was stored before.
    "curved": (4, lambda x, y: ((x + y) * 0.5 + 0.0014 * (x - y), ((x + y) * 0.5) ** 2 * 0.8 - 0.0014 * (x - y)),
               False),
}

MOVE = 0.006
for method in ("CELLS", "ENVELOPE"):
    for name, (cells, squash, strict) in SQUASHES.items():
        doc.frame_current = 0
        layers.active = anchor
        ops.layer.new()
        grid = layers.active
        assert grid.add_primitive("GRID", [(-1, -1), (1, 1)], columns=cells, rows=cells)
        layers.active = anchor
        ops.layer.new()
        art = layers.active
        art.add_curve([(0.05, 0.02), (0.5, 0.3), (0.85, -0.4), (0.3, -0.7)])
        layers.active = art
        assert ops.object.modifier_add(type="MESH_DEFORM") == {'FINISHED'}
        art.modifier_set(0, layer=grid.name, shape=grid.shapes[0][0], mesh_method=method)
        doc.frame_current = 10
        for i, p in enumerate(grid.points):
            grid.set_point(i, squash(p[0], p[1]))
        at(10)
        worst = flung = grab_worst = 0.0
        for p in range(art.point_count):
            for k in range(8):
                d = (MOVE * math.cos(k * math.pi / 4), MOVE * math.sin(k * math.pi / 4))
                before = art.shown()[0][p]
                layers.active = art
                ops.mesh.select_none()
                art.select_points([p])
                assert ops.mesh.translate(move_x=str(d[0]), move_y=str(d[1])) == {'FINISHED'}
                at(10)
                now = art.shown()[0][p]
                worst = max(worst, math.dist(now, (before[0] + d[0], before[1] + d[1])))
                flung = max(flung, math.dist(now, before))
                assert ops.ed.undo() == {'FINISHED'}
                at(10)
                # Sculpt Grab settles each dab's points the same way, trying where they were stored too.
                path = "%r,%r;%r,%r" % (before[0], before[1], before[0] + d[0], before[1] + d[1])
                assert ops.sculpt.stroke(brush="GRAB", path=path, radius="0.002", strength="1", falloff="CONSTANT",
                                         mirror="0") == {'FINISHED'}
                at(10)
                now = art.shown()[0][p]
                grab_worst = max(grab_worst, math.dist(now, (before[0] + d[0], before[1] + d[1])))
                assert ops.ed.undo() == {'FINISHED'}
                at(10)
        print(method, name, "worst off %.4f (Sculpt Grab %.4f), farthest %.4f" % (worst, grab_worst, flung), flush=True)
        assert not strict or worst < MOVE, (method, name, "a point landed farther off than where it was", worst)
        assert flung < 2.0 * MOVE, (method, name, "a point was flung", flung)
        assert grab_worst < max(worst, MOVE) + 1e-4, (method, name, "Sculpt Grab left a point farther off than G",
                                                      grab_worst, worst)
        grid.visible = False
        art.visible = False

print("settling on squashed deforms: all good")
