"""The weight tools work on the active layer or group, so only one bone layer's bones are in play: other picked layers
are left alone, a group's drawings go together, those of a bone layer inside it stay out, and nothing under a Parent Bone
is weighed. Automatic Weights can use only the selected bones of that bone layer, and none selected is not all of them.

Run with:  OpenAnimate --background --python tests/test_weight_tools_active.py
"""
import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0


def find(layer):
    return next(l for l in layers.all() if l.id == layer.id)


def drawing(name, points, group):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    layer.add_curve(points)
    assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
    layer = find(layer)
    layers.active = layer
    return layer


def bones_of(layer):
    return sorted({b for w in find(layer).weights for b in w})


def pick(rig, *names):
    layers.active = find(rig)
    ops.armature.select_none()
    for name in names:
        find(rig).select_bone(name, extend=True)


assert ops.layer.new_bone() == {'FINISHED'}
outer = layers.active
outer.name = "Outer"
ops.armature.bone_add(name="hip", head_x="0", head_y="0", tail_x="0", tail_y="1", parent="")
ops.armature.bone_add(name="chest", head_x="0", head_y="1", tail_x="0", tail_y="2", parent="")
assert ops.layer.new_bone() == {'FINISHED'}
inner = layers.active
inner.name = "Inner"
assert ops.layer.move_to_group(group=str(outer.id)) == {'FINISHED'}
inner = find(inner)
layers.active = inner
ops.armature.bone_add(name="thigh", head_x="1", head_y="0", tail_x="1", tail_y="-1", parent="")
ops.armature.bone_add(name="shin", head_x="1", head_y="-1", tail_x="1", tail_y="-2", parent="")
top = drawing("Top", [(0.0, 0.2), (0.0, 1.8)], outer)
low = drawing("Low", [(1.0, -0.2), (1.0, -1.8)], inner)

# --- Selected bones ---------------------------------------------------------------------------------------------------
pick(outer)
layers.active = top
assert ops.weight.automatic(bones="SELECTED") == {'CANCELLED'}, "none selected isn't all of them"
assert not any(find(top).weights)
pick(outer, "chest")
layers.active = top
assert ops.weight.automatic(bones="SELECTED") == {'FINISHED'}
assert bones_of(top) == ["chest"], bones_of(top)
assert ops.weight.automatic() == {'FINISHED'}
assert bones_of(top) == ["chest", "hip"], "every bone, as before"
# What it replaces: the points, all of them with points=ALL whatever is selected.
pick(outer, "hip")
layers.active = top
find(top).select_points([0])
assert ops.weight.automatic(bones="SELECTED") == {'FINISHED'}
assert list(find(top).weights[0]) == ["hip"] and "chest" in find(top).weights[1], "the selected point"
assert ops.weight.automatic(bones="SELECTED", points="ALL") == {'FINISHED'}
assert list(find(top).weights[1]) == ["hip"], "points=ALL"
ops.mesh.select_none()

# --- Nested bone groups: a drawing's weights are its own bone layer's -------------------------------------------------
pick(outer, "chest")
pick(inner)
layers.active = low
assert ops.weight.automatic(bones="SELECTED") == {'CANCELLED'}, "the bone selected is in the bone layer around it"
assert not any(find(low).weights), find(low).weights
pick(outer)
pick(inner, "thigh")
layers.active = top
kept = find(top).weights
assert ops.weight.automatic(bones="SELECTED") == {'CANCELLED'}, "and the other way round"
assert find(top).weights == kept
layers.active = low
assert ops.weight.automatic(bones="SELECTED") == {'FINISHED'}
assert bones_of(low) == ["thigh"], bones_of(low)

# --- Only the active layer ----------------------------------------------------------------------------------------------
other = drawing("Other", [(0.3, 0.2), (0.3, 1.8)], outer)
layers.active = other
find(top).selected = True
kept = find(top).weights
assert ops.weight.automatic() == {'FINISHED'}
assert all(find(other).weights) and find(top).weights == kept, "the other picked layer stays as it was"
assert ops.weight.clear() == {'FINISHED'}
assert not any(find(other).weights) and find(top).weights == kept
find(top).selected = False

# --- A group, its drawings together --------------------------------------------------------------------------------------
ops.layer.new_group()
group = layers.active
assert ops.layer.move_to_group(group=str(outer.id)) == {'FINISHED'}
group = find(group)
first = drawing("First", [(0.1, 0.2), (0.1, 1.8)], group)
ops.layer.new_group()
deeper = layers.active
assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
deeper = find(deeper)
second = drawing("Second", [(0.2, 0.2), (0.2, 1.8)], deeper)
followed = drawing("Followed", [(0.4, 0.2), (0.4, 1.8)], group)
assert ops.layer.parent_bone_set(bone="hip") == {'FINISHED'}
assert ops.layer.new_bone() == {'FINISHED'}
sub = layers.active
sub.name = "Sub"
assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
sub = find(sub)
layers.active = sub
ops.armature.bone_add(name="tail", head_x="2", head_y="0", tail_x="2", tail_y="1", parent="")
tail_art = drawing("TailArt", [(2.0, 0.2), (2.0, 0.8)], sub)
assert ops.weight.automatic() == {'FINISHED'}
kept = find(tail_art).weights

pick(outer, "chest")
layers.active = find(group)
assert ops.weight.automatic(bones="SELECTED") == {'FINISHED'}
assert bones_of(first) == ["chest"] and bones_of(second) == ["chest"], "however deep"
assert find(tail_art).weights == kept, "a bone layer inside has its own bones"
assert not any(find(followed).weights), "a Parent Bone moves that one as a whole"
assert ops.weight.clear() == {'FINISHED'}
assert not any(find(first).weights) and not any(find(second).weights) and find(tail_art).weights == kept
assert find(followed).parent_bone == "hip"
assert ops.weight.clear() == {'CANCELLED'}, "nothing left to clear"
ops.ed.undo()
assert bones_of(first) == ["chest"], "one undo step"

# With Edit Descendants off, its drawings aren't edited from the group.
find(group).edit_descendants = False
layers.active = find(group)
assert ops.weight.automatic() == {'CANCELLED'}
find(group).edit_descendants = True

# A bone layer is such a group: its own drawings, not those of a bone layer inside it.
keep_low = find(low).weights
layers.active = find(outer)
assert ops.weight.automatic() == {'FINISHED'}
assert bones_of(top) == ["chest", "hip"] and bones_of(first) == ["chest", "hip"]
assert find(low).weights == keep_low, "Inner has its own bones"

# --- Nothing under a Parent Bone ---------------------------------------------------------------------------------------
layers.active = find(group)
assert ops.layer.parent_bone_set(bone="hip") == {'FINISHED'}
assert ops.weight.clear() == {'CANCELLED'}, "the group follows a bone"
layers.active = find(first)
kept = find(first).weights
assert ops.weight.clear() == {'CANCELLED'}, "and so does what's in it"
assert find(first).weights == kept
layers.active = find(group)
assert ops.layer.parent_bone_set(bone="") == {'FINISHED'}

# --- With a weight tool on, selecting works on the same layers ----------------------------------------------------------
layers.active = find(group)
assert ops.wm.tool_weight_select() == {'FINISHED'}
for layer in (first, second, followed, tail_art, top):
    find(layer).select_points(range(find(layer).point_count), False)
assert ops.mesh.select_all(action="SELECT") == {'FINISHED'}
selected = {l.name: sum(1 for q in l.points if q[2]) for l in (find(first), find(second), find(followed), find(tail_art), find(top))}
assert selected == {"First": 2, "Second": 2, "Followed": 0, "TailArt": 0, "Top": 0}, selected
assert ops.mesh.select_none() == {'FINISHED'}
assert ops.wm.tool_weight_paint() == {'FINISHED'}

print("Weight tool tests passed")
