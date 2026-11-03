"""Mesh Deform's Points method: every line of the mesh is a bone. What lies near a line keeps its place by it, turned as
the line turns there, and each place is shared by the lines near it, the nearest leading (Blend) and the others' say
fading out by Falloff past the nearest one's distance, measured through the mesh. The weights are set at rest: pulling
one side of the mesh a long way leaves everything further than Falloff from it exactly where it was.

Run with:  OpenAnimate --background --python tests/test_mesh_points.py
"""

import math
import os
import shutil
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True
anchor = layers.active


def at(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def new_layer(name):
    layers.active = anchor
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


def deform(layer, mesh, **settings):
    layers.active = layer
    assert ops.object.modifier_add(type="MESH_DEFORM") == {'FINISHED'}
    layer.modifier_set(0, layer=mesh.name, shape=mesh.shapes[0][0], precision=0.0, mesh_method="POINTS", **settings)
    at(0)
    layer.evaluated()


def shown(layer):
    return [tuple(p[:2]) for p in layer.evaluated()[0]]


def reshape(mesh, change, frame=10):
    doc.frame_current = frame
    for i, p in enumerate(mesh.points):
        q = change(p[0], p[1])
        if q and math.dist(q, p[:2]) > 1e-9:
            mesh.set_point(i, q)
    at(frame)


def new_grid(name):
    grid = new_layer(name)
    assert grid.add_primitive("GRID", [(-1, -1), (1, 1)], columns=4, rows=4)
    return grid


def moved(rest, now):
    return [math.dist(a, b) for a, b in zip(rest, now)]


# --- Pulling one side a long way ------------------------------------------------------------------------------------
grid = new_grid("Grid")
art = new_layer("Art")
ys = [-0.9, -0.7, -0.4, -0.2, 0.2, 0.4, 0.7, 0.9]
xs = [-0.9, -0.6, -0.3, 0.1, 0.4, 0.8]
for y in ys:
    art.add_curve([(x, y) for x in xs])
deform(art, grid)
m = art.modifiers[0]
assert m["mesh_method"] == "POINTS" and m["kind"] == "Grid" and abs(m["falloff"] - 0.25) < 1e-6, m
rest = shown(art)
assert max(moved(rest, [p[:2] for p in art.points])) < 1e-6, "on the setup frame nothing bends"


def rows_moved():
    by_row = {}
    for a, d in zip(rest, moved(rest, shown(art))):
        by_row[a[1]] = max(by_row.get(a[1], 0.0), d)
    return {round(y, 2): d for y, d in by_row.items()}


# The bottom row goes down 0.6 (the bottom cells three times as tall). What lies by it goes with it; what lies past the
# next line up stays exactly where it was: no line's weight changes as the mesh moves.
reshape(grid, lambda x, y: (x, y - 0.6) if y < -0.99 else None)
art.modifier_set(0, falloff=0.1)
at(10)
low = rows_moved()
assert low[-0.9] > 0.4 and low[-0.7] > 0.1, ("the bottom row's art goes with it", low)
assert all(low[y] < 1e-5 for y in ys if y > -0.5), ("what lies past the next line stays", low)


def row_heights(y):
    return [b[1] for a, b in zip(rest, shown(art)) if abs(a[1] - y) < 1e-6]


# The bottom cells, three times as tall now, take what is in them along as their sides now run: each row stays a
# straight row, where Cells puts it. Each line beside a row would otherwise hold it as far off as at rest (waves: the
# side lines holding it high, the bottom one low), or as far off as its neighbours have stretched end to end (the
# moved points' handles leave those running unevenly: smaller waves).
art.modifier_set(0, mesh_method="CELLS")
at(10)
by_cells = {y: row_heights(y) for y in (-0.9, -0.7)}
art.modifier_set(0, mesh_method="POINTS")
for falloff in (0.1, 0.25):
    art.modifier_set(0, falloff=falloff)
    at(10)
    for y, cells in by_cells.items():
        heights = row_heights(y)
        assert max(abs(h - c) for h, c in zip(heights, cells)) < 1e-3, ("the row stays even", falloff, y, heights, cells)
# However far Falloff reaches, a stretch stays in the cells that stretched: the bottom line carries what lies past them
# as far off them as it was.
art.modifier_set(0, falloff=1.0)
at(10)
wide = rows_moved()
assert all(wide[y] < 1e-4 for y in ys if y > -0.5), ("a stretch reaches out of its cells", wide)
art.modifier_set(0, falloff=0.25)

# Falloff's view: where lines share what is in the mesh; more of it shared the wider Falloff is.
view = art.deform_falloff_view(0)
assert view is not None and view[1] > 100, view[1:]


def shared(falloff):
    art.modifier_set(0, falloff=falloff)
    places = art.deform_falloff_view(0)[0]
    return sum(s for x, y, s in places) / len(places)


assert shared(0.05) + 0.05 < shared(0.25) < shared(0.8), (shared(0.05), shared(0.25), shared(0.8))
art.modifier_set(0, falloff=0.25, mesh_method="CELLS")
assert art.deform_falloff_view(0) is None, "only Points has lines to share"
art.modifier_set(0, mesh_method="POINTS")
doc.frame_current = 0

# --- A cell sheared ---------------------------------------------------------------------------------------------------
# The bottom row slides across: what is in the bottom cells slides with their sides, as far as it was up them, its
# height kept (Cells as the reference; the slanted sides, S-shaped by their points' handles, turn what lies by them a
# little, so the heights only nearly). Each line beside it would otherwise hold it where it was.
sheared = new_grid("Sheared")
slid = new_layer("Slid")
for y in (-0.9, -0.7):
    slid.add_curve([(x, y) for x in xs])
deform(slid, sheared, falloff=0.1)
reshape(sheared, lambda x, y: (x + 0.4, y) if y < -0.99 else None)
slid.modifier_set(0, mesh_method="CELLS")
at(10)
by_cells = shown(slid)
slid.modifier_set(0, mesh_method="POINTS")
at(10)
for p, c in zip(shown(slid), by_cells):
    assert abs(p[0] - c[0]) < 0.005 and abs(p[1] - c[1]) < 0.025, ("it slides with the cell", p, c)
doc.frame_current = 0

# --- Art on a line follows it exactly -------------------------------------------------------------------------------
lined = new_grid("Lined")
on_line = new_layer("OnLine")
on_line.add_curve([(x, 0.0) for x in (-0.8, -0.65, -0.3, 0.2, 0.35, 0.7, 0.9)])
deform(on_line, lined)
reshape(lined, lambda x, y: (x, 0.2 + 0.1 * x) if abs(y) < 0.01 else None)
row = lined.curve_handles(2)
nodes = [p[:2] for p in lined.points if abs(p[1] - 0.2 - 0.1 * p[0]) < 1e-6]
nodes.sort()


def on_row(x):
    """Where the middle row now runs, at the place along it that `x` was at rest (its handles a third along each way)."""
    k = min(int((x + 1.0) / 0.5), 3)
    t = (x + 1.0) / 0.5 - k
    b = (nodes[k], row[k][2], row[k + 1][1], nodes[k + 1])
    return tuple(sum(c * q[i] for c, q in zip(((1 - t) ** 3, 3 * t * (1 - t) ** 2, 3 * t * t * (1 - t), t ** 3), b))
                 for i in range(2))


for (x, y), q in zip([p[:2] for p in on_line.points], shown(on_line)):
    assert math.dist(q, on_row(x)) < 1e-4, ("it keeps to its place along its line", (x, y), q, on_row(x))
doc.frame_current = 0

# --- The whole mesh turned and moved: everything in it goes along as one, its handles turned too ---------------------
turned = new_grid("Turned")
rigid = new_layer("Rigid")
rigid.add_curve([(-0.7, -0.6), (-0.2, 0.3), (0.45, 0.1), (0.8, 0.75)])
rigid.add_curve([(-0.3, -0.8), (0.6, -0.4)])
deform(rigid, turned)
rigid_rest = shown(rigid)
art_handles = rigid.curve_handles(0)
angle, shift = math.radians(30.0), (0.3, -0.2)


def turn(x, y):
    c, s = math.cos(angle), math.sin(angle)
    return (c * x - s * y + shift[0], s * x + c * y + shift[1])


doc.frame_current = 10
handles = [turned.curve_handles(c) for c in range(10)]
reshape(turned, turn)
for c, points in enumerate(handles):
    for i, (_, h_in, h_out) in enumerate(points):
        turned.place_handle(c, i, "IN", turn(*h_in), keep_other=True)
        turned.place_handle(c, i, "OUT", turn(*h_out), keep_other=True)
at(10)
for a, b in zip(rigid_rest, shown(rigid)):
    assert math.dist(turn(*a), b) < 1e-4, ("it goes along as one", a, b, turn(*a))
for (_, i0, o0), (_, i1, o1) in zip(art_handles, rigid.evaluated_handles(0)):
    assert math.dist(turn(*i0), i1) < 1e-4 and math.dist(turn(*o0), o1) < 1e-4, ("its handles turn", i0, i1)
doc.frame_current = 0

# --- A point's handle turns what lies by it -------------------------------------------------------------------------
# The middle row's handle leaving the middle node turns up a third: what lies just along from the node turns up with it,
# about the node; what lies by the other lines out of the node hardly moves.
handled = new_grid("Handled")
near = new_layer("Near")
near.add_curve([(0.1, 0.02), (0.2, -0.02)])
near.add_curve([(-0.15, 0.3), (-0.3, 0.15)])
deform(near, handled, falloff=0.1)
near_rest = shown(near)
doc.frame_current = 10
handled.place_handle(2, 2, "OUT", (0.1667 * math.cos(math.radians(30)), 0.1667 * math.sin(math.radians(30))),
                     keep_other=True)
at(10)
near_now = shown(near)
for a, b in zip(near_rest[:2], near_now[:2]):
    assert b[1] - a[1] > 0.01, ("what lies along the handle turns up with it", a, b)
    assert abs(math.hypot(*b) - math.hypot(*a)) < 0.4 * math.dist(a, b), ("it turns about the node", a, b)
assert max(moved(near_rest[2:], near_now[2:])) < 0.25 * min(moved(near_rest[:2], near_now[:2])), (near_rest, near_now)
# A wider Falloff lets the turned line reach further: across the next lines, less the further.
near.modifier_set(0, falloff=0.6)
at(10)
wide_now = shown(near)
assert min(moved(near_rest[2:], wide_now[2:])) > 2 * max(moved(near_rest[2:], near_now[2:])) + 0.002, (near_now, wide_now)
doc.frame_current = 0

# --- Never across a gap ---------------------------------------------------------------------------------------------
# A U drawn by hand: two arms up from a bottom cell, a narrow slot between them. The right arm's inner side swings out;
# what lies in the left arm by the slot (as near the right arm's side as Falloff reaches, straight across) stays, as the
# right arm's side is a long way round through the mesh. What lies as near it in the right arm goes along.
u = new_layer("U")
u.add_curve([(-1.0, -0.5), (-0.1, -0.5), (0.1, -0.5), (1.0, -0.5)])


def pick(*points):
    ops.mesh.select_none()
    u.select_points(list(points))


def grow(point, pos):
    pick(point)
    [added] = u.extrude_selected()
    u.set_point(added, pos)
    return added


def join(a, b):
    pick(a, b)
    assert ops.mesh.make_segment() == {'FINISHED'}


inner_left, inner_right = grow(1, (-0.1, 1.0)), grow(2, (0.1, 1.0))
join(grow(0, (-1.0, 1.0)), inner_left)
join(grow(3, (1.0, 1.0)), inner_right)
join(grow(0, (-1.0, -1.0)), grow(3, (1.0, -1.0)))
pick(*range(len(u.points)))
assert ops.mesh.peak() == {'FINISHED'}
ops.mesh.select_none()
assert u.shape_count == 1
slot = new_layer("Slot")
slot.add_curve([(-0.15, 0.9), (-0.15, 0.6)])
slot.add_curve([(0.15, 0.9), (0.15, 0.6)])
deform(slot, u)
assert slot.modifiers[0]["kind"] == "Mesh", slot.modifiers[0]
# A step along the right arm's art's first handle: where the deform takes that is the way the handle should turn.
[(_, _, handle)] = slot.curve_handles(1)[:1]
way = (handle[0] - 0.15, handle[1] - 0.9)
step = new_layer("Step")
step.add_curve([(0.15, 0.9), (0.15 + way[0] * 0.01, 0.9 + way[1] * 0.01)])
deform(step, u)
slot_rest = shown(slot)
reshape(u, lambda x, y: (0.4, 1.0) if abs(x - 0.1) < 1e-6 and y > 0.99 else None)
slot_now = shown(slot)
assert max(moved(slot_rest[:2], slot_now[:2])) < 1e-5, ("reached across the slot", slot_rest[:2], slot_now[:2])
assert min(moved(slot_rest[2:], slot_now[2:])) > 0.02, ("the right arm's art goes along", slot_rest[2:], slot_now[2:])
# The art's own handles bend as the deform does at their point (not as the cell's patch would).
a, b = shown(step)
[(_, _, bent)] = slot.evaluated_handles(1)[:1]
along, bent_way = math.atan2(b[1] - a[1], b[0] - a[0]), math.atan2(bent[1] - a[1], bent[0] - a[0])
assert abs(along - bent_way) < math.radians(0.2), ("the handle turns as the deform does", math.degrees(along),
                                                    math.degrees(bent_way))
doc.frame_current = 0

# --- One closed shape of any sides ----------------------------------------------------------------------------------
# Mesh Deform takes a lone closed shape as one cell, whatever its sides (as Cage Deform takes it, its own way): its sides
# are the bones. Pulling the bottom of a five-sided cage down leaves what lies by its top sides where it was.
pentagon = new_layer("Pentagon")
corners = [(math.sin(math.radians(a)), math.cos(math.radians(a))) for a in range(180, 540, 72)]
pentagon.add_curve(corners, closed=True)
lid = new_layer("Lid")
lid.add_curve([(-0.4, 0.75), (0.0, 0.85), (0.4, 0.75)])
lid.add_curve([(-0.3, -0.7), (0.3, -0.7)])
deform(lid, pentagon, falloff=0.1)
assert lid.modifiers[0]["kind"] == "Mesh" and lid.modifiers[0]["fits"] == "MESH_DEFORM", lid.modifiers[0]
lid_rest = shown(lid)
reshape(pentagon, lambda x, y: (x, y - 0.5) if y < -0.5 else None)
lid_now = shown(lid)
assert max(moved(lid_rest[:3], lid_now[:3])) < 1e-5, ("what lies by the top stays", lid_rest[:3], lid_now[:3])
assert min(moved(lid_rest[3:], lid_now[3:])) > 0.2, ("what lies by the bottom goes with it", lid_now[3:])
lid.modifier_set(0, mesh_method="CELLS")
at(10)
assert min(moved(lid_rest[3:], shown(lid)[3:])) > 0.2, "Cells bends it too"
lid.modifier_set(0, mesh_method="POINTS")
doc.frame_current = 0
# A loop of three points (one curve, three sides), and Cage Deform still takes a closed shape as before.
loop = new_layer("Loop")
loop.add_curve([(-1.0, -0.8), (1.0, -0.8), (0.0, 1.0)], closed=True)
deform(lid, loop)
assert lid.modifiers[0]["kind"] == "Mesh" and lid.modifiers[0]["fits"] == "MESH_DEFORM", lid.modifiers[0]
lid.modifier_set(0, type="CAGE_DEFORM")
assert lid.modifiers[0]["kind"] == "Cage" and lid.modifiers[0]["fits"] == "CAGE_DEFORM", lid.modifiers[0]

# --- Falloff is saved -----------------------------------------------------------------------------------------------
folder = tempfile.mkdtemp()
try:
    path = os.path.join(folder, "points.oanim")
    art.modifier_set(0, falloff=0.37)
    assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
    kept = next(l for l in oanim.data.layers.all() if l.name == art.name)
    assert kept.modifiers[0]["mesh_method"] == "POINTS" and abs(kept.modifiers[0]["falloff"] - 0.37) < 1e-6, \
        kept.modifiers[0]
finally:
    shutil.rmtree(folder)

print("mesh points: ok")
