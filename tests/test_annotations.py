"""Annotations and drawing guides: notes on annotation layers (strokes with arrowheads, the eraser that
splits them) and guides (line, perspective, grid) with Ctrl's snapping. Neither is part of the drawing:
no layers, never picked; both are undone and saved with the file.

Run with:  OpenAnimate --background --python tests/test_annotations.py
"""

import math
import pathlib
import tempfile

import oanim

data = oanim.data
doc = data.document
layers = data.layers
ops = oanim.ops
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_annotations_"))


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


assert data.annotations == [] and data.active_annotation == -1 and data.guides == []
layer_count = len(layers.all())

# --- Annotations ------------------------------------------------------------------------------------
# The first stroke makes a layer, "Note".
assert ops.annotation.draw(points="0 0 1 0 1 1") == {'FINISHED'}
(note,) = data.annotations
assert note["name"] == "Note" and data.active_annotation == 0
assert [close(p, q) for p, q in zip(note["strokes"][0]["points"], [(0, 0), (1, 0), (1, 1)])] == [True] * 3
assert note["strokes"][0]["start"] == "NONE" and note["strokes"][0]["end"] == "NONE"
assert len(layers.all()) == layer_count, "not a layer"
assert oanim.wm.undo_history()[0][-1] == "Annotate"

# A line with arrowheads.
assert ops.annotation.draw(points="-2 -2 2 -2", start="DOT", end="ARROW") == {'FINISHED'}
line = data.annotations[0]["strokes"][1]
assert (line["start"], line["end"]) == ("DOT", "ARROW")
assert ops.annotation.draw(points="0 0") == {'CANCELLED'}, "a stroke needs two points"
assert ops.annotation.draw(points="0 0 1 1", end="BOW") == {'CANCELLED'}

# The eraser splits the line where its circle crosses it: the cut ends lose their arrowheads.
assert ops.annotation.erase(x="0", y="-2", radius="0.5") == {'FINISHED'}
strokes = data.annotations[0]["strokes"]
pieces = [s for s in strokes if all(abs(p[1] + 2) < 1e-6 for p in s["points"])]
assert len(pieces) == 2, strokes
left, right = sorted(pieces, key=lambda s: s["points"][0][0])
assert close(left["points"][-1], (-0.5, -2)) and close(right["points"][0], (0.5, -2)), (left, right)
assert (left["start"], left["end"], right["start"], right["end"]) == ("DOT", "NONE", "NONE", "ARROW")
assert ops.annotation.erase(x="9", y="9", radius="0.5") == {'CANCELLED'}, "nothing there"
# Rubbing out a whole stroke takes it away.
assert ops.annotation.erase(x="-1.25", y="-2", radius="1") == {'FINISHED'}
assert len(data.annotations[0]["strokes"]) == 2

# Layers: added above the active one and made active, unique names, hidden and locked ones left alone.
assert ops.annotation.layer_add() == {'FINISHED'}
assert [l["name"] for l in data.annotations] == ["Note", "Note.001"] and data.active_annotation == 1
assert ops.annotation.layer_set(name="Director", color="0 0.5 1", thickness="6") == {'FINISHED'}
director = data.annotations[1]
assert director["name"] == "Director" and close(director["color"], (0, 0.5, 1, 1)) and director["thickness"] == 6
assert ops.annotation.layer_set(thickness="500") == {'FINISHED'} and data.annotations[1]["thickness"] == 20
assert ops.annotation.layer_set(color="red") == {'CANCELLED'}
assert ops.annotation.draw(points="5 5 6 6") == {'FINISHED'}
assert len(data.annotations[1]["strokes"]) == 1 and len(data.annotations[0]["strokes"]) == 2
assert ops.annotation.layer_set(locked="1") == {'FINISHED'}
assert ops.annotation.draw(points="5 6 6 7") == {'CANCELLED'}, "locked"
assert ops.annotation.erase(x="5.5", y="5.5", radius="1") == {'CANCELLED'}, "locked: not rubbed out"
assert ops.annotation.layer_set(locked="0", hidden="1") == {'FINISHED'}
assert ops.annotation.erase(x="5.5", y="5.5", radius="1") == {'CANCELLED'}, "hidden: not rubbed out"
assert ops.annotation.layer_set(index="0", active="1") == {'FINISHED'} and data.active_annotation == 0
assert ops.annotation.layer_remove(index="1") == {'FINISHED'}
assert [l["name"] for l in data.annotations] == ["Note"] and data.active_annotation == 0

# Undone like any edit.
ops.ed.undo()
assert [l["name"] for l in data.annotations] == ["Note", "Director"]
ops.ed.redo()
assert [l["name"] for l in data.annotations] == ["Note"]

# --- Drawing guides ---------------------------------------------------------------------------------
assert ops.guide.add(type="LINE", points="0 0 1 0") == {'FINISHED'}
assert data.guides[0]["type"] == "LINE" and data.active_guide == 0
assert ops.guide.add(type="CIRCLE", points="0 0 1 0") == {'CANCELLED'}
assert ops.guide.add(type="LINE", points="") == {'CANCELLED'}
# A guide gets as many handles as its kind has.
assert ops.guide.add(type="GRID", points="3 3") == {'FINISHED'}
assert len(data.guides[1]["points"]) == 2 and data.active_guide == 1
assert ops.guide.add(type="PERSPECTIVE", points="-5 2 5 2", lines="999") == {'FINISHED'}
persp = data.guides[2]
assert len(persp["points"]) == 2 and persp["lines"] == 180
# One to three vanishing points: the third goes below the middle of the first two.
assert ops.guide.set(count="3") == {'FINISHED'}
third = data.guides[2]["points"][2]
assert abs(third[0]) < 1e-5 and third[1] < 2, third
assert ops.guide.set(count="1") == {'FINISHED'} and len(data.guides[2]["points"]) == 1
assert ops.guide.set(count="2") == {'FINISHED'} and len(data.guides[2]["points"]) == 2

# Handles move; Ctrl's snapping (snap=1) takes the nearest of a point of the drawing and another
# guide's handle, else a point on another guide's line, else the line's turn in 15 degree steps.
oanim.ops.layer.new()
art = layers.active
art.add_curve([(10.0, 10.0), (12.0, 10.0)])
assert ops.guide.move_point(index="0", point="1", x="10.02", y="9.99", snap="1", radius="0.1") == {'FINISHED'}
assert close(data.guides[0]["points"][1], (10, 10)), ("to the drawing's point", data.guides[0])
assert ops.guide.move_point(index="0", point="1", x="3.03", y="2.98", snap="1", radius="0.1") == {'FINISHED'}
assert close(data.guides[0]["points"][1], (3, 3)), ("to the grid's handle", data.guides[0])
assert ops.guide.move_point(index="0", point="1", x="0.5", y="2.04", snap="1", radius="0.1") == {'FINISHED'}
assert close(data.guides[0]["points"][1], (0.5, 2)), ("onto the horizon", data.guides[0])
assert ops.guide.move_point(index="0", point="1", x="1", y="-0.62", snap="1", radius="0.1") == {'FINISHED'}
p = data.guides[0]["points"][1]
angle = math.degrees(math.atan2(p[1], p[0]))
assert abs(angle - round(angle / 15) * 15) < 1e-3 and abs(angle + 30) < 1e-3, ("15 degree steps", p, angle)
assert ops.guide.move_point(index="0", point="1", x="1", y="-0.62") == {'FINISHED'}
assert close(data.guides[0]["points"][1], (1, -0.62)), "no snap without it"
# Locked: its handles stay.
assert ops.guide.set(index="0", locked="1") == {'FINISHED'}
assert ops.guide.move_point(index="0", point="1", x="7", y="7") == {'CANCELLED'}
assert ops.guide.set(index="0", locked="0", hidden="1") == {'FINISHED'}
assert data.guides[0]["hidden"]

# Guides aren't the drawing: picking goes through them, and the drawing is as it was.
assert art.points[0][:2] == (10.0, 10.0)

# Delete: the picked one by default, the indices after it close up.
assert ops.guide.set(index="2", active="1") == {'FINISHED'} and data.active_guide == 2
assert ops.guide.delete(index="0") == {'FINISHED'}
assert [g["type"] for g in data.guides] == ["GRID", "PERSPECTIVE"] and data.active_guide == 1
assert ops.guide.delete() == {'FINISHED'} and [g["type"] for g in data.guides] == ["GRID"]
assert data.active_guide == -1
ops.ed.undo()
assert [g["type"] for g in data.guides] == ["GRID", "PERSPECTIVE"]

# --- Guide lines: sz_draw_guides' measures --------------------------------------------------------------
def line(index=-1):
    return data.guides[index]["points"]


def angle_of(points):
    return math.degrees(math.atan2(points[1][1] - points[0][1], points[1][0] - points[0][0]))


def length_of(points):
    return math.dist(points[0], points[1])


assert ops.guide.add(type="LINE", points="0 0 2 0") == {'FINISHED'}
# An end goes where it's dragged; Ctrl's snap takes 45 degree steps, or the line's old angle if nearer.
assert ops.guide.drag(grab="END", end="1", press="2 0", mouse="3 1") == {'FINISHED'}
assert close(line()[1], (3, 1))
assert ops.guide.drag(grab="END", end="1", press="3 1", mouse="1 0.98", snap="1") == {'FINISHED'}
assert abs(angle_of(line()) - 45) < 1e-3, angle_of(line())
ops.ed.undo()
assert close(line()[1], (3, 1)), "one undo for a drag"
old = angle_of(line())
assert ops.guide.drag(grab="END", end="1", press="3 1", mouse="2 0.7", snap="1") == {'FINISHED'}
assert abs(angle_of(line()) - old) < 1e-3, ("keeps its old angle", angle_of(line()), old)
assert ops.guide.set(points="0 0 2 0") == {'FINISHED'}
# An end's turn keeps the length, whichever way, and the other end stays.
assert ops.guide.drag(grab="ROTATE_END", end="1", press="2.2 0", mouse="0 5") == {'FINISHED'}
assert close(line()[0], (0, 0)) and close(line()[1], (0, 2)), line()
assert ops.guide.drag(grab="ROTATE_END", end="0", press="0 0", mouse="1 2") == {'FINISHED'}
assert close(line()[1], (0, 2)) and abs(length_of(line()) - 2) < 1e-4 and close(line()[0], (2, 2)), line()
assert ops.guide.set(points="0 0 2 0") == {'FINISHED'}
# Round the middle: by how far the mouse went round it, from wherever it took hold.
assert ops.guide.drag(grab="ROTATE_CENTER", press="1 1", mouse="0 0") == {'FINISHED'}
assert abs(length_of(line()) - 2) < 1e-4 and close(((line()[0][0] + line()[1][0]) / 2, (line()[0][1] + line()[1][1]) / 2), (1, 0))
assert abs((angle_of(line()) % 180) - 90) < 1e-3, angle_of(line())
assert ops.guide.set(points="0 0 2 0") == {'FINISHED'}
assert ops.guide.drag(grab="ROTATE_CENTER", press="1 1", mouse="0.2 1.1", snap="1") == {'FINISHED'}
assert abs(angle_of(line()) - 45) < 1e-3, angle_of(line())
# Move: all of it; Ctrl keeps it to the axes.
assert ops.guide.set(points="0 0 2 0") == {'FINISHED'}
assert ops.guide.drag(grab="MOVE", press="1 0", mouse="2 1") == {'FINISHED'}
assert close(line()[0], (1, 1)) and close(line()[1], (3, 1))
assert ops.guide.drag(grab="MOVE", press="0 0", mouse="1 0.2", snap="1") == {'FINISHED'}
assert abs(line()[0][1] - 1) < 1e-4 and line()[0][0] > 1.5, line()
assert ops.guide.drag(grab="SPIN", press="0 0", mouse="1 1") == {'CANCELLED'}
# Mirror across the document's centre; locked ones stay.
assert ops.guide.set(points="1 2 3 4") == {'FINISHED'}
assert ops.guide.mirror(axis="X") == {'FINISHED'}
assert close(line()[0], (-1, 2)) and close(line()[1], (-3, 4))
assert ops.guide.mirror(axis="Y") == {'FINISHED'}
assert close(line()[0], (-1, -2)) and close(line()[1], (-3, -4))
assert ops.guide.mirror(axis="Z") == {'CANCELLED'}
assert ops.guide.set(locked="1") == {'FINISHED'}
assert ops.guide.mirror(axis="X") == {'CANCELLED'} and ops.guide.drag(press="0 0", mouse="1 1") == {'CANCELLED'}
assert ops.guide.set(locked="0") == {'FINISHED'}
# Only a line's own stretch snaps now it ends: past its end, nothing.
assert ops.guide.add(type="LINE", points="20 0 22 0") == {'FINISHED'}
assert ops.guide.add(type="LINE", points="30 30 31 31") == {'FINISHED'}
assert ops.guide.move_point(point="1", x="21", y="0.05", snap="1", radius="0.1") == {'FINISHED'}
assert close(line()[1], (21, 0)), ("onto the line", line())
assert ops.guide.move_point(point="1", x="25", y="0.05", snap="1", radius="0.1") == {'FINISHED'}
assert not close(line()[1], (25, 0)), ("past its end", line())
ops.guide.delete()
ops.guide.delete(index=str(len(data.guides) - 1))
# One colour for all of them, saved with the file below; a new document starts red and solid.
assert data.guide_settings == {"show": True, "hue": 0, "opacity": 100}
assert ops.guide.settings(hue="999", opacity="-5", show="0") == {'FINISHED'}
assert data.guide_settings == {"show": False, "hue": 360, "opacity": 0}
ops.ed.undo()
assert data.guide_settings == {"show": True, "hue": 0, "opacity": 100}
assert ops.guide.settings(hue="200", opacity="60") == {'FINISHED'}
# Delete All, in one undo.
count = len(data.guides)
assert ops.guide.delete_all() == {'FINISHED'} and data.guides == [] and data.active_guide == -1
assert ops.guide.delete_all() == {'CANCELLED'}
ops.ed.undo()
assert len(data.guides) == count
ops.guide.delete()

# The tool, by its variants.
assert ops.wm.tool_annotate(mode="GUIDE_PERSPECTIVE") == {'FINISHED'}
assert ops.wm.tool_annotate(mode="LASSO") == {'CANCELLED'}
assert ops.wm.tool_annotate() == {'FINISHED'}

# --- Saved with the file, with the Annotations overlay -----------------------------------------------
oanim.wm.set_overlay("annotations", False)
path = str(folder / "notes.oanim")
saved_notes, saved_guides = data.annotations, data.guides
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
assert data.annotations == saved_notes, (data.annotations, saved_notes)
assert data.guides == saved_guides
assert data.guide_settings == {"show": True, "hue": 200, "opacity": 60}
assert not oanim.wm.overlays()["annotations"]
# A file from before them opens with none.
assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
assert data.annotations == [] and data.guides == []
assert data.guide_settings == {"show": True, "hue": 0, "opacity": 100}

print("annotations: ok")
