"""The timeline's channel rows, one per property, in four groups: Pinned (properties pinned to it,
whatever is selected, even with no keys left), Props (the settings: layer transform, opacity,
visibility, constraint settings, Simulation), All (the motion: bones and points) and Selected;
inside them grouped by owner (the default) or by kind.

Run with:  OpenAnimate --background --python tests/test_timeline_pins.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm


def rows(group_by="OWNER"):
    return wm.timeline_rows(group_by)


def names(group_by="OWNER"):
    return [(depth, name) for depth, name, red, pinned in rows(group_by)]


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


# --- Group by Owner: Props holds the layer's own transform; All holds the bones' motion ------------
owner = names()
tops = [name for depth, name in owner if depth == 0]
assert tops == ["Props", "All", "Selected"], tops
props, motion = section(owner, "Props"), section(owner, "All")
for row in [(1, rig.name), (2, "Transform"), (3, "Position"), (3, "Rotation"), (3, "Scale")]:
    assert row in props, (row, props)
assert not any(name in ("Bones", "arm", "Angle") for depth, name in props), "no bone motion in Props"
for row in [(1, rig.name), (2, "Bones"), (3, "arm"), (4, "Angle"), (4, "Transform"), (4, "Scale")]:
    assert row in motion, (row, motion)
assert not any(depth == 2 and name == "Transform" for depth, name in motion), "the layer's own transform is a Prop"
assert motion.index((3, "arm")) < motion.index((4, "Angle")), "a bone's rows sit under it"
assert (1, "arm") in section(owner, "Selected"), "the selected bone's motion"

# --- Group by Kind -------------------------------------------------------------------------------
kind = names("KIND")
props, motion = section(kind, "Props"), section(kind, "All")
for row in [(1, "Layers"), (2, rig.name), (3, "Angle & Transform"), (4, "Rotation")]:
    assert row in props, (row, props)
for row in [(1, "Bones"), (2, "Bone Angle"), (3, "arm: Angle"), (2, "Bone Transform"), (3, "arm: Transform"),
            (3, "arm: Scale")]:
    assert row in motion, (row, motion)

# Constraints' keyed settings get their own rows, by constraint and setting.
rig.select_bone("arm")
assert oanim.ops.armature.constraint_add(type="DYNAMICS", bone="arm") == {'FINISHED'}
bounce = 'bones["arm"].constraints[0].angle_spring.bounce'
assert oanim.ops.anim.keyframe_toggle(layer=str(rig.id), paths=bounce) == {'FINISHED'}
assert (1, "Constraints") in section(names("KIND"), "Props")
assert (2, "arm: Dynamics: Angle Bounce") in section(names("KIND"), "Props"), names("KIND")
props = section(names(), "Props")
assert (2, "Bones") in props and (3, "arm") in props and (4, "Dynamics: Angle Bounce") in props, props
assert (4, "Dynamics: Angle Bounce") not in section(names(), "All"), "a setting, not motion"

# --- Pinning: the drawing's opacity, while the bone layer is the one picked -----------------------
assert not art.channel_pinned("opacity")
assert art.pin_channel("opacity") == {'FINISHED'} and art.channel_pinned("opacity")
pinned = rows()
assert pinned[0][:3] == (0, "Pinned", True), pinned[0]
assert pinned[1] == (1, art.name + ": Opacity", True, True), pinned[1]
assert (1, art.name) not in names(), "only the pinned property shows, not its whole layer"
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
