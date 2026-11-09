"""The timeline's filters (anim.timeline_filter, its Filters menu and search): show Animated (the default:
rows for channels with keys), Changed (keys that don't all hold one value) or All (every channel); kinds
(Transform, Bones, Points, Opacity, Switch, Media, Other); a fuzzy search on names (a folder's name finds all
it holds); a count of the rows they take away (the red "N hidden", which clears them). Saved with the project.

Run with:  OpenAnimate --background --python tests/test_timeline_filters.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True


def names():
    return [r[1] for r in wm.timeline_rows()]


# A bone layer: its position keyed to move, its opacity keyed but holding one value, two bones keyed.
ops.layer.new_bone()
rig = layers.active
rig.name = "rig"
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0")
ops.armature.bone_add(name="leg", head_x="0", head_y="0", tail_x="0", tail_y="-1")
doc.frame_current = 10
rig.location_x = 0.5
rig.bone_set("arm", angle=0.5)
rig.bone_set("leg", angle=0.25)
assert ops.anim.keyframe_insert() == {'FINISHED'}
if not rig.keyframes("opacity"):
    assert ops.anim.keyframe_toggle(layer=str(rig.id), paths="opacity") == {'FINISHED'}
assert len(rig.keyframes("opacity")) == 2, rig.keyframes("opacity")
doc.frame_current = 0
assert ops.armature.select_none() == {'FINISHED'}
doc.timeline_only_selected = False  # every bone (Only Selected lists the picked ones only)
assert "Opacity" in names() and "arm" in names() and "Position" in names(), names()
assert wm.timeline_hidden() == 0

# Kinds: just the bones.
assert ops.anim.timeline_filter(kinds="BONES") == {'FINISHED'}
listed = names()
assert "arm" in listed and "leg" in listed and "Position" not in listed and "Opacity" not in listed, listed
assert "rig" in listed and "Bones" in listed, "the folders holding them stay"
hidden = wm.timeline_hidden()
assert hidden >= 4, hidden  # Position, Rotation, Scale, Opacity at least
# Transform and Opacity.
assert ops.anim.timeline_filter(kinds="TRANSFORM,OPACITY") == {'FINISHED'}
listed = names()
assert "Position" in listed and "Opacity" in listed and "Bones" not in listed and "arm" not in listed, listed
assert ops.anim.timeline_filter(kinds="NOT_A_KIND") == {'CANCELLED'}

# Changed: the opacity's keys hold one value, so it goes; the position changes.
assert ops.anim.timeline_filter(kinds="ALL", show="CHANGED") == {'FINISHED'}
listed = names()
assert "Position" in listed and "Opacity" not in listed and "arm" in listed, listed

# Search: the letters in order, whatever their case; a bone's name finds its rows.
assert ops.anim.timeline_filter(show="ANIMATED", search="lg") == {'FINISHED'}
listed = names()
assert "leg" in listed and "arm" not in listed and "Position" not in listed, listed
leg_at = listed.index("leg")
assert listed[leg_at + 1:leg_at + 2] == ["Angle"] or "Angle" in listed[leg_at:], ("a bone's name finds its rows", listed)
assert ops.anim.timeline_filter(search="OPAC") == {'FINISHED'}
assert "Opacity" in names() and "leg" not in names(), names()
# While searching, folded rows open, so what's found shows.
rows = wm.timeline_row_details()
fold = next(r["fold"] for r in rows if r["name"] == "rig")
wm.set_ui_open("Timeline/" + fold, False)
assert "Opacity" in [r["name"] for r in wm.timeline_row_details(True)]
wm.set_ui_open("Timeline/" + fold, True)

# All: every channel, keys or not (the bone layer's unkeyed ones too).
assert ops.anim.timeline_filter(search="", show="ALL") == {'FINISHED'}
listed = names()
assert "Visibility" in listed and "Draw Order" in listed, ("the rig's unkeyed channels too", listed)
assert len(listed) > 12, listed
# A layer with no keys at all (I keyed every channel of the rig): its channels listed under it all the same.
active = layers.active
ops.layer.new()
still = layers.active
still.name = "still"
assert not still.keyframes("position")
rows = wm.timeline_row_details()
at = next((i for i, r in enumerate(rows) if r["layer"] == still.id), None)
assert at is not None and rows[at]["chain"] == "TOGETHER", ("an unkeyed layer listed with All, chained",
                                                            [r["name"] for r in rows])
still.key_together = False  # open: its channels show
rows = wm.timeline_row_details()
at = next((i for i, r in enumerate(rows) if r["layer"] == still.id), None)
assert at is not None, ("an unkeyed layer listed with All", [r["name"] for r in rows])
under = []
for r in rows[at + 1:]:
    if r["depth"] <= rows[at]["depth"]:
        break
    under.append(r["name"])
assert "Position" in under, ("an unkeyed layer's channels listed with All", under)
ops.layer.delete()
layers.active = active

# Clearing puts them all back (the red "N hidden" does this).
assert ops.anim.timeline_filter(kinds="BONES", show="CHANGED", search="xyz") == {'FINISHED'}
assert wm.timeline_hidden() > 0
assert ops.anim.timeline_filter(clear="1") == {'FINISHED'}
assert "Opacity" in names() and "arm" in names() and wm.timeline_hidden() == 0

# Only Selected through the same operator.
assert ops.anim.timeline_filter(only_selected="0") == {'FINISHED'} and not doc.timeline_only_selected
assert ops.anim.timeline_filter(only_selected="1") == {'FINISHED'} and doc.timeline_only_selected

# Saved with the project.
assert ops.anim.timeline_filter(kinds="BONES,POINTS", show="CHANGED", search="ar") == {'FINISHED'}
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_tl_filters_")) / "filters.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.anim.timeline_filter(clear="1") == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in oanim.data.layers.all() if l.name == "rig")
oanim.data.layers.active = rig
oanim.data.document.timeline_only_selected = False  # every bone, for the search to choose from
listed = names()
assert "arm" in listed and "leg" not in listed and "Position" not in listed, ("kept after opening", listed)

print("timeline filters: ok")
