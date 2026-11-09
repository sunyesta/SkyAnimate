"""The timeline's Keyed Here list: Ctrl-click (or right-click) a key to see every property keyed on
that frame under its row, and pick one to go to it: the frame jumps there, then a bone's property
selects the bone, a point's selects the point (staying in the layer worked in when it can edit that
point, else going to the point's layer), and anything else is shown in the Properties editor, its
panel opened and its row flashed.

Run with:  OpenAnimate --background --python tests/test_keyed_here.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0

# A rig with a bone, a drawing inside it (one editable with the bones), and a layer outside.
oanim.ops.layer.new_bone()
rig = layers.active
rig.key_whole_drawing = False  # keyed point by point here
rig.key_together = False  # unfolded: keyed channel by channel
oanim.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
oanim.ops.layer.new()
art = layers.active
art.key_whole_drawing = False  # keyed point by point here
art.key_together = False  # unfolded: keyed channel by channel
art.add_curve([(0.0, 1.0), (1.0, 1.0), (2.0, 1.0)])
oanim.ops.layer.move_to_group(group=str(rig.id))
oanim.ops.layer.new()
lips = layers.active
lips.key_whole_drawing = False  # keyed point by point here
lips.key_together = False  # unfolded: keyed channel by channel
lips.name = "lips"
lips.add_curve([(0.0, 2.0), (1.0, 2.0)])
oanim.ops.layer.move_to_group(group=str(rig.id))
lips.edit_with_bones = True
oanim.ops.layer.new()
other = layers.active
other.key_whole_drawing = False  # keyed point by point here
other.key_together = False  # unfolded: keyed channel by channel
other.name = "other"
other.add_curve([(5.0, 0.0), (6.0, 0.0)])

# Keys on frame 10: the bone, two of art's points, a lips point, other's opacity, the gravity.
rig.bone_set("arm", angle=0.5)
rig.keyframe_insert('bones["arm"].angle', 10)
art.keyframe_insert("points[0].pos", 10)
art.keyframe_insert("points[2].pos", 10)
lips.keyframe_insert("points[1].pos", 10)
other.keyframe_insert("opacity", 10)
oanim.data.simulation.keyframe_insert("gravity", 10)

assert 'bones["arm"].angle' in rig.keyed_at(10)
assert set(art.keyed_at(10)) >= {"points[0].pos", "points[2].pos"} and "points[1].pos" not in art.keyed_at(10)
assert art.keyed_at(11) == [], "nothing keyed on frame 11"


def go(layer, paths, frame=10, extend=False):
    return oanim.ops.anim.keyed_go(layer=str(layer.id), paths="|".join(paths), frame=str(frame),
                                 extend="1" if extend else "0")


# --- A bone's property: jump to the frame, select the bone ---------------------------------------------
layers.active = other
assert go(rig, ['bones["arm"].angle']) == {'FINISHED'}
assert doc.frame_current == 10, "it jumps to the key's frame"
assert layers.active == rig
arm = next(b for b in rig.bones if b["name"] == "arm")
assert arm["selected"] and arm["active"]
# frame -1 (Alt) keeps the frame.
doc.frame_current = 3
assert go(rig, ['bones["arm"].angle'], frame=-1) == {'FINISHED'} and doc.frame_current == 3

# --- Points: go to their layer when they can't be edited from here ---------------------------------------
layers.active = other
assert go(art, ["points[0].pos", "points[2].pos"]) == {'FINISHED'}
assert layers.active == art, "other can't edit art's points: go to art"
assert [p[2] for p in art.points] == [True, False, True], art.points
# Shift adds.
assert go(art, ["points[1].pos"], extend=True) == {'FINISHED'}
assert all(p[2] for p in art.points)

# Stay in the bone layer when its drawing is in one of its shown bone collections.
layers.active = rig
assert go(lips, ["points[1].pos"]) == {'FINISHED'}
assert layers.active == rig, "the rig edits lips' points: stay there"
assert [p[2] for p in lips.points] == [False, True]

# --- Anything else: shown in the Properties editor ------------------------------------------------------
assert go(other, ["opacity"]) == {'FINISHED'}
tab, layer_id, path, panel = oanim.wm.properties_flash()
assert layers.active == other and tab == 1 and layer_id == other.id and path == "opacity" and panel == "Compositing"
assert go(oanim.data.simulation, ["gravity"]) == {'FINISHED'}
tab, layer_id, path, panel = oanim.wm.properties_flash()
assert tab == 0 and path == "gravity" and panel == "Simulation", oanim.wm.properties_flash()

# A bone constraint's setting: select the bone, and show it in Bone Constraints (box opened).
layers.active = rig
doc.frame_current = 0
rig.select_bone("arm")
assert oanim.ops.armature.constraint_add(type="LIMIT_ANGLE", bone="arm") == {'FINISHED'}
rig.keyframe_insert('bones["arm"].constraints[0].influence', 10)
assert go(rig, ['bones["arm"].constraints[0].influence']) == {'FINISHED'}
tab, layer_id, path, panel = oanim.wm.properties_flash()
assert tab == 4 and path == 'bones["arm"].constraints[0].influence'

# Unknown things are refused.
assert go(rig, ['bones["nobody"].angle']) == {'CANCELLED'}

print("Keyed Here tests passed")
