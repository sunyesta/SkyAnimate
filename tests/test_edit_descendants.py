"""Edit Descendants: a group picked or active edits every drawing inside it, however deep, as if each were
picked. On for groups, off for bone groups; saved with the project.

Run with:  OpenAnimate --background --python tests/test_edit_descendants.py
"""

import os
import shutil
import tempfile

import oanim

layers = oanim.data.layers
doc = oanim.data.document

# A group holding a drawing, and a group inside it holding another.
first = layers.active
first.add_curve([(0.0, 0.0), (1.0, 0.0)])
oanim.ops.layer.group()
outer = layers.active
assert outer.type == "GROUP" and outer.edit_descendants, "on for groups"

oanim.ops.layer.new()
second = layers.active
second.add_curve([(0.0, 1.0), (1.0, 1.0)])
oanim.ops.layer.group()
inner = layers.active
assert oanim.ops.layer.move_to_group(group=str(outer.id)) == {'FINISHED'}
assert inner.parent == outer and first.parent == outer and second.parent == inner


def xs(layer):
    return [round(p[0], 4) for p in layer.points]


# The outer group active: select all and move reaches both drawings, the nested one too.
layers.active = outer
assert oanim.ops.mesh.select_all() == {'FINISHED'}
assert oanim.ops.mesh.translate(move_x="0.5", move_y="0") == {'FINISHED'}
assert xs(first) == [0.5, 1.5] and xs(second) == [0.5, 1.5], (xs(first), xs(second))
oanim.ops.ed.undo()

# A hidden or locked drawing inside stays out.
second.locked = True
layers.active = outer
oanim.ops.mesh.select_all()
oanim.ops.mesh.translate(move_x="0.5", move_y="0")
assert xs(first) == [0.5, 1.5] and xs(second) == [0.0, 1.0], "locked: left alone"
oanim.ops.ed.undo()
second.locked = False

# Off: picking the group edits nothing inside it.
outer.edit_descendants = False
layers.active = outer
oanim.ops.mesh.select_all()
oanim.ops.mesh.translate(move_x="0.5", move_y="0")
assert xs(first) == [0.0, 1.0] and xs(second) == [0.0, 1.0], (xs(first), xs(second))
outer.edit_descendants = True

# The timeline lists the animated drawings inside a picked group, as it does for a bone group.
doc.frame_current = 10
first.location_x = 0.25
doc.frame_current = 11
layers.active = outer
names = [r[1] for r in oanim.wm.timeline_rows()]
assert first.name in names, names
outer.edit_descendants = False
layers.active = outer
assert first.name not in [r[1] for r in oanim.wm.timeline_rows()], "off: only the group"
outer.edit_descendants = True
doc.frame_current = 0

# Keying a picked group keys the layers inside it: I, Alt I and the key buttons.
def key_frames(layer, path):
    return [k["frame"] for k in layer.keyframes(path)]


doc.frame_current = 20
layers.active = outer
assert oanim.ops.anim.keyframe_insert() == {'FINISHED'}
assert 20 in key_frames(first, "position") and 20 in key_frames(second, "position"), "I keys them"
assert oanim.ops.anim.keyframe_delete() == {'FINISHED'}
assert 20 not in key_frames(first, "position") and 20 not in key_frames(second, "position"), "Alt I unkeys them"
oanim.ops.anim.keyframe_toggle(layer=str(outer.id), paths="opacity")
assert 20 in key_frames(outer, "opacity") and 20 in key_frames(first, "opacity") and 20 in key_frames(second, "opacity")
oanim.ops.anim.keyframe_toggle(layer=str(outer.id), paths="opacity")
assert 20 not in key_frames(first, "opacity") and 20 not in key_frames(second, "opacity"), "the button again unkeys"
outer.edit_descendants = False
oanim.ops.anim.keyframe_toggle(layer=str(outer.id), paths="opacity")
assert 20 in key_frames(outer, "opacity") and 20 not in key_frames(first, "opacity"), "off: the group alone"
outer.edit_descendants = True
doc.frame_current = 0

# Bone groups start off; converting between the two sets it to match.
oanim.ops.layer.new_bone()
assert not layers.active.edit_descendants, "off for bone groups"
layers.active = inner
oanim.ops.layer.convert_to_bone()
assert not inner.edit_descendants
oanim.ops.layer.convert_to_group()
assert inner.edit_descendants

# Saved with the project.
outer.edit_descendants = False
folder = tempfile.mkdtemp(prefix="oanim_descendants_")
path = os.path.join(folder, "groups.oanim")
oanim.ops.wm.save_as_mainfile(filepath=path)
oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
assert not oanim.data.layers[outer.name].edit_descendants
assert oanim.data.layers[inner.name].edit_descendants
shutil.rmtree(folder, ignore_errors=True)

print("edit descendants: ok")
