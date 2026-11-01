"""A mask card read as a shape built from nothing, then shown or hidden: nothing saved changes, but the steps read in the
order you think of them.

- a card's steps read +, - or Keep Only whichever way the card is saved, on a black card and a white one;
- Result: flipping a card swaps what shows, exactly, its steps reading the same;
- the mouth hole: head minus ((inside mouth minus side cut) and lip line), set in that order, in one undo step per
  change, gives the same card as the one written by hand;
- setting a lone step's verb leaves the card's Result alone;
- the summary says a shape hidden whole;
- a driver on Start With or a step's mode stops the swap;
- it saves as it always did.

Run with:  OpenAnimate --background --python tests/test_mask_show_hide.py
"""
import json
import pathlib
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_mask_show_hide_"))


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
    """Whether the art shows at (x, y): masks hide what they hide."""
    layers.active = find("Layer 1")
    if ops.layer.select_at(x=str(x), y=str(y)) != {'FINISHED'}:
        return False
    return layers.active.name == "head"


# The inside of the mouth, a cut taken off its right, and the lip line across its middle.
inside = square("inside", -1.0, -1.0, 1.0, 1.0)
cut = square("cut", 0.5, -1.0, 1.0, 1.0)
lip = square("lip", -1.2, -0.2, 1.2, 0.2)
head = square("head", -3.0, -3.0, 3.0, 3.0, hidden=False)
layers.active = head

SPOTS = {
    "hole": (0.0, 0.0),          # inside, within the lip: gone
    "past the lip": (0.0, 0.9),  # inside, past the lip: stays
    "side cut": (0.75, 0.0),     # taken back by the cut: stays
    "outside": (1.5, 0.0),       # not inside at all: stays
    "far": (-2.5, 2.5),
}


def seen():
    result = {name: picks(*at) for name, at in SPOTS.items()}
    layers.active = head
    return result


WANT = {"hole": False, "past the lip": True, "side cut": True, "outside": True, "far": True}

# --- Built in the order you say it, on the card that shows, then Hide -------------------------------------------------
assert ops.mask.add(sources="inside", mode="ADD") == {'FINISHED'}
card = head.masks[0]
assert card["result"] == "SHOW" and card["steps"][0]["verb"] == "ADD", card
assert ops.mask.add(card="0", sources="cut", verb="REMOVE") == {'FINISHED'}
assert ops.mask.add(card="0", sources="lip", verb="KEEP_ONLY") == {'FINISHED'}
card = head.masks[0]
assert [s["verb"] for s in card["steps"]] == ["ADD", "REMOVE", "KEEP_ONLY"], card["steps"]
assert card["steps"][2]["mode"] == "SUBTRACT" and card["steps"][2]["invert"], "Keep Only on a black card"
shown = seen()
assert shown == {k: not v for k, v in WANT.items()}, ("shows only inside the shape", shown)

assert ops.mask.set(index="0", result="HIDE") == {'FINISHED'}
# Undo takes the swap back whole.
ops.ed.undo()
assert head.masks[0]["result"] == "SHOW" and [s["mode"] for s in head.masks[0]["steps"]] == ["ADD", "SUBTRACT", "SUBTRACT"]
ops.ed.redo()
card = head.masks[0]
assert card["result"] == "HIDE" and card["start"] == "EVERYTHING"
assert [s["verb"] for s in card["steps"]] == ["ADD", "REMOVE", "KEEP_ONLY"], ("the steps read the same", card["steps"])
assert seen() == WANT, seen()

# What it is saved as: the card written by hand, which is why nothing else had to change.
assert [(s["mode"], s["invert"]) for s in head.masks[0]["steps"]] == [("SUBTRACT", False), ("ADD", False), ("ADD", True)]

# The summary reads as what it is.
assert head.mask_summary == "Shows: everything but (inside, minus cut, within lip)", head.mask_summary

# Asking for what it already is changes nothing.
before = [dict(s) for s in head.masks[0]["steps"]]
assert ops.mask.set(index="0", result="HIDE") == {'FINISHED'}
assert [s["mode"] for s in head.masks[0]["steps"]] == [s["mode"] for s in before]

# --- A card from before reads the same way, saved untouched ----------------------------------------------------------
assert ops.mask.remove(index="0") == {'FINISHED'}
assert ops.mask.add(sources="inside", mode="SUBTRACT") == {'FINISHED'}
assert ops.mask.add(card="0", sources="cut", mode="ADD") == {'FINISHED'}
assert ops.mask.add(card="0", sources="lip", mode="ADD", invert="1") == {'FINISHED'}
card = head.masks[0]
assert card["result"] == "HIDE"
assert [s["verb"] for s in card["steps"]] == ["ADD", "REMOVE", "KEEP_ONLY"], card["steps"]
assert seen() == WANT, seen()

# Every verb on either card, set and read back.
for result in ("HIDE", "SHOW"):
    assert ops.mask.set(index="0", result=result) == {'FINISHED'}
    for step in (0, 1, 2):
        for verb in ("KEEP_ONLY", "REMOVE", "ADD"):
            assert ops.mask.set(index="0", step=str(step), verb=verb) == {'FINISHED'}
            assert head.masks[0]["steps"][step]["verb"] == verb
    assert ops.mask.set(index="0", step="1", verb="REMOVE") == {'FINISHED'}
    assert ops.mask.set(index="0", step="2", verb="KEEP_ONLY") == {'FINISHED'}

# A + step of the outside is still there, under Invert.
assert ops.mask.set(index="0", step="0", invert="1") == {'FINISHED'}
assert head.masks[0]["steps"][0]["verb"] == "ADD", head.masks[0]["steps"][0]
assert ops.mask.set(index="0", step="0", invert="0") == {'FINISHED'}

# --- A lone step ------------------------------------------------------------------------------------------------------
assert ops.mask.remove(index="0") == {'FINISHED'}
assert ops.mask.add(sources="inside", mode="SUBTRACT") == {'FINISHED'}
assert head.masks[0]["result"] == "HIDE"
assert head.mask_summary == "Shows: everything but inside", head.mask_summary
assert ops.mask.set(index="0", verb="REMOVE") == {'FINISHED'}
assert head.masks[0]["result"] == "HIDE", "a step's verb leaves the card's Result alone"
assert ops.mask.set(index="0", verb="ADD") == {'FINISHED'}
# As it was: mode= on a lone step still sets Start With, for scripts.
assert ops.mask.set(index="0", mode="ADD") == {'FINISHED'}
assert head.masks[0]["start"] == "NOTHING"

# --- Refused ----------------------------------------------------------------------------------------------------------
assert ops.mask.set(index="0", verb="NOPE") == {'CANCELLED'}
assert ops.mask.set(index="0", result="NOPE") == {'CANCELLED'}

# --- A driver on what the swap would change stops it ----------------------------------------------------------------
assert ops.mask.remove(index="0") == {'FINISHED'}
assert ops.mask.add(sources="inside", mode="SUBTRACT") == {'FINISHED'}
assert ops.anim.driver_add(layer=str(head.id), path="masks[0].start") == {'FINISHED'}
assert ops.mask.set(index="0", result="SHOW") == {'CANCELLED'}
assert head.masks[0]["result"] == "HIDE"

# --- Saved as it always was: no new keys ---------------------------------------------------------------------------
path = folder / "show_hide.oanim"
assert ops.wm.save_as_mainfile(filepath=str(path)) == {'FINISHED'}
with zipfile.ZipFile(path) as z:
    text = next(z.read(n).decode("utf-8") for n in z.namelist() if n.endswith(".json"))
assert "verb" not in text and "\"result\"" not in text, "nothing new in the file"

print("Mask show and hide tests passed")
