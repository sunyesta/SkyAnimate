"""What bending by a Mesh Deform costs, counted (oanim.wm.deform_counts): the work that made the eye rig slow, without
changing what any drawing shows.

- A point is placed in each four-sided cell once, however many steps ask (on a side? outside? which cell?).
- A cell the point is well outside of is looked in only when no cell holds it: in a mesh of two eyes every point of
  one eye was placed in the other eye's cell too, which takes all of Newton's steps and finds nothing.
- A point well clear of a cell's side isn't searched for on it.
- A mesh of one shape that isn't four-sided is its own cell: its points aren't measured against its outline.
- A group's deform is built once for all the drawings in it, not once each.
- A render's drawings are bent side by side on the CPU's cores (wm.evaluated_together), each as it is on its own.

Run with:  OpenAnimate --background --python tests/test_deform_speed.py
"""

import math
import os

import oanim

doc, layers, ops, wm = oanim.data.document, oanim.data.layers, oanim.ops, oanim.wm
doc.frame_current = 0
doc.auto_key = True
anchor = layers.active


def new_layer(name, points=None, closed=False):
    layers.active = anchor
    ops.layer.new()
    layer = layers.active
    layer.name = name
    if points:
        layer.add_curve(points, closed=closed)
    return layer


def pick(layer, *points):
    ops.mesh.select_none()
    layer.select_points(list(points))


def two_cells():
    """Two four-sided cells side by side, sharing the side at x = 0."""
    h = new_layer("Mesh")
    h.add_curve([(-1.0, 0.8), (0.0, 0.8), (1.0, 0.8)])
    pick(h, 1)
    h.extrude_selected()
    h.set_point(3, (0.0, -0.8))
    pick(h, 0)
    h.extrude_selected()
    h.set_point(4, (-1.0, -0.8))
    pick(h, 4, 3)
    ops.mesh.make_segment()
    pick(h, 2)
    h.extrude_selected()
    h.set_point(5, (1.0, -0.8))
    pick(h, 5, 3)
    ops.mesh.make_segment()
    ops.mesh.select_none()
    return h


def into_group(name, *children):
    layers.active = anchor
    assert ops.layer.new_group() == {'FINISHED'}
    group = layers.active
    group.name = name
    for child in children:
        layers.active = child
        assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
    return group


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def counted(drawings):
    """Each drawing's points as shown, and what showing them all cost."""
    wm.deform_counts_clear()
    shown = [layer.evaluated()[0] for layer in drawings]
    return shown, wm.deform_counts()


def line(x0, x1, y, n):
    return [(x0 + (x1 - x0) * k / (n - 1), y + 0.05 * math.sin(k)) for k in range(n)]


# --- Two four-sided cells, as a group's deform, three drawings in the group -------------------------------------------
mesh = two_cells()
# Each in one cell (as each eye's drawings are in its own): the other cell is first for one or the other.
deep = new_layer("Deep", line(-0.8, -0.5, 0.1, 12))
right = new_layer("Right", line(0.7, 0.3, -0.2, 12))
wide = new_layer("Wide", line(-0.9, -0.2, 0.4, 12))
group = into_group("Group", deep, right, wide)
layers.active = group
assert ops.object.modifier_add(type="MESH_DEFORM") == {'FINISHED'}
group.modifier_set(0, layer=mesh.name, shape=mesh.shapes[0][0], mesh_method="ENVELOPE", precision=0.0)
assert group.modifiers[0]["kind"] == "Grid", group.modifiers[0]
drawings = [next(l for l in layers.all() if l.name == n) for n in ("Deep", "Right", "Wide")]

doc.frame_current = 10
mesh.set_point(0, (-1.2, 0.9))   # the left cell's corner out and up, keyed
mesh.set_point(4, (-1.1, -0.7))
mesh.set_point(5, (1.2, -0.95))  # and the right one's
at(10)
shown, c = counted(drawings)
print("two cells:", c)
for before, now in zip((d.points for d in drawings), shown):
    assert len(now) == len(before) and any(math.dist(p[:2], q) > 1e-3 for p, q in zip(before, now)), "they bend"

# Every point well inside its cell: placed in it once (and again for its handles), never in the other.
assert c["points"] >= 36, c
assert c["cell_solves"] <= 1.5 * c["points"], ("a point is placed in its cell once", c)
# Well inside a cell, no side is searched.
assert c["side_tests"] < c["points"] // 4, ("only points near a side look for the nearest place on it", c)

# One build for the group, shared by the drawings.
assert c["builds"] == 1 and c["builds_shared"] == 2, ("the group's deform built once", c)
shown_again, c = counted(drawings)
assert shown_again == shown and c["builds"] == 0 and c["builds_shared"] == 0, ("kept: nothing built again", c)

# --- A mesh of one shape that isn't four-sided: its own cell ---------------------------------------------------------
doc.frame_current = 0
blob = new_layer("Blob", [(math.cos(2 * math.pi * k / 7), 0.8 * math.sin(2 * math.pi * k / 7)) for k in range(7)],
                 closed=True)
art = new_layer("Art", line(-0.5, 0.5, 0.0, 16))
layers.active = art
assert ops.object.modifier_add(type="MESH_DEFORM") == {'FINISHED'}
art.modifier_set(0, layer=blob.name, shape=blob.shapes[0][0], mesh_method="CELLS", precision=0.0)
doc.frame_current = 10
blob.set_point(0, (1.3, 0.1))
at(10)
shown, c = counted([art])
print("one shape:", c)
assert any(math.dist(p[:2], q) > 1e-3 for p, q in zip(art.points, shown[0])), "it bends"
assert c["points"] >= 16 and c["cell_measures"] == 0, ("one cell: nothing to pick between", c)

# --- Side by side: many drawings in the group, bent on several threads at once ----------------------------------------
doc.frame_current = 0
many = [new_layer("Many%d" % k, line(-0.9 + 0.1 * (k % 8), 0.8 - 0.05 * (k % 8), 0.6 - 0.07 * k, 40)) for k in range(16)]
# The same drawings again, bent one at a time (their own bends: none kept from the others').
twins = [new_layer("Twin%d" % k, [p[:2] for p in l.points]) for k, l in enumerate(many)]
for child in many + twins:
    layers.active = child
    assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
many = [next(l for l in layers.all() if l.name == "Many%d" % k) for k in range(16)]
twins = [next(l for l in layers.all() if l.name == "Twin%d" % k) for k in range(16)]
at(10)
together, threads = wm.evaluated_together(many)
alone = [wm.evaluated_together([t])[0][0] for t in twins]
for k, (a, b) in enumerate(zip(together, alone)):
    assert a == b, ("bent side by side, the same as alone", k)
    assert any(math.dist(p[:2], q) > 1e-3 for p, q in zip(many[k].points, a[0])), "they bend"
if (os.cpu_count() or 1) > 1:
    assert threads > 1, ("bent on several threads", threads)
print("side by side on", threads, "threads")

print("deform speed: fine")
