"""The status bar's statistics, as Blender's Scene Statistics: the drawings being edited (points, curves,
segments, shapes, selected/total), a bone layer's bones, the layers; Blender's "Name | Points:1/4 | ..."
format with thousands separators.

Run with:  OpenAnimate --background --python tests/test_statistics.py
"""

import oanim

layers = oanim.data.layers


def stats():
    return oanim.context.statistics


# A square (a closed, filled curve: 4 points, 4 segments, 1 shape) and an open line (2 points, 1 segment).
square = layers.active
square.name = "Square"
square.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True, fill=True)
oanim.ops.mesh.select_none()
s = stats()
assert s["mode"] == "DRAWINGS" and s["name"] == "Square", s
assert s["points"] == (0, 4) and s["curves"] == (0, 1) and s["segments"] == (0, 4), s
assert s["shapes"][1] >= 1 and s["shapes"][0] == 0, s
assert s["layers"] == (1, 1), s

# Two neighbouring points: one segment between them is selected, the curve and shape are not.
square.select_points([0, 1])
s = stats()
assert s["points"] == (2, 4) and s["segments"] == (1, 4) and s["curves"] == (0, 1), s
assert s["shapes"][0] == 0, s
assert s["text"].startswith("Square | Points:2/4 | Curves:0/1 | Segments:1/4 | Shapes:0/"), s["text"]
assert s["text"].endswith("| Layers:1/1"), s["text"]

# All of them: the curve and its shapes are selected too.
oanim.ops.mesh.select_all()
s = stats()
assert s["points"] == (4, 4) and s["curves"] == (1, 1) and s["segments"] == (4, 4), s
assert s["shapes"][0] == s["shapes"][1], s

# A second drawing picked with it: both count, and "Drawings:2" says how many.
oanim.ops.layer.new()
line = layers.active
line.add_curve([(0.0, 2.0), (1.0, 2.0)])
square.select = True
s = stats()
assert s["drawings"] == 2 and s["points"][1] == 6 and s["segments"][1] == 5, s
assert "| Drawings:2 |" in s["text"], s["text"]
assert s["layers"] == (2, 2), s

# A bone layer: its bones, selected by body or a joint.
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
rig.name = "Rig"
oanim.ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="0", tail_y="1", parent="")
oanim.ops.armature.bone_add(name="b", head_x="0", head_y="1", tail_x="0", tail_y="2", parent="a")
oanim.ops.armature.bone_add(name="c", head_x="0", head_y="2", tail_x="0", tail_y="3", parent="b")
oanim.ops.armature.select_none()
rig.select_bone("b")
s = stats()
assert s["mode"] == "BONES" and s["bones"] == (1, 3), s
assert s["text"].startswith("Rig | Bones:1/3 | Layers:"), s["text"]
rig.select_bone("c", extend=True, part="TAIL")
assert stats()["bones"] == (2, 3), stats()

# Nothing editable active (a group with Edit Descendants off): the layers and every point.
oanim.ops.layer.new()
oanim.ops.layer.group()
group = layers.active
group.edit_descendants = False
for l in layers.all():
    l.select = l.id == group.id
s = stats()
assert s["mode"] == "DOCUMENT", s
assert s["text"].endswith("| Points:6"), s["text"]

# Thousands separators.
big = layers.active
oanim.ops.layer.new()
many = layers.active
many.add_curve([(i * 0.001, 0.0) for i in range(1234)])
s = stats()
assert s["points"][1] == 1234 and "Points:0/1,234" in s["text"], s["text"]

print("statistics: ok")
