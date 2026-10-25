"""Onion skins: the drawings being edited as they are on the keys (or frames) before and after this one,
as wireframes in a colour per side; the frames they show on, their wires, and the settings saved.

Run with:  OpenAnimate --background --python tests/test_onion_skins.py
"""

import os
import shutil
import tempfile

import oanim

doc = oanim.data.document
assert not doc.onion_skin, "off by default"
assert doc.onion_mode == "KEYFRAMES" and doc.onion_before == 2 and doc.onion_after == 0

oanim.ops.layer.new()
layer = oanim.data.layers.active
layer.add_curve([(0.0, 0.0), (1.0, 0.0)])

# Keys at 10, 20 and 30 (auto-key), moving the layer along.
for frame, x in ((10, 0.5), (20, 1.0), (30, 1.5)):
    doc.frame_current = frame
    layer.location_x = x
doc.frame_current = 25
assert oanim.wm.onion_frames() == [], "nothing while it is off"


def frames():
    return [(g["frame"], g["after"], g["distance"]) for g in oanim.wm.onion_frames()]


doc.onion_skin = True
doc.onion_after = 1
assert frames() == [(20, False, 1), (10, False, 2), (30, True, 1)], frames()

# On a key, that key isn't a ghost of itself.
doc.frame_current = 20
assert frames() == [(10, False, 1), (30, True, 1)], frames()

# The wires are the drawing as it is on each ghost's frame (document space); the document stays as it was.
wires = {w["frame"]: w for w in oanim.wm.onion_wires()}
assert set(wires) == {10, 30}
start = wires[10]["lines"][0][0]
assert abs(start[0] - 0.5) < 1e-4 and abs(start[1]) < 1e-4, start
assert abs(wires[30]["lines"][0][0][0] - 1.5) < 1e-4
assert abs(wires[10]["lines"][0][-1][0] - 1.5) < 1e-4, "the whole curve"
assert doc.frame_current == 20 and abs(layer.location_x - 1.0) < 1e-6, "the document is left as it was"

# Frames: every onion_step frames, kept in the animation's range.
doc.onion_mode = "FRAMES"
doc.onion_step = 2
doc.frame_current = 25
assert frames() == [(23, False, 1), (21, False, 2), (27, True, 1)], frames()
doc.onion_step = 1
doc.frame_current = 2
assert [f for f, after, _ in frames() if not after] == [1], "frame 0 is the setup, not a ghost"
doc.frame_end = 3
assert [f for f, after, _ in frames() if after] == [3]
doc.frame_end = 250

# Only the layers being edited count, unless All Layers.
doc.onion_mode = "KEYFRAMES"
oanim.ops.layer.new()
other = oanim.data.layers.active
other.add_curve([(0.0, 2.0), (1.0, 2.0)])
doc.frame_current = 15
other.location_x = 0.25
doc.frame_current = 22
oanim.data.layers.active = layer
for l in oanim.data.layers:
    l.select = l.name == layer.name
assert [f for f, after, _ in frames() if not after] == [20, 10], frames()
doc.onion_all_layers = True
assert [f for f, after, _ in frames() if not after] == [20, 15], frames()
doc.onion_all_layers = False

# A hidden layer shows no ghost wires.
layer.visible = False
assert all(not w["lines"] for w in oanim.wm.onion_wires())
layer.visible = True

# Fade and colours.
oanim.wm.set_onion_colors(before=(0.1, 0.2, 0.3), after=(0.4, 0.5, 0.6, 0.5))
colors = oanim.wm.onion_colors()
assert all(abs(a - b) < 1e-6 for a, b in zip(colors["before"], (0.1, 0.2, 0.3, 1.0))), colors
doc.onion_fade = False

# Saved with the project.
folder = tempfile.mkdtemp(prefix="oanim_onion_")
path = os.path.join(folder, "onion.oanim")
doc.onion_before = 3
oanim.ops.wm.save_as_mainfile(filepath=path)
oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
doc = oanim.data.document
assert doc.onion_skin and doc.onion_before == 3 and doc.onion_after == 1 and not doc.onion_fade
colors = oanim.wm.onion_colors()
assert all(abs(a - b) < 1e-6 for a, b in zip(colors["after"], (0.4, 0.5, 0.6, 0.5))), colors
shutil.rmtree(folder, ignore_errors=True)

print("onion skins: ok")
