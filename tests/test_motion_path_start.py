"""Motion paths start where the animation starts (the Frame Range's Start): frame 0 is the setup pose, not part of the
shot. With a key on Start the path begins there; without one it reaches back to the last key before Start (frame 0's,
the setup's, when there is none) so the stretch the shot opens in can still be shaped, the part before Start drawn
faded. With Start at 0, from 0 as before."""

import oanim

ops, doc, layers, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm

ops.layer.new()
art = layers.active
art.key_whole_drawing = False
art.add_curve([(0.0, 0.0), (1.0, 0.0)])
doc.auto_key = True
doc.motion_paths = True
doc.path_all = True
doc.frame_end = 20
for frame, x in ((5, 1.0), (10, 2.0)):
    doc.frame_current = frame
    art.set_point(0, (x, 1.0))
doc.frame_current = 7
for l in layers.all():
    l.selected = l.id == art.id
art.select_points([0])
ops.wm.tool_motion_path()


def first():
    return wm.motion_path_range()[0]


# No key on Start (1): back to the setup's frame 0, the stretch before Start faded.
doc.frame_start = 1
assert first() == 0, ("from the setup when no key is on Start", wm.motion_path_range())
ops.wm.tool_select()
path = wm.overlay_colors(art, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 0.0)["paths"][0]
# Frame 0 to 1 is the first step along it (from (0, 0)); the rest is the shot.
early = [k for k in path["pieces"] if abs(k["a"][0]) < 1e-4 and abs(k["a"][1]) < 1e-4]
later = [k for k in path["pieces"] if k not in early]
assert early and later, path["pieces"]
assert all(k["alpha"] < 120 for k in early) and all(k["alpha"] > 200 for k in later), \
    ("faded before Start, solid after", [k["alpha"] for k in early], [k["alpha"] for k in later])
marks = {m["frame"]: m for m in path["marks"]}
assert marks[0]["alpha"] < 120 and marks[5]["alpha"] == 255, ("the setup's key faded, the shot's not", marks[0], marks[5])
ops.wm.tool_motion_path()

# A key on Start: the path begins there, nothing before it.
doc.frame_current = 1
art.set_point(0, (0.5, 0.5))
doc.frame_current = 7
assert first() == 1, ("a key on Start: from there", wm.motion_path_range())

# Start later, between keys: back to the key before it (a key of the shot, fully editable).
doc.frame_start = 7
assert first() == 5, ("the key before Start", wm.motion_path_range())
doc.frame_start = 10
assert first() == 10, "a key on Start"

# Start at 0: from 0, as before.
doc.frame_start = 0
assert first() == 0

print("Motion path start tests passed")
