"""The Boolean modifier: a layer's shapes and other layers' made one outline (Union, Subtract,
Intersect), as posed; Smoothing rounds only the corners where the outlines meet. The shapes it takes
in from other layers stop drawing there (merged), so nothing is drawn twice.

Run with:  OpenAnimate --background --python tests/test_boolean_modifier.py
"""

import math
import os
import tempfile

import oanim

layers = oanim.data.layers
doc = oanim.data.document
doc.frame_current = 0


def add_square(layer, x0, y0, x1, y1):
    """A square with sharp corners (Peak), as the active layer's."""
    layers.active = layer
    before = layer.point_count
    layer.add_curve([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], closed=True, fill=True)
    layer.select_points(range(layer.point_count), False)
    layer.select_points(range(before, layer.point_count))
    assert oanim.ops.mesh.peak() == {'FINISHED'}
    layer.select_points(range(layer.point_count), False)


head = layers.active
head.name = "head"
add_square(head, -1, -1, 1, 1)
oanim.ops.layer.new()
ear = layers.active
ear.name = "ear"
add_square(ear, 0.5, 0.5, 2, 2)
layers.active = head


def shown(layer):
    """[(area, merged)] of the shapes the layer draws or hands over."""
    return [(s[4], s[3]) for s in layer.evaluated()[2]]


def boolean_index():
    return next(i for i, m in enumerate(head.modifiers) if m["type"] == "BOOLEAN")


# --- Union: one outline round both, the ear drawn once ---------------------------------------------
assert oanim.ops.object.modifier_add(type="BOOLEAN", targets="ear") == {'FINISHED'}
m = head.modifiers[boolean_index()]
assert m["targets"] == ["ear"] and m["operation"] == "UNION" and m["smoothing"] == 0.0, m
(area, merged), = shown(head)
assert abs(area - 6.0) < 1e-3 and not merged, ("4 + 2.25 - the 0.25 overlap", area)
(ear_area, ear_merged), = shown(ear)
assert ear_merged == "ALL" and abs(ear_area - 2.25) < 1e-3, "the ear's shape is drawn by head now, not by itself"

# Smoothing rounds the two corners where the outlines cross, and only those: a 90 degree fillet of
# radius r fills r * r * (1 - pi / 4) each.
oanim.ops.object.modifier_boolean_set(index=str(boolean_index()), smoothing="0.1")
(area, _), = shown(head)
fillet = 0.01 * (1 - math.pi / 4)
assert abs(area - (6.0 + 2 * fillet)) < 1.5e-3, (area, 6.0 + 2 * fillet)
points = head.evaluated()[0]
assert any(abs(x + 1) < 1e-4 and abs(y + 1) < 1e-4 for x, y in points), "the head's own corner stays sharp"
assert any(abs(x - 2) < 1e-4 and abs(y - 2) < 1e-4 for x, y in points), "and the ear's"

# Keyed: Smoothing animates.
oanim.ops.anim.keyframe_insert(layer=str(head.id), paths="modifiers[%d].smoothing" % boolean_index()) \
    if hasattr(oanim.ops.anim, "keyframe_insert") else None

# --- Moved layers merge where they are -------------------------------------------------------------
oanim.ops.object.modifier_boolean_set(index=str(boolean_index()), smoothing="0")
ear.location_x = 0.25
(area, _), = shown(head)
assert abs(area - (4 + 2.25 - 0.25 * 0.5)) < 1e-3, ("the overlap is half as wide now", area)
ear.location_x = 0.0

# --- A hidden layer is out of a Union; then the ear draws itself again --------------------------------
ear.visible = False
(area, _), = shown(head)
assert abs(area - 4.0) < 1e-3, area
ear.visible = True
head.visible = False
assert shown(ear)[0][1] == "", "with head hidden, nothing draws the ear for it"
head.visible = True

# --- Subtract and Intersect: the ear cuts (hidden or not) ----------------------------------------------
oanim.ops.object.modifier_boolean_set(index=str(boolean_index()), operation="SUBTRACT")
(area, _), = shown(head)
assert abs(area - 3.75) < 1e-3, area
ear.visible = False
(area, _), = shown(head)
assert abs(area - 3.75) < 1e-3, "a cutter cuts while hidden, as a mask shape does"
ear.visible = True
assert shown(ear)[0][1] == "ALL", "the cutter isn't drawn"
oanim.ops.object.modifier_boolean_set(index=str(boolean_index()), operation="INTERSECT")
(area, _), = shown(head)
assert abs(area - 0.25) < 1e-3, area
oanim.ops.object.modifier_boolean_set(index=str(boolean_index()), operation="UNION")

# --- Picking shapes: only those merge; the rest draw as they were ----------------------------------------
add_square(ear, 3, 3, 4, 4)
layers.active = head
ear_shapes = [s[0] for s in ear.evaluated()[2]]
first = ear_shapes[0]
assert oanim.ops.object.modifier_boolean_set(index=str(boolean_index()), target="ear", target_shapes=first) == {'FINISHED'}
assert head.modifiers[boolean_index()]["target_shapes"] == {"ear": [first]}
assert [s[1] for s in shown(ear)] == ["ALL", ""], "the other square still draws on the ear layer"
(area, _), = shown(head)
assert abs(area - 6.0) < 1e-3, area

# --- What a merge takes: what its own shape draws. An outline-only layer takes the ear's line, the ear
# keeps drawing its fill; a fill layer with Corners Only just grows the rounded corners. ----------------
outline = None
oanim.ops.layer.new()
outline = layers.active
outline.name = "outline"
add_square(outline, -1, -1, 1, 1)
outline.set_shape_style(0, has_fill=False)
layers.active = outline
assert oanim.ops.object.modifier_add(type="BOOLEAN", targets="ear") == {'FINISHED'}
oanim.ops.object.modifier_boolean_set(index=str(len(outline.modifiers) - 1), target="ear", target_shapes=first)
oanim.ops.object.modifier_boolean_set(index=str(len(outline.modifiers) - 1), smoothing="0.1")
assert shown(ear)[0][1] == "ALL", "head's full merge still takes it all"
layers.active = head
oanim.ops.object.modifier_boolean_set(index=str(boolean_index()), corners_only="1", smoothing="0.1")
assert shown(ear)[0][1] == "LINE", "the outline takes the ear's line; the ear still draws its fill"
(area, _), = shown(outline)
assert abs(area - (6.0 + 2 * fillet)) < 1.5e-3, ("one outline round both, its corners rounded", area)
(area, merged), = shown(head)
assert abs(area - (4.0 + 2 * fillet)) < 1.5e-3, ("the head's square plus the two rounded corners", area)

oanim.ops.object.modifier_boolean_set(index=str(boolean_index()), smoothing="0")
(area, _), = shown(head)
assert abs(area - 4.0) < 1e-3, "no corners to add: the square as drawn"
oanim.ops.object.modifier_boolean_set(index=str(boolean_index()), corners_only="0")
layers.active = outline
assert oanim.ops.layer.delete() == {'FINISHED'}
layers.active = head

# --- Two layers merging each other stop there (no endless loop) ----------------------------------------
layers.active = ear
assert oanim.ops.object.modifier_add(type="BOOLEAN", targets="head") == {'FINISHED'}
assert len(head.evaluated()[2]) >= 1 and len(ear.evaluated()[2]) >= 1
oanim.ops.object.modifier_remove(index=str(len(ear.modifiers) - 1))
layers.active = head

# --- Saved and opened -----------------------------------------------------------------------------------
oanim.ops.object.modifier_boolean_set(index=str(boolean_index()), smoothing="0.05")
path = os.path.join(tempfile.mkdtemp(), "merge.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
head = next(l for l in layers.all() if l.name == "head")
ear = next(l for l in layers.all() if l.name == "ear")
m = head.modifiers[boolean_index()]
assert (m["targets"], m["operation"], abs(m["smoothing"] - 0.05) < 1e-6) == (["ear"], "UNION", True), m
assert m["target_shapes"] == {"ear": [first]}
assert m["name"] == "Boolean"

# A file from when it was called Merge opens with a Boolean (its default name renamed too).
import json, zipfile
old = os.path.join(os.path.dirname(path), "old.oanim")
with zipfile.ZipFile(path) as zin, zipfile.ZipFile(old, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename == "document.json":
            d = json.loads(data)
            renamed = 0

            def walk(o):
                global renamed
                if isinstance(o, dict):
                    if o.get("type") == "BOOLEAN" and o.get("name") == "Boolean":
                        o["type"], o["name"] = "MERGE", "Merge"
                        renamed += 1
                    for v in o.values():
                        walk(v)
                elif isinstance(o, list):
                    for v in o:
                        walk(v)
            walk(d)
            assert renamed == 1, renamed
            data = json.dumps(d).encode()
        zout.writestr(item, data)
assert oanim.ops.wm.open_mainfile(filepath=old, confirmed="1") == {'FINISHED'}
head = next(l for l in layers.all() if l.name == "head")
ear = next(l for l in layers.all() if l.name == "ear")
m = head.modifiers[boolean_index()]
assert (m["type"], m["name"], m["targets"]) == ("BOOLEAN", "Boolean", ["ear"]), m

# --- Apply (Setup only): the outline becomes head's, the ear's merged square leaves it ----------------
layers.active = head
oanim.ops.object.modifier_boolean_set(index=str(boolean_index()), smoothing="0")
doc.frame_current = 5
assert oanim.ops.object.modifier_apply(index=str(boolean_index())) == {'CANCELLED'}
doc.frame_current = 0
assert oanim.ops.object.modifier_apply(index=str(boolean_index())) == {'FINISHED'}
assert not any(mod["type"] == "BOOLEAN" for mod in head.modifiers)
assert [round(a, 3) for a, _ in shown(head)] == [6.0], shown(head)
assert [s[1] for s in shown(ear)] == [""] and abs(shown(ear)[0][0] - 1.0) < 1e-3, "only the other square left"
oanim.ops.ed.undo()
assert any(mod["type"] == "BOOLEAN" for mod in head.modifiers), "undo brings it back"

# --- The eyedropper (pick=, pick_shape=): a clicked shape and its layer go in -------------------------
oanim.ops.layer.new()
cheek = layers.active
cheek.name = "cheek"
add_square(cheek, -1.5, -0.5, -0.8, 0.5)
add_square(cheek, -1.5, -2.0, -0.5, -1.2)
cheek.add_curve([(5.0, 5.0), (6.0, 5.0), (6.0, 6.0)])
names = [s[0] for s in cheek.evaluated()[2]]
layers.active = head
i = str(boolean_index())
assert oanim.ops.object.modifier_boolean_set(index=i, pick="cheek", pick_shape=names[0]) == {'FINISHED'}
m = head.modifiers[boolean_index()]
assert m["targets"] == ["ear", "cheek"] and m["target_shapes"]["cheek"] == [names[0]], m
assert oanim.ops.object.modifier_boolean_set(index=i, pick="cheek", pick_shape=names[1]) == {'FINISHED'}
assert head.modifiers[boolean_index()]["target_shapes"]["cheek"] == [names[0], names[1]], "a second shape joins its picks"
assert oanim.ops.object.modifier_boolean_set(index=i, pick="cheek", pick_shape=names[2]) == {'CANCELLED'},     "an open line can't merge"
assert oanim.ops.object.modifier_boolean_set(index=i, pick="head", pick_shape="S1") == {'CANCELLED'}
# Clicked on a layer taking all its shapes: just the shape clicked, not all of them.
assert oanim.ops.object.modifier_boolean_set(index=i, target="cheek", target_shapes="") == {'FINISHED'}
assert head.modifiers[boolean_index()]["target_shapes"]["cheek"] == []
assert oanim.ops.object.modifier_boolean_set(index=i, pick="cheek", pick_shape=names[1]) == {'FINISHED'}
assert head.modifiers[boolean_index()]["target_shapes"]["cheek"] == [names[1]], "the shape clicked, not all"
# Picked on the ear's row: the cheek takes its place.
assert oanim.ops.object.modifier_boolean_set(index=i, pick="cheek", pick_shape=names[0], replace=str(ear.id)) ==     {'FINISHED'}
assert head.modifiers[boolean_index()]["targets"] == ["cheek"], head.modifiers[boolean_index()]

# --- A shape drawn (or flipped) the other way round joins, rather than cutting the overlap out --------
oanim.ops.layer.new()
left = layers.active
left.name = "left"
add_square(left, 10, 0, 12, 2)
oanim.ops.layer.new()
right = layers.active
right.name = "right"
layers.active = right
right.add_curve([(11, 0), (11, 2), (14, 2), (14, 0)], closed=True, fill=True)  # clockwise
right.select_points(range(right.point_count))
assert oanim.ops.mesh.peak() == {'FINISHED'}
layers.active = left
assert oanim.ops.object.modifier_add(type="BOOLEAN", targets="right") == {'FINISHED'}
assert [round(s[4], 3) for s in left.evaluated()[2]] == [8.0], ("one shape, 4 + 6 - the 2 they share",
                                                                   left.evaluated()[2])

# The merged shape looks as its shape does now: a new colour shows at once, though the outlines
# haven't changed (what was worked out for them is kept).
left.set_shape_style(0, fill=(0.2, 0.4, 0.6, 1.0))
fill = left.evaluated()[2][0][5]
assert all(abs(a - b) < 1e-4 for a, b in zip(fill, (0.2, 0.4, 0.6, 1.0))), fill
left.set_shape_style(0, fill=(0.9, 0.1, 0.1, 1.0))
fill = left.evaluated()[2][0][5]
assert all(abs(a - b) < 1e-4 for a, b in zip(fill, (0.9, 0.1, 0.1, 1.0))), fill
assert [round(s[4], 3) for s in left.evaluated()[2]] == [8.0]

# --- The stack runs top to bottom, the Boolean at its place in it ----------------------------------
oanim.ops.wm.read_homefile(confirmed="1")
layers = oanim.data.layers
own = layers.active
own.add_curve([(0, 0), (2, 0), (2, 2), (0, 2)], closed=True, fill=True)
own.select_points(range(4))
oanim.ops.mesh.peak()
oanim.ops.layer.new()
other = layers.active
other.name = "other"
other.add_curve([(1, 1), (3, 1), (3, 3), (1, 3)], closed=True, fill=True)
other.select_points(range(4))
oanim.ops.mesh.peak()
layers.active = own
assert oanim.ops.object.modifier_add(type="BOOLEAN", targets="other") == {'FINISHED'}
assert oanim.ops.object.modifier_add(type="BEVEL") == {'FINISHED'}
assert [m["type"] for m in own.modifiers] == ["BOOLEAN", "BEVEL"], "new modifiers go at the bottom"
own.modifier_set(1, amount=0.1, limit="NONE", show_viewport=False)
joined = len(own.evaluated()[0])
own.modifier_set(1, show_viewport=True)
# Bevel below: every point of the joined outline is cut in two.
points, _, shapes = own.evaluated()
assert len(points) == 2 * joined, (len(points), joined)
below = shapes[0][4]
# Bevel above: only the layer's own square is cut, then joined: a different outline.
assert oanim.ops.object.modifier_move_to(index="1", to="0") == {'FINISHED'}
points, _, shapes = own.evaluated()
assert len(points) != 2 * joined and abs(shapes[0][4] - below) > 1e-3, (len(points), shapes[0][4], below)

print("boolean_modifier: ok")
