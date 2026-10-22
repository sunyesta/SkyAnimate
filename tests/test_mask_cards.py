"""Mask cards: each card is a mask of its own that starts white (Everything shown) or black (Nothing),
its steps painting on it top to bottom (+ Shape white, - Shape black, Invert the outside), and the
layer shows only where every card that masks that part shows it. A card's first step sets its Start
With (Add: Nothing; Subtract: Everything), so does a lone step's mode; lines ended at an edge end at
the card's whole shape's edge; Clip to Below alone starts from everything; saved with the file; files
from before cards turn their rows into cards that show the same.

Run with:  OpenAnimate --background --python tests/test_mask_cards.py
"""

import json
import pathlib
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_mask_cards_"))


def find(name):
    return next(l for l in layers.all() if l.name == name)


def square(name, x0, x1, hidden=True):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve([(x0, x0), (x1, x0), (x1, x1), (x0, x1)], closed=True, fill=True)
    l.visible = not hidden
    return l


def picks(x, y):
    """The layer Alt-click picks at (x, y): masks hide what they hide."""
    layers.active = find("Layer 1")  # empty: never picked, so a pick always changes something
    if oanim.ops.layer.select_at(x=str(x), y=str(y)) != {'FINISHED'}:
        return None
    return layers.active.name


head = square("head", -1.0, 1.0)
patch = square("patch", 1.5, 2.0)
art = square("art", -3.0, 3.0, hidden=False)
layers.active = art

# A card whose first step is + Shape starts from nothing: art shows only inside head.
assert oanim.ops.mask.add(sources="head", mode="ADD") == {'FINISHED'}
assert art.masks[0]["start"] == "NOTHING"
assert picks(0.0, 0.0) == "art" and picks(2.5, 2.5) is None
assert art.mask_summary == "Shows: inside head", art.mask_summary

# Starting with everything, + Shape paints white on white: all of art shows.
layers.active = art
assert oanim.ops.mask.set(index="0", start="EVERYTHING") == {'FINISHED'}
assert art.masks[0]["start"] == "EVERYTHING"
assert picks(2.5, 2.5) == "art"
assert art.mask_summary == "Shows: everything, plus head", art.mask_summary
layers.active = art
assert oanim.ops.mask.set(index="0", start="GREY") == {'CANCELLED'}

# A lone step's mode is the whole card's: - Shape alone starts from everything.
assert oanim.ops.mask.set(index="0", start="NOTHING") == {'FINISHED'}
assert oanim.ops.mask.set(index="0", mode="SUBTRACT") == {'FINISHED'}
assert art.masks[0]["start"] == "EVERYTHING" and art.mask_summary == "Shows: everything but head", art.mask_summary
assert picks(0.0, 0.0) is None and picks(2.5, 2.5) == "art"

# With more steps, a step's mode leaves Start With alone; Invert paints the outside.
layers.active = art
assert oanim.ops.mask.add(card="0", sources="patch", mode="ADD") == {'FINISHED'}
assert oanim.ops.mask.set(index="0", step="1", mode="SUBTRACT", invert="1") == {'FINISHED'}
assert art.masks[0]["start"] == "EVERYTHING"
assert art.mask_summary == "Shows: everything but head, within patch", art.mask_summary
assert picks(1.75, 1.75) == "art" and picks(2.5, 2.5) is None
layers.active = art
assert oanim.ops.mask.set(index="0", start="NOTHING") == {'FINISHED'}
assert art.mask_summary == "Shows: nothing, minus head, within patch", art.mask_summary
assert picks(1.75, 1.75) is None, "black, painted black: nothing shows"

# Cards multiply: each starts on its own, and art shows where both show it.
oanim.ops.mask.remove(layer="art")
assert oanim.ops.mask.add(layer="art", sources="head", mode="SUBTRACT") == {'FINISHED'}
assert oanim.ops.mask.add(layer="art", sources="patch", mode="ADD") == {'FINISHED'}
assert [m["start"] for m in art.masks] == ["EVERYTHING", "NOTHING"]
assert art.mask_summary == "Shows: everything but head; inside patch", art.mask_summary
assert picks(1.75, 1.75) == "art", "outside head, inside patch"
assert picks(-2.5, -2.5) is None, "outside patch: the second card hides it"
assert picks(0.0, 0.0) is None, "inside head: the first card hides it"
# Off, a card masks nothing.
layers.active = art
assert oanim.ops.mask.set(index="1", enabled="0") == {'FINISHED'}
assert picks(-2.5, -2.5) == "art"

# Lines end at the edge of a card's whole shape: - head, + patch, ending lines; a card that only slices
# lines doesn't end them.
oanim.ops.mask.remove(layer="art")
assert oanim.ops.mask.add(layer="art", sources="head", mode="SUBTRACT", end_lines="1") == {'FINISHED'}
assert oanim.ops.mask.add(layer="art", card="0", sources="patch", mode="ADD") == {'FINISHED'}
assert not art.line_shows(0.0, 0.0), "inside head"
assert art.line_shows(2.5, -2.5), "outside head and patch: the line still shows"
assert art.line_shows(1.75, 1.75), "inside patch"
assert oanim.ops.mask.add(layer="art", sources="patch", mode="SUBTRACT") == {'FINISHED'}
assert art.line_shows(1.75, 1.75), "the slicing card doesn't end lines"

# A card can be named: the summary says it first, and the undo step says so.
layers.active = art
assert oanim.ops.mask.set(index="0", name="Ear join") == {'FINISHED'}
assert art.masks[0]["name"] == "Ear join" and art.masks[1]["name"] == ""
assert art.mask_summary.startswith("Shows: Ear join: everything but head"), art.mask_summary
assert oanim.wm.undo_history()[0][-1] == "Rename Mask: Ear join"

# Clip to Below alone starts from everything (only what's over its base shows).
art.visible = False
base = square("base", 0.0, 1.0, hidden=False)
shade = square("shade", -3.0, 3.0, hidden=False)
layers.active = shade
assert oanim.ops.mask.clip_to_below(layer="shade") == {'FINISHED'}
assert picks(0.5, 0.5) == "shade" and picks(-2.5, -2.5) is None

# Saved with the file.
path = str(folder / "cards.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
art = find("art")
assert [(m["start"], m["end_lines"], len(m["steps"])) for m in art.masks] == [("EVERYTHING", True, 2), ("EVERYTHING", False, 1)]
assert art.masks[0]["name"] == "Ear join", "the name is saved"
with zipfile.ZipFile(path) as archive:
    stored = json.loads(archive.read("document.json"))
ids = {l["name"]: l["id"] for l in stored["document"]["layers"]}


# Files from before cards (as Clay10 was): a layer's rows, the layer's Start With (or none: from the
# first row), Intersect as Subtract of the outside. Rows that mask lines and fills differently become a
# fills card and a lines card, the lines one ending lines if a row did.
def strip(layer_list):
    for l in layer_list:
        if l.get("name") == "art":
            l["masks"] = [{"mode": "SUBTRACT", "layers": [ids["head"]]},
                          {"mode": "ADD", "layers": [ids["patch"]], "cuts": "LINES", "end_lines": True}]
            l["mask_start"] = "EVERYTHING"
        if l.get("name") == "shade":
            l["masks"] = [{"mode": "ADD", "layers": [ids["head"]]},
                          {"mode": "INTERSECT", "invert": False, "layers": [ids["patch"]]}]
        strip(l.get("children", []))


strip(stored["document"]["layers"])
old = folder / "old.oanim"
with zipfile.ZipFile(old, "w") as archive:
    archive.writestr("document.json", json.dumps(stored))
assert oanim.ops.wm.open_mainfile(filepath=str(old), confirmed="1") == {'FINISHED'}
art = find("art")
assert [(m["start"], m["cuts"], m["end_lines"], [(s["mode"], s["layers"]) for s in m["steps"]]) for m in art.masks] == [
    ("EVERYTHING", "FILLS", False, [("SUBTRACT", ["head"])]),
    ("EVERYTHING", "LINES", True, [("SUBTRACT", ["head"]), ("ADD", ["patch"])]),
], art.masks
shade = find("shade")
assert len(shade.masks) == 1 and shade.masks[0]["start"] == "NOTHING", "a first Add row started from nothing"
assert [(s["mode"], s["invert"]) for s in shade.masks[0]["steps"]] == [("ADD", False), ("SUBTRACT", True)], shade.masks

print("mask_cards: ok")
