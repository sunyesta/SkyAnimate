"""The timeline's rows follow the Layers editor: Summary first (every key below it), then Pinned, then each
shown layer in the Layers editor's order and nesting, folding to its own channels, its Drawing or Points,
its Bones (a folder per bone) and a Media layer's clips; the camera and Simulation last. Folds are kept by
the project; the picked layers start open.

Run with:  OpenAnimate --background --python tests/test_timeline_tree.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True


def rows(as_shown=False):
    return wm.timeline_row_details(as_shown)


def names(as_shown=False):
    return [(r["depth"], r["name"]) for r in rows(as_shown)]


def row(name, as_shown=False):
    return next(r for r in rows(as_shown) if r["name"] == name)


# A group holding two drawings, and a drawing above it on its own.
first = layers.active
first.name = "first"
ops.layer.new()
top = layers.active
top.name = "top"
layers.active = first
first.add_curve([(0.0, 0.0), (1.0, 0.0)])
ops.layer.group()
group = layers.active
group.name = "group"
ops.layer.new()
second = layers.active
second.name = "second"
second.add_curve([(0.0, 1.0), (1.0, 1.0)])
assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
assert top.parent is None and second.parent == group

# Keys: the drawings' positions on 10, the second's points on 20, the top's opacity on 30.
doc.frame_current = 10
for l in (first, second):
    l.location_x = 0.5
doc.frame_current = 20
layers.active = second
ops.mesh.select_all()
assert ops.mesh.translate(move_x="0.25", move_y="0") == {'FINISHED'}
doc.frame_current = 30
top.opacity = 0.5
doc.frame_current = 0

# The group picked (it edits what's inside it): its layers nested under it, in the Layers editor's order.
layers.active = group
tree = names()
assert tree[0] == (0, "Summary"), tree
order = [l.name for l in group.children]
at = tree.index((0, "group"))
inside = [name for depth, name in tree[at + 1:] if depth == 1 and name in ("first", "second")]
assert inside == order, (inside, order, tree)
assert (2, "Position") in tree and (2, "Drawing") in tree, tree
assert tree.index((1, "second")) < tree.index((2, "Drawing")), "a layer's rows under it"
assert "top" not in [name for depth, name in tree], "only what's picked"
assert row("Summary")["frames"] == [0, 10, 20], row("Summary")
assert row("group")["frames"] == [0, 10, 20], "a folding row shows what it holds"
assert row("group")["layer"] == group.id and row("Drawing")["layer"] == 0

# Two picked at the top level: both, in the Layers editor's order, the Summary showing both.
top.selected = True
tree = names()
tops = [name for depth, name in tree if depth == 0]
expected = [l.name for l in layers if l.name in ("top", "group")]
assert tops == ["Summary"] + expected, (tops, expected)
assert row("Summary")["frames"] == [0, 10, 20, 30], row("Summary")
assert (1, "Opacity") in tree
top.selected = False

# Picking the drawing alone: the group it is in is listed dimmed, for where it is.
layers.active = second
tree = names()
assert (0, "group") in tree and (1, "second") in tree and row("group")["dim"], tree

# Folds: the picked layer starts open, the project keeps a fold (saved with it).
layers.active = group
assert row("group", True)["open"], "the picked layer starts open"
assert not row("first", True)["open"], "the rest start folded"
assert "Position" not in [r["name"] for r in rows(True) if r["depth"] == 2], "a folded layer hides its rows"
wm.set_ui_open("Timeline/" + row("first")["fold"], True)
assert row("first", True)["open"]
assert (2, "Position") in names(True)
wm.set_ui_open("Timeline/" + row("group")["fold"], False)
assert names(True) == [(0, "Summary"), (0, "group")], names(True)

# Points keyed point by point: a Points folder, with the selected points' keys red under it.
wm.set_ui_open("Timeline/" + row("group")["fold"], True)
second.key_whole_drawing = False
layers.active = second
ops.mesh.select_none()
second.select_points([0])
pts = row("Points")
assert pts["fold"] and pts["frames"] == [0, 20], pts
sel = row("Selected Points")
assert sel["red"] and sel["depth"] == pts["depth"] + 1, sel

# A bone layer: its own channels, then Bones, each bone a folder of its channels (picked bones red).
ops.layer.new_bone()
rig = layers.active
rig.name = "rig"
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0")
ops.armature.bone_add(name="leg", head_x="0", head_y="0", tail_x="0", tail_y="-1")
doc.frame_current = 5
rig.bone_set("arm", angle=0.5)
rig.bone_set("leg", angle=0.25)
assert ops.anim.keyframe_insert() == {'FINISHED'}
doc.frame_current = 0
rig.select_bone("arm")
tree = names()
at = tree.index((0, "rig"))
mine = tree[at:]
assert (1, "Bones") in mine and (2, "arm") in mine and (3, "Angle") in mine and (2, "leg") in mine, mine
assert row("arm")["red"] and not row("leg")["red"], "the picked bone is red"
assert row("Bones", True)["open"] and row("arm", True)["open"], "a picked bone starts open"
assert not row("leg", True)["open"]

# The camera, once keyed, comes last.
cam = oanim.data.camera
assert ops.anim.keyframe_toggle(layer=str(cam.id), paths="rotation") == {'FINISHED'}
tree = names()
assert (0, "Camera") in tree and tree.index((0, "Camera")) > tree.index((0, "rig")), tree

print("timeline tree: ok")
