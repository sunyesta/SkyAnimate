"""Masks and masking areas: a mask's areas are a stack on Show All or Hide All, each painting white (+), black (-) or
keeping only what's inside it, and a group's areas build a shape of their own for it to paint.

- the mouth hole, as it's said: Show All, then - a group of + inside, - side cut, Keep Only lip line;
- a group works directly on the base whatever is in it; higher up only if it makes one shape, and otherwise does nothing
  rather than guess;
- a group's areas are moved, removed and changed like any;
- Inverse takes the outside of any area or group, turning a - area into Keep Only;
- a global area, as a shared material: its layers and settings are shared, each use keeping its own + or -; it saves,
  and unlinks looking the same;
- groups survive saving and copying, and a copy that changes a global area's layers is its own from then on.

Run with:  OpenAnimate --background --python tests/test_mask_areas.py
"""
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_mask_areas_"))


def find(name):
    return next(l for l in layers.all() if l.name == name)


def square(name, x0, y0, x1, y1, hidden=True):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    layer.add_curve([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], closed=True, fill=True)
    layer.visible = not hidden
    return layer


def picks(x, y):
    """Whether the head shows at (x, y): masks hide what they hide."""
    layers.active = find("Layer 1")
    if ops.layer.select_at(x=str(x), y=str(y)) != {'FINISHED'}:
        return False
    return layers.active.name == "head"


inside = square("inside", -1.0, -1.0, 1.0, 1.0)
cut = square("cut", 0.5, -1.0, 1.0, 1.0)
lip = square("lip", -1.8, -0.2, 1.8, 0.2)
patch = square("patch", -2.8, 2.0, -2.2, 2.6)
head = square("head", -3.0, -3.0, 3.0, 3.0, hidden=False)
layers.active = head

SPOTS = {
    "hole": (0.0, 0.0),          # inside, within the lip: gone
    "past the lip": (0.0, 0.9),  # inside, past the lip: stays
    "side cut": (0.75, 0.0),     # taken back by the cut: stays
    "outside": (1.5, 0.0),       # not inside at all: stays
    "far": (-2.5, 2.5),          # in the patch
}
HOLE = {"hole": False, "past the lip": True, "side cut": True, "outside": True, "far": True}


def seen():
    result = {name: picks(*at) for name, at in SPOTS.items()}
    layers.active = head
    return result


def clear():
    while head.masks:
        assert ops.mask.remove(index="0") == {'FINISHED'}


def mouth_group(verb, start):
    """A card on `start`, its one area a group made of the mouth."""
    clear()
    assert ops.mask.add(sources="patch", mode="ADD") == {'FINISHED'}
    assert ops.mask.set(index="0", start=start) == {'FINISHED'}
    assert ops.mask.group(index="0", name="Mouth hole", verb=verb) == {'FINISHED'}
    assert ops.mask.add(card="0", group="1", sources="inside", verb="ADD") == {'FINISHED'}
    assert ops.mask.add(card="0", group="1", sources="cut", verb="REMOVE") == {'FINISHED'}
    assert ops.mask.add(card="0", group="1", sources="lip", verb="KEEP_ONLY") == {'FINISHED'}
    assert ops.mask.remove(index="0", step="0") == {'FINISHED'}


# --- The mouth hole, as it's said -------------------------------------------------------------------------------------
mouth_group("REMOVE", "EVERYTHING")
card = head.masks[0]
assert len(card["steps"]) == 1 and card["steps"][0]["group"] and card["steps"][0]["name"] == "Mouth hole"
assert [s["verb"] for s in card["steps"][0]["children"]] == ["ADD", "REMOVE", "KEEP_ONLY"]
assert seen() == HOLE, seen()
assert head.mask_summary == "Shows: everything but (inside, minus cut, within lip)", head.mask_summary

# The same shape shown, on black, and kept, on white, is the opposite.
mouth_group("ADD", "NOTHING")
assert seen() == {k: not v for k, v in HOLE.items()}, seen()
mouth_group("KEEP_ONLY", "EVERYTHING")
assert seen() == {k: not v for k, v in HOLE.items()}, seen()

# On the base it would do nothing: + on white and - on black.
mouth_group("ADD", "EVERYTHING")
assert all(seen().values()), seen()
mouth_group("REMOVE", "NOTHING")
assert not any(seen().values()), seen()

# --- Plain areas paint in order, bottom first ------------------------------------------------------------------------
clear()
assert ops.mask.add(sources="inside", mode="SUBTRACT") == {'FINISHED'}
assert head.masks[0]["start"] == "EVERYTHING", "the first area picks the base"
assert ops.mask.add(card="0", sources="cut", verb="ADD") == {'FINISHED'}
assert [s["verb"] for s in head.masks[0]["steps"]] == ["REMOVE", "ADD"]
assert ops.mask.set(index="0", step="1", verb="KEEP_ONLY") == {'FINISHED'}
assert head.masks[0]["steps"][1]["mode"] == "SUBTRACT" and head.masks[0]["steps"][1]["invert"]
assert ops.mask.set(index="0", step="1", verb="ADD") == {'FINISHED'}
assert not head.masks[0]["steps"][1]["invert"]
# A step's verb leaves the base alone; mode= on a lone step still sets it, for scripts.
assert ops.mask.remove(index="0", step="1") == {'FINISHED'}
assert ops.mask.set(index="0", verb="ADD") == {'FINISHED'}
assert head.masks[0]["start"] == "EVERYTHING"
assert ops.mask.set(index="0", verb="NOPE") == {'CANCELLED'}

# --- A group higher up, where it makes one shape ---------------------------------------------------------------------


def above_patch(verb, *children):
    """Hide All, + inside, then a group of `children` (layer, verb)."""
    clear()
    assert ops.mask.add(sources="inside", mode="ADD") == {'FINISHED'}
    assert ops.mask.group(index="0", verb=verb) == {'FINISHED'}
    for name, child in children:
        assert ops.mask.add(card="0", group="1", sources=name, verb=child) == {'FINISHED'}


above_patch("ADD", ("patch", "ADD"), ("lip", "ADD"))
shown = seen()
assert shown["far"] and shown["hole"] and shown["past the lip"], ("a group of + adds them all", shown)
above_patch("REMOVE", ("cut", "ADD"), ("lip", "ADD"))
shown = seen()
assert not shown["hole"] and not shown["side cut"] and shown["past the lip"], ("a group of + taken away", shown)
above_patch("REMOVE", ("lip", "ADD"), ("cut", "REMOVE"))
shown = seen()
assert not shown["hole"] and shown["side cut"] and shown["past the lip"], ("lip minus cut, taken away", shown)
above_patch("KEEP_ONLY", ("inside", "ADD"), ("lip", "KEEP_ONLY"))
shown = seen()
assert shown["hole"] and not shown["past the lip"], ("kept to where they overlap", shown)
# Three kinds at once, up here: the group works out its shape and paints it, whatever it holds.
above_patch("REMOVE", ("lip", "ADD"), ("cut", "REMOVE"), ("inside", "KEEP_ONLY"))
shown = seen()
assert not shown["hole"] and shown["side cut"] and shown["past the lip"] and not shown["outside"], ("taken away", shown)
assert head.masks[0]["steps"][1]["problem"] == ""
assert head.mask_summary == "Shows: inside inside, minus (lip, minus cut, within inside)", head.mask_summary
above_patch("ADD", ("lip", "ADD"), ("cut", "REMOVE"), ("patch", "ADD"))
shown = seen()
assert shown["far"] and shown["outside"] and shown["hole"], ("added", shown)
above_patch("KEEP_ONLY", ("lip", "ADD"), ("cut", "REMOVE"), ("patch", "ADD"))
shown = seen()
assert shown["hole"] and not shown["past the lip"] and not shown["side cut"], ("kept", shown)
# Inverted paint inside it, and the same on the lines' side of things.
above_patch("REMOVE", ("lip", "ADD"), ("cut", "KEEP_ONLY"), ("patch", "ADD"))
shown = seen()
assert shown["hole"] and not shown["side cut"], ("only what's in the cut, and the patch", shown)
assert ops.mask.set(index="0", end_lines="1") == {'FINISHED'}
assert head.line_shows(*SPOTS["hole"]) and not head.line_shows(*SPOTS["side cut"]), "End Lines reads the group too"
# Show All and Hide All are areas: in a group, a Show All is white to cut from.
above_patch("REMOVE", ("cut", "REMOVE"))
assert ops.mask.add(card="0", group="1", whole="SHOW") == {'FINISHED'}
group = head.masks[0]["steps"][1]
assert group["children"][0]["whole"] and group["children"][0]["verb"] == "ADD", ("it goes in at the bottom", group["children"])
assert group["problem"] == ""
shown = seen()
assert not shown["hole"] and shown["side cut"] and not shown["outside"], ("all but the cut, taken from the inside", shown)
assert head.mask_summary == "Shows: inside inside, minus (everything, minus cut)", head.mask_summary
assert ops.mask.set(index="0", step="1", child="0", verb="KEEP_ONLY") == {'FINISHED'}
assert head.masks[0]["steps"][1]["children"][0]["verb"] == "ADD", "there's no keeping only the whole canvas"
assert ops.mask.set(index="0", step="1", child="0", verb="REMOVE") == {'FINISHED'}
assert "needs a + area" in head.masks[0]["steps"][1]["problem"]
# In the stack too: painting over what's below it, and the file keeps it.
clear()
assert ops.mask.add(sources="inside", mode="ADD") == {'FINISHED'}
assert ops.mask.add(card="0", whole="HIDE") == {'FINISHED'}
assert head.masks[0]["steps"][0]["whole"], "at the bottom of the stack"
assert ops.mask.area_move(index="0", step="0", to="2") == {'FINISHED'}
assert not any(seen().values()), seen()
assert ops.mask.set(index="0", step="1", verb="ADD") == {'FINISHED'}
assert all(seen().values()), seen()
assert head.line_shows(*SPOTS["hole"])
assert ops.mask.add(sources="inside", whole="NOPE") == {'CANCELLED'}
save_path = folder / "whole.oanim"
assert ops.wm.save_as_mainfile(filepath=str(save_path)) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=str(save_path)) == {'FINISHED'}
head = find("head")
assert head.masks[0]["steps"][1]["whole"]
layers.active = head

# Export paints a group's shape as a mask of its own.
above_patch("REMOVE", ("lip", "ADD"), ("cut", "REMOVE"), ("inside", "KEEP_ONLY"))
svg = folder / "group.svg"
assert ops.export.svg(filepath=str(svg)) == {'FINISHED'}
assert svg.read_text(encoding="utf-8").count("<mask") >= 5, "the card, the group, and each of its areas"

# Nothing + in it: it builds nothing, and says so.
above_patch("REMOVE", ("cut", "REMOVE"))
assert "needs a + area" in head.masks[0]["steps"][1]["problem"], head.masks[0]["steps"][1]["problem"]
shown = seen()
assert shown["hole"] and shown["side cut"], ("an empty group paints nothing", shown)

# --- Inverse: the outside of any area -----------------------------------------------------------------------------------
# On a - area it hides what's outside, which is Keep Only, and off again it's a - area.
clear()
assert ops.mask.add(sources="inside", mode="SUBTRACT") == {'FINISHED'}
shown = seen()
assert not shown["hole"] and shown["outside"], shown
assert ops.mask.set(index="0", step="0", invert="1") == {'FINISHED'}
assert head.masks[0]["steps"][0]["verb"] == "KEEP_ONLY"
shown = seen()
assert shown["hole"] and not shown["outside"], shown
assert ops.mask.set(index="0", step="0", invert="0") == {'FINISHED'}
assert head.masks[0]["steps"][0]["verb"] == "REMOVE"
# On a + area it shows everything but the shape.
clear()
assert ops.mask.add(sources="inside", mode="ADD") == {'FINISHED'}
assert ops.mask.set(index="0", step="0", invert="1") == {'FINISHED'}
assert head.masks[0]["steps"][0]["verb"] == "ADD"
shown = seen()
assert not shown["hole"] and shown["outside"], shown
# On a group, whatever is in it. Directly on the base, shown on black, it's the shape's opposite.
mouth_group("ADD", "NOTHING")
assert ops.mask.set(index="0", step="0", invert="1") == {'FINISHED'}
assert head.masks[0]["steps"][0]["group"] and head.masks[0]["steps"][0]["verb"] == "ADD"
assert seen() == HOLE, seen()
assert head.mask_summary == "Shows: outside (inside, minus cut, within lip)", head.mask_summary
# Taken from white it becomes Keep Only.
mouth_group("REMOVE", "EVERYTHING")
assert ops.mask.set(index="0", step="0", invert="1") == {'FINISHED'}
assert head.masks[0]["steps"][0]["verb"] == "KEEP_ONLY"
assert seen() == {k: not v for k, v in HOLE.items()}, seen()
# Higher up: Hide All, + inside, then all but a group of two + areas, where the plain group would add them.
above_patch("ADD", ("lip", "ADD"), ("cut", "ADD"))
assert ops.mask.set(index="0", step="1", invert="1") == {'FINISHED'}
shown = seen()
assert shown["hole"] and shown["far"] and not shown["outside"], shown
# And one that makes one shape another way: all but lip minus cut.
above_patch("ADD", ("lip", "ADD"), ("cut", "REMOVE"))
assert ops.mask.set(index="0", step="1", invert="1") == {'FINISHED'}
shown = seen()
assert shown["hole"] and shown["far"] and not shown["outside"], shown
# And one of three kinds, that stays a group.
above_patch("ADD", ("inside", "ADD"), ("cut", "REMOVE"), ("lip", "KEEP_ONLY"))
assert ops.mask.set(index="0", step="1", invert="1") == {'FINISHED'}
shown = seen()
assert shown["hole"] and shown["far"] and shown["outside"], shown

# --- Areas in a group change, move and go ----------------------------------------------------------------------------
mouth_group("REMOVE", "EVERYTHING")
assert ops.mask.set(index="0", step="0", child="1", grow="2") == {'FINISHED'}
assert head.masks[0]["steps"][0]["children"][1]["grow"] == 2.0
assert ops.mask.set(index="0", step="0", child="1", grow="0") == {'FINISHED'}
assert ops.mask.remove(index="0", step="0", child="1") == {'FINISHED'}
assert [c["layers"] for c in head.masks[0]["steps"][0]["children"]] == [["inside"], ["lip"]]
assert not seen()["hole"]
# Convert To and Delete, as a row's right-click menu calls them: a group's area, and a group whole.
assert ops.mask.set(index="0", step="0", child="1", verb="REMOVE") == {'FINISHED'}
assert head.masks[0]["steps"][0]["children"][1]["verb"] == "REMOVE"
assert ops.mask.set(index="0", step="0", child="1", verb="KEEP_ONLY") == {'FINISHED'}
assert ops.mask.set(index="0", step="0", verb="ADD") == {'FINISHED'}
assert head.masks[0]["steps"][0]["verb"] == "ADD"
assert ops.mask.set(index="0", step="0", verb="REMOVE") == {'FINISHED'}
assert ops.mask.add(card="0", sources="patch", verb="ADD") == {'FINISHED'}
assert ops.mask.remove(index="0", step="0") == {'FINISHED'}
assert [s["layers"] for s in head.masks[0]["steps"]] == [["patch"]], "a group goes with what's in it"
# The last area takes its mask with it.
assert ops.mask.remove(index="0", step="0") == {'FINISHED'}
assert not head.masks
mouth_group("REMOVE", "EVERYTHING")
assert ops.mask.remove(index="0", step="0", child="1") == {'FINISHED'}
# Out of the group on top of the stack, and back in.
assert ops.mask.area_move(index="0", step="0", child="1", to="1") == {'FINISHED'}
assert [len(s["children"]) for s in head.masks[0]["steps"]] == [1, 0], head.masks[0]["steps"]
assert ops.mask.area_move(index="0", step="1", group="0", to="1") == {'FINISHED'}
assert len(head.masks[0]["steps"]) == 1 and len(head.masks[0]["steps"][0]["children"]) == 2
# A group doesn't go inside a group.
assert ops.mask.group(index="0") == {'FINISHED'}
assert ops.mask.area_move(index="0", step="1", group="0", to="0") == {'CANCELLED'}
# Order in the stack.
assert ops.mask.add(card="0", sources="patch", verb="ADD") == {'FINISHED'}
assert ops.mask.area_move(index="0", step="2", to="0") == {'FINISHED'}
assert head.masks[0]["steps"][0]["layers"] == ["patch"]

# --- Several at once, as the Layers editor's picked rows ---------------------------------------------------------------
ROW = lambda step, child=-1: (step << 8) | (child + 1)


def rows(*at):
    return "|".join(str(ROW(*a)) for a in at)


def plain(*verbs_and_layers):
    clear()
    first = True
    for layer_name, verb in verbs_and_layers:
        if first:
            assert ops.mask.add(sources=layer_name, mode="ADD" if verb == "ADD" else "SUBTRACT") == {'FINISHED'}
            first = False
        else:
            assert ops.mask.add(card="0", sources=layer_name, verb=verb) == {'FINISHED'}


# All - stays a -, painting as it did.
plain(("inside", "REMOVE"), ("cut", "REMOVE"), ("patch", "REMOVE"))
before = seen()
assert ops.mask.areas_group(index="0", rows=rows((0,), (1,))) == {'FINISHED'}
steps = head.masks[0]["steps"]
assert steps[0]["group"] and steps[0]["verb"] == "REMOVE" and [c["verb"] for c in steps[0]["children"]] == ["ADD", "ADD"]
assert steps[1]["layers"] == ["patch"]
assert seen() == before, ("the same paint", seen(), before)

# All + stays a +, where the lowest was.
plain(("patch", "REMOVE"), ("inside", "ADD"), ("lip", "ADD"))
before = seen()
assert ops.mask.areas_group(index="0", rows=rows((1,), (2,))) == {'FINISHED'}
assert head.masks[0]["steps"][1]["group"] and head.masks[0]["steps"][1]["verb"] == "ADD"
assert seen() == before, seen()

# A mix keeps each one's own, the group taking its paint from the base: that's the mouth hole.
plain(("inside", "ADD"), ("cut", "REMOVE"), ("lip", "KEEP_ONLY"))
assert ops.mask.set(index="0", start="EVERYTHING") == {'FINISHED'}
assert ops.mask.areas_group(index="0", rows=rows((0,), (1,), (2,))) == {'FINISHED'}
steps = head.masks[0]["steps"]
assert len(steps) == 1 and steps[0]["verb"] == "REMOVE"
assert [c["verb"] for c in steps[0]["children"]] == ["ADD", "REMOVE", "KEEP_ONLY"]
assert seen() == HOLE, seen()

# It's one undo step, and groups don't go inside groups.

assert ops.mask.add(card="0", sources="patch", verb="ADD") == {'FINISHED'}
assert ops.mask.areas_group(index="0", rows=rows((0,), (1,))) == {'CANCELLED'}
assert ops.mask.areas_group(index="0", rows=rows((0, 1), (1,))) == {'CANCELLED'}

# Convert and delete take them all; deleting them all takes the mask.
plain(("inside", "ADD"), ("cut", "ADD"), ("lip", "ADD"), ("patch", "ADD"))
assert ops.mask.areas_set(index="0", rows=rows((1,), (2,)), verb="REMOVE") == {'FINISHED'}
assert [s["verb"] for s in head.masks[0]["steps"]] == ["ADD", "REMOVE", "REMOVE", "ADD"]
assert ops.mask.areas_delete(index="0", rows=rows((1,), (3,))) == {'FINISHED'}
assert [s["layers"] for s in head.masks[0]["steps"]] == [["inside"], ["lip"]]
assert ops.mask.areas_delete(index="0", rows=rows((0,), (1,))) == {'FINISHED'}
assert not head.masks

# Moved together, keeping their order.
plain(("inside", "ADD"), ("cut", "ADD"), ("lip", "ADD"), ("patch", "ADD"))
assert ops.mask.area_move(index="0", rows=rows((0,), (2,)), to="4") == {'FINISHED'}
assert [s["layers"][0] for s in head.masks[0]["steps"]] == ["cut", "patch", "inside", "lip"]
assert ops.mask.group(index="0") == {'FINISHED'}
assert ops.mask.area_move(index="0", rows=rows((1,), (3,)), group="4", to="0") == {'FINISHED'}
assert [c["layers"][0] for c in head.masks[0]["steps"][2]["children"]] == ["patch", "lip"]
assert [s["layers"][0] for s in head.masks[0]["steps"][:2]] == ["cut", "inside"]

# --- Global areas -----------------------------------------------------------------------------------------------------
clear()
assert ops.mask.add(sources="inside", mode="SUBTRACT") == {'FINISHED'}
assert ops.mask.area_global(index="0", step="0", action="MAKE", name="Mouth") == {'FINISHED'}
step = head.masks[0]["steps"][0]
assert step["global"] != 0 and step["name"] == "Mouth" and step["users"] == 1
global_id = step["global"]

other = square("other", -3.0, -3.0, 3.0, 3.0, hidden=False)
layers.active = other
assert ops.mask.add(sources="patch", mode="ADD") == {'FINISHED'}
assert ops.mask.area_global(index="0", step="0", action="LINK", **{"global": str(global_id)}) == {'FINISHED'}
linked = other.masks[0]["steps"][0]
assert linked["layers"] == ["inside"] and linked["verb"] == "ADD", ("it keeps its own +", linked)
assert linked["users"] == 2 and head.masks[0]["steps"][0]["users"] == 2

# Changing it from either changes both; paint stays each one's.
assert ops.mask.set(index="0", step="0", grow="3", sources="cut") == {'FINISHED'}
assert head.masks[0]["steps"][0]["grow"] == 3.0 and head.masks[0]["steps"][0]["layers"] == ["cut"]
assert head.masks[0]["steps"][0]["verb"] == "REMOVE"
assert other.masks[0]["steps"][0]["verb"] == "ADD"
assert ops.mask.set(index="0", step="0", area_name="Mouth area") == {'FINISHED'}
assert head.masks[0]["steps"][0]["name"] == "Mouth area"
assert ops.mask.area_global(index="0", step="0", action="LINK", **{"global": "999"}) == {'CANCELLED'}

# Making another keeps names apart.
layers.active = head
assert ops.mask.add(card="0", sources="lip", verb="ADD") == {'FINISHED'}
assert ops.mask.area_global(index="0", step="1", action="MAKE", name="Mouth area") == {'FINISHED'}
assert head.masks[0]["steps"][1]["name"] == "Mouth area.001", head.masks[0]["steps"][1]["name"]

# It saves.
path = folder / "areas.oanim"
assert ops.wm.save_as_mainfile(filepath=str(path)) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=str(path)) == {'FINISHED'}
head, other = find("head"), find("other")
assert head.masks[0]["steps"][0]["global"] == global_id and other.masks[0]["steps"][0]["users"] == 2
layers.active = other
assert ops.mask.set(index="0", step="0", grow="5") == {'FINISHED'}
assert head.masks[0]["steps"][0]["grow"] == 5.0, "linked after opening"

# Unlinked it looks the same, and is its own.
assert ops.mask.area_global(index="0", step="0", action="UNLINK") == {'FINISHED'}
assert other.masks[0]["steps"][0]["global"] == 0 and other.masks[0]["steps"][0]["grow"] == 5.0
assert head.masks[0]["steps"][0]["users"] == 1
assert ops.mask.set(index="0", step="0", grow="1") == {'FINISHED'}
assert head.masks[0]["steps"][0]["grow"] == 5.0

# --- Groups save and copy ---------------------------------------------------------------------------------------------
layers.active = head
mouth_group("REMOVE", "EVERYTHING")
assert ops.mask.area_global(index="0", step="0", action="MAKE") == {'FINISHED'}
assert ops.wm.save_as_mainfile(filepath=str(path)) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=str(path)) == {'FINISHED'}
head = find("head")
find("other").visible = False  # it would be picked over the head where it shows
layers.active = head
assert seen() == HOLE, seen()
group = head.masks[0]["steps"][0]
assert group["group"] and [c["verb"] for c in group["children"]] == ["ADD", "REMOVE", "KEEP_ONLY"] and group["global"]

# A plain duplicate stays linked, masks naming the same layers.
assert ops.layer.duplicate() == {'FINISHED'}
assert head.masks[0]["steps"][0]["users"] == 2, head.masks[0]["steps"][0]["users"]

print("Mask area tests passed")
