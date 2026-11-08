"""The timeline's mouse on keys, through the same click and drag as the UI (oanim.wm.timeline_key_press): a click
picks a key, Shift adds, Ctrl takes away, a double-click picks the row's every key; dragging a key moves the
selected keys (Ctrl: Ripple; Alt: just the one channel, not Time Together's linked ones), frames typed set the
move; pressing a selected key among others and letting go picks it alone. The right-click menu: Interpolation, then
what is keyed on the frame in a submenu.

Run with:  OpenAnimate --background --python tests/test_timeline_mouse.py
"""

import oanim

doc = oanim.data.document
layers = oanim.data.layers
wm = oanim.wm
doc.frame_current = 0
doc.auto_key = True

art = layers.active
art.name = "art"
art.add_curve([(0.0, 0.0), (1.0, 0.0)])
for f, x in ((10, 0.5), (20, 1.0)):
    doc.frame_current = f
    art.location_x = x
for f, o in ((5, 0.8), (20, 0.5)):
    doc.frame_current = f
    art.opacity = o
doc.frame_current = 0


def frames(path):
    return [k["frame"] for k in art.keyframes(path)]


def picked(path):
    return [k["frame"] for k in art.keyframes(path) if k["selected"]]


def row(name):
    return next(i for i, r in enumerate(wm.timeline_row_details(True)) if r["name"] == name)


assert frames("position") == [0, 10, 20] and frames("opacity") == [0, 5, 20]
P = "position"

# A click picks the key alone; Shift adds; Ctrl takes away.
assert wm.timeline_key_press(row("Position"), 10)
assert picked(P) == [10] and picked("opacity") == [], (picked(P), picked("opacity"))
assert wm.timeline_key_press(row("Position"), 20, shift=True)
assert picked(P) == [10, 20], picked(P)
assert wm.timeline_key_press(row("Position"), 10, ctrl=True)
assert picked(P) == [20], ("Ctrl-click takes a key out", picked(P))
assert not wm.timeline_key_press(row("Position"), 7), "no key there"

# Pressing a selected key among others and letting go: it alone.
assert wm.timeline_key_press(row("Position"), 10, shift=True)
assert picked(P) == [10, 20]
assert wm.timeline_key_press(row("Position"), 20)
assert picked(P) == [20], ("let go without dragging: just that key", picked(P))

# A double-click picks every key of the row, and of the rows inside it.
assert wm.timeline_key_press(row("Position"), 20, double_click=True)
assert picked(P) == [0, 10, 20] and picked("opacity") == [], picked(P)
assert not wm.timeline_key_press(row("art"), 5, double_click=True), "the layer's row open: a heading, no keys"

# Dragging a key moves the selected keys.
assert wm.timeline_key_press(row("Position"), 10)
assert wm.timeline_key_press(row("Position"), 10, 14)
assert frames(P) == [0, 14, 20], frames(P)
assert wm.undo_history()[0][-1] == "Move Keys"
# Dragging a selected key moves all the selected ones, the selection kept.
assert wm.timeline_key_press(row("Position"), 20, shift=True)
assert wm.timeline_key_press(row("Position"), 14, 16)
assert frames(P) == [0, 16, 22] and picked(P) == [16, 22], (frames(P), picked(P))
oanim.ops.ed.undo()
assert frames(P) == [0, 14, 20]

# Frames typed set the move, whatever the mouse did.
assert wm.timeline_key_press(row("Position"), 14)
assert wm.timeline_key_press(row("Position"), 14, 30, typed="-4")
assert frames(P) == [0, 10, 20], ("typed -4", frames(P))

# Ctrl: Ripple, the keys after go along.
assert wm.timeline_key_press(row("Position"), 10)
assert wm.timeline_key_press(row("Position"), 10, 12, ripple=True)
assert frames(P) == [0, 12, 22], ("Ripple", frames(P))
oanim.ops.ed.undo()
assert frames(P) == [0, 10, 20]

# Time Together: the opacity (keyed on both ends) goes through the same stretch; Alt: just the one.
doc.time_together = True
art.selected = True  # Time Together links the selected layers' channels
assert wm.timeline_key_press(row("Position"), 10)
assert wm.timeline_key_press(row("Position"), 10, 15, alt=True)
assert frames(P) == [0, 15, 20] and frames("opacity") == [0, 5, 20], ("Alt: just the one", frames("opacity"))
oanim.ops.ed.undo()
assert wm.timeline_key_press(row("Position"), 10)
assert wm.timeline_key_press(row("Position"), 10, 15)
assert frames(P) == [0, 15, 20] and frames("opacity") == [0, 8, 20], ("linked: 5 of 10 is 7.5 of 15", frames("opacity"))

# The right-click menu: Interpolation first, then what is keyed on the frame folded into a submenu named for it (no
# Select These Keys: a click on the key already picks them); greyed out where nothing is keyed.
assert wm.timeline_key_menu(row("Position"), 15) == [("Interpolation", True), ("Keyed on frame 15", True)],     wm.timeline_key_menu(row("Position"), 15)
assert wm.timeline_key_menu(row("Position"), 13) == [("Interpolation", True), ("Keyed on frame 13", False)]

print("timeline mouse: ok")
