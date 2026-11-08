"""The timeline's channel rows: Summary, Pinned (properties pinned to it, whatever is selected, even
with no keys left), then the layer tree as the Layers editor has it, each layer folding to its own
channels, its drawing and its Bones (each bone a folder of its channels), then Camera and Simulation.

Run with:  OpenAnimate --background --python tests/test_timeline_pins.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm


def rows():
    return wm.timeline_rows()


def names():
    return [(depth, name) for depth, name, red, pinned in rows()]


# A drawing with its opacity keyed, and a bone layer with a keyed bone.
art = layers.active
doc.frame_current = 10
art.opacity = 0.5
doc.frame_current = 11
assert [k["frame"] for k in art.keyframes("opacity")] == [0, 10]

doc.frame_current = 0
oanim.ops.layer.new_bone()
rig = layers.active
assert oanim.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0") == {'FINISHED'}
doc.frame_current = 5
rig.bone_set("arm", angle=0.5)
assert oanim.ops.anim.keyframe_insert() == {'FINISHED'}

def section(rows_, title):
    """The rows under top-level group `title`."""
    at = rows_.index((0, title))
    end = next((i for i in range(at + 1, len(rows_)) if rows_[i][0] == 0), len(rows_))
    return rows_[at + 1:end]


# --- The layer tree: Summary, then the picked bone layer with its own channels and its bones ---------
tree = names()
tops = [name for depth, name in tree if depth == 0]
assert tops == [rig.name], ("the rig alone, open: no Summary over its rows", tops)
assert next(r for r in wm.timeline_row_details() if r["depth"] == 0)["label"] == rig.name
assert art.name not in [name for depth, name in tree], "only the picked layer"
mine = section(tree, rig.name)
for row in [(1, "Position"), (1, "Rotation"), (1, "Scale"), (1, "Bones"), (2, "arm"), (3, "Angle"),
            (3, "Transform"), (3, "Scale")]:
    assert row in mine, (row, mine)
assert mine.index((1, "Rotation")) < mine.index((1, "Bones")) < mine.index((2, "arm")) < mine.index((3, "Angle")),     "the layer's own channels, then its bones, each bone's rows under it"

# Constraints' keyed settings get their own rows under their bone, by constraint and setting.
rig.select_bone("arm")
assert oanim.ops.armature.constraint_add(type="DYNAMICS", bone="arm") == {'FINISHED'}
bounce = 'bones["arm"].constraints[0].angle_spring.bounce'
assert oanim.ops.anim.keyframe_toggle(layer=str(rig.id), paths=bounce) == {'FINISHED'}
mine = section(names(), rig.name)
assert (3, "Dynamics: Angle Bounce") in mine and mine.index((2, "arm")) < mine.index((3, "Dynamics: Angle Bounce")), mine
assert any(red for depth, name, red, pinned in rows() if name == "arm"), "the picked bone's rows are red"

# Clicking another layer brings its rows instead, all its layers in Layers-editor order when both are picked.
art.selected = True
tops = [name for depth, name in names() if depth == 0]
order = [l.name for l in layers]
assert tops == ["Summary"] + [n for n in order if n in (art.name, rig.name)], (tops, order)
assert (1, "Opacity") in section(names(), art.name), section(names(), art.name)
art.selected = False

# --- Pinning: the drawing's opacity, while the bone layer is the one picked -----------------------
assert not art.channel_pinned("opacity")
assert art.pin_channel("opacity") == {'FINISHED'} and art.channel_pinned("opacity")
pinned = rows()
assert pinned[0][:2] == (0, "Summary"), pinned[0]
assert pinned[1][:3] == (0, "Pinned", True), pinned[1]
assert pinned[2] == (1, art.name + ": Opacity", True, True), pinned[2]
assert (0, art.name) not in names(), "only the pinned property shows, not its whole layer"
# Pinned even without keys: an empty row stays.
assert art.pin_channel("rotation") == {'FINISHED'}
assert (1, art.name + ": Rotation") in names() and art.keyframes("rotation") == []
# Its keys can be picked and moved though its layer isn't picked.
assert oanim.ops.anim.keys_select(layers=str(art.id), path="opacity", frame_start="10") == {'FINISHED'}
assert oanim.ops.anim.keys_offset(frames="2") == {'FINISHED'}
assert [k["frame"] for k in art.keyframes("opacity")] == [0, 12], art.keyframes("opacity")
# A bone's property shows as its bone's, and the document's gravity as Simulation's.
assert rig.pin_channel('bones["arm"].angle') == {'FINISHED'}
assert oanim.data.simulation.pin_channel("gravity") == {'FINISHED'}
listed = names()
assert (1, "arm: Angle") in listed and (1, "Simulation: Gravity") in listed, listed
# The pinned bone property's row in the bone layer's own rows carries its pin too.
assert any(name == "Angle" and pinned for depth, name, red, pinned in rows()), rows()
# Undo takes a pin back.
assert oanim.ops.ed.undo() == {'FINISHED'}
assert not oanim.data.simulation.channel_pinned("gravity")
assert oanim.ops.ed.redo() == {'FINISHED'}
assert oanim.data.simulation.channel_pinned("gravity")
# Unpinning (the pin's click) takes the row away.
assert art.pin_channel("rotation", False) == {'FINISHED'}
assert (1, art.name + ": Rotation") not in names()

# --- Saved with the file -------------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_pins_")) / "pins.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
art = next(l for l in layers if l.type != "BONE")
rig = next(l for l in layers if l.type == "BONE")
assert art.channel_pinned("opacity") and rig.channel_pinned('bones["arm"].angle')
assert oanim.data.simulation.channel_pinned("gravity") and not art.channel_pinned("rotation")

# --- Clear Pins ----------------------------------------------------------------------------------
assert oanim.ops.anim.pins_clear() == {'FINISHED'}
assert not any(name == "Pinned" for depth, name in names())
assert oanim.ops.anim.pins_clear() == {'CANCELLED'}, "nothing left to clear"

print("Timeline pin tests passed")
