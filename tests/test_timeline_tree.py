"""The timeline's rows follow the Layers editor: Summary first (every key below it), then Pinned, then each
shown layer in the Layers editor's order and nesting, folding to its own channels, its Drawing or Points,
its Bones (a folder per bone) and a Media layer's clips; the camera and Simulation last. Folding is a view kept by the
project and keys nothing; a layer keying together is a chained row that never opens.

Run with:  OpenAnimate --background --python tests/test_timeline_tree.py
"""

import oanim

oanim.data.document.auto_freeze_keys = False  # keyed channel by channel here, not holding the poses either side

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
first.key_together = False  # open: its own rows show
first.name = "first"
ops.layer.new()
top = layers.active
top.key_together = False  # open: its own rows show
top.name = "top"
layers.active = first
first.add_curve([(0.0, 0.0), (1.0, 0.0)])
ops.layer.group()
group = layers.active
group.name = "group"
ops.layer.new()
second = layers.active
second.key_together = False  # open: its own rows show
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
assert tree[:2] == [(0, "Summary"), (0, "group")] and row("group")["label"] == "group", \
    ("the Summary, then the group: its own row, open a heading", tree)
order = [l.name for l in group.children]
at = tree.index((0, "group"))
inside = [name for depth, name in tree[at + 1:] if depth == 1 and name in ("first", "second")]
assert inside == order, (inside, order, tree)
assert (2, "Layer") in tree and (3, "Position") in tree and (2, "Drawing") in tree, tree
assert tree.index((1, "second")) < tree.index((2, "Drawing")), "a layer's rows under it"
assert "top" not in [name for depth, name in tree], "only what's picked"
assert row("group")["frames"] == [], "open (not keying together): no keys of its own"
# Folded, it shows what it holds and keys nothing: folding is a view.
assert wm.timeline_row_fold(1, False) and not group.key_together
assert row("group", True)["frames"] == [0, 10, 20] and row("group", True)["label"] == "group", rows(True)
assert names(True) == [(0, "Summary"), (0, "group")], "folded: what it holds hidden"
assert next(r for r in rows() if r["name"] == "Drawing")["frames"] == [0, 20], "no fillers: nothing keyed"
assert wm.timeline_row_fold(1, True) and not group.key_together
assert row("group")["frames"] == [] and (2, "Drawing") in names()
# Keying together: chained, never open, showing what it holds; clicking the chain lets go, and its fillers go.
group.key_together = True
assert row("group")["chain"] == "TOGETHER" and not row("group")["open"], row("group")
assert row("group")["frames"] == [0, 10, 20] and names() == [(0, "Summary"), (0, "group")], names()
assert not wm.timeline_row_fold(1, True), "a chained row never opens"
assert wm.timeline_row_chain(1) and not group.key_together
assert row("group")["chain"] == "" and (2, "Drawing") in names()
assert next(r for r in rows() if r["name"] == "Drawing")["frames"] == [0, 20], "the fillers went again"
assert row("group")["layer"] == group.id and row("Drawing")["layer"] == 0

# Two picked at the top level: both, in the Layers editor's order, the Summary showing both.
top.selected = True
tree = names()
tops = [name for depth, name in tree if depth == 0]
expected = [l.name for l in layers if l.name in ("top", "group")]
assert tops == ["Summary"] + expected, (tops, expected)
assert row("Summary")["frames"] == [0, 10, 20, 30], row("Summary")
assert (1, "Layer") in tree and (2, "Opacity") in tree
top.selected = False

# Picking the drawing alone: the group it is in is listed dimmed, for where it is.
layers.active = second
tree = names()
assert (0, "group") in tree and (1, "second") in tree and row("group")["dim"], tree

# Folds: these layers start open and fold as the project keeps them, keying nothing; keying together chains a row
# shut whatever its kept fold.
ops.mesh.select_none()
layers.active = group
assert row("group", True)["open"] and row("first", True)["open"], "open to start with"
assert names(True).count((2, "Layer")) == 2
wm.set_ui_open("Timeline/" + row("first")["fold"], False)
assert not row("first", True)["open"] and names(True).count((2, "Layer")) == 1, "a folded layer hides its rows"
assert not first.key_together, "folding keys nothing"
at = [r["name"] for r in rows(True)].index("first")
assert wm.timeline_row_fold(at, True) and row("first", True)["open"] and not first.key_together
first.key_together = True
assert not row("first", True)["open"] and row("first")["chain"] == "TOGETHER", "Key Together set elsewhere chains it"
first.key_together = False
assert row("first", True)["open"], "let go, its kept fold again"

# Points keyed point by point: a Points folder, with the selected points' keys red under it.
second.key_whole_drawing = False
layers.active = second
ops.mesh.select_none()
second.select_points([0])
pts = row("Points")
assert pts["fold"] and pts["open"] and pts["frames"] == [], ("open: a heading, its keys in its rows", pts)
at = [r["name"] for r in rows(True)].index("Points")
assert wm.timeline_row_fold(at, False)
assert row("Points", True)["frames"] == [0, 20], ("folded: every key inside", row("Points", True))
assert wm.timeline_row_fold(at, True)
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
doc.timeline_only_selected = False  # every bone (Only Selected lists just the picked ones)
tree = names()
at = next(i for i, (d, n) in enumerate(tree) if n == "rig")
d = tree[at][0]
mine = tree[at:]
assert (d + 1, "Bones") in mine and (d + 2, "arm") in mine and (d + 3, "Angle") in mine and (d + 2, "leg") in mine, mine
assert row("arm")["red"] and not row("leg")["red"], "the picked bone is red"
assert row("Bones", True)["open"] and row("arm", True)["open"], "a picked bone starts open"
assert not row("leg", True)["open"]
assert row("Bones", True)["frames"] == [] and row("arm", True)["frames"] == [], "open folders: headings"
assert row("leg", True)["frames"] == [0, 5], ("a folded bone: its keys", row("leg", True))
assert row("Angle", True)["frames"] == [0, 5], "the open bone's keys in its rows"
doc.timeline_only_selected = True

# The camera, once keyed, comes last.
cam = oanim.data.camera
assert ops.anim.keyframe_toggle(layer=str(cam.id), paths="rotation") == {'FINISHED'}
tree = names()
assert (0, "Camera") in tree and tree.index((0, "Camera")) > [n for d, n in tree].index("rig"), tree

# --- New layers: a drawing starts folded (keying together, as its properties are usually animated as one); a group and
# a bone layer start open. Files from before open as they were saved.
assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
assert oanim.data.layers.active.key_together, "the start-up drawing starts folded"
ops.layer.new()
drawing = oanim.data.layers.active
drawing.add_curve([(0.0, 0.0), (1.0, 0.0)])
assert drawing.key_together and not row(drawing.name, True)["open"], rows(True)
ops.layer.group()
assert not oanim.data.layers.active.key_together, "a new group starts open"
ops.layer.new_bone()
assert not oanim.data.layers.active.key_together, "a new bone layer starts open"

print("timeline tree: ok")
