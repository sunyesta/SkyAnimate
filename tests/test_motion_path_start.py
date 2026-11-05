"""Motion paths start where the animation starts (the Frame Range's Start): frame 0 is the setup pose, not part of the
shot. With a key on Start the path begins there; without one it reaches back to the last key before Start (frame 0's,
the setup's, when there is none) so the stretch the shot opens in can still be shaped, the part before Start drawn
faded. With Start at 0, from 0 as before. The End the same way: with no key on End, on to the first key after
it, faded past End. A point with only a handle picked traces too; path dots a handle lies over
are left out."""

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

# --- The End the same way: no key on End, on to the first key after it, faded past End --------------------------------
def last():
    return wm.motion_path_range()[1]


doc.frame_end = 7
assert last() == 10, ("no key on End: on to the key after it", wm.motion_path_range())
ops.wm.tool_select()
path = wm.overlay_colors(art, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 0.0)["paths"][0]
marks = {m["frame"]: m for m in path["marks"]}
assert marks[10]["alpha"] < 120 and marks[5]["alpha"] == 255, ("the key past End faded, the shot's not", marks[10], marks[5])
# Frame 7 to 10 is past End: faded.
tail = [k for k in path["pieces"] if k["alpha"] < 120]
assert tail, "the stretch past End faded"
ops.wm.tool_motion_path()
doc.frame_end = 5
assert last() == 5, "a key on End: to there"
doc.frame_end = 12
assert last() == 12, "no key after End: End"
doc.frame_end = 20

# --- A point with only a handle picked traces its path too -----------------------------------------------------------
ops.mesh.select_none()
assert wm.motion_paths() == [] or all("point" not in p["label"] for p in wm.motion_paths()), "nothing picked: no point path"
art.select_handle(0, 0, "OUT")
labels = [p["label"] for p in wm.motion_paths()]
assert labels == [art.name + " point 0"], ("its handle picked: the point's path", labels)

# --- A handle lying along the path: the dots under it are left out, so it reads as one bar --------------------------
line = [(0.0, 0.0, 100.0, 0.0)]
assert wm.path_dot_under_handles(50.0, 0.0, line), "on the line"
assert wm.path_dot_under_handles(50.0, 3.5, line), "within its width and a little"
assert not wm.path_dot_under_handles(50.0, 8.0, line), "beside it: shown"
assert not wm.path_dot_under_handles(110.0, 0.0, line), "past its tip: shown"

# --- An Auto handle with no length still shows, short, along the way the path runs -----------------------------------
# A key where the path turns back in both x and y (leftmost and highest, as a bounce's top corner): Smooth flattens its
# slope, so its Auto handles have no length; they show as stubs to grab.
ops.layer.new()
ball = layers.active
ball.add_curve([(0.0, 0.0), (0.2, 0.0)])
for l in layers.all():
    l.selected = l.id == ball.id
doc.frame_start, doc.frame_end = 0, 40
for frame, xy in ((10, (0.0, 0.0)), (20, (-1.0, 1.0)), (30, (0.0, 0.5))):
    assert ops.anim.motion_path_move(frame=str(frame), x=str(xy[0]), y=str(xy[1]), layer=str(ball.id)) == {'FINISHED'}
doc.frame_current = 20
doc.path_before, doc.path_after = 3, 3
ops.anim.motion_path_select(frame="20", layer=str(ball.id))
path = next(p for p in wm.motion_paths() if p["layer"] == ball.id and "point" not in p["label"])
h = next(h for h in path["handles"] if h["frame"] == 20)
assert h["in"] is None and h["out"] is None, ("no length at a turnaround", h)
assert h["in_stub"] is not None and h["out_stub"] is not None, ("shown short instead", h)
# Out runs toward the next key (right and down), in back toward the one before (right and down too).
assert h["out_stub"][0] > 0 and h["out_stub"][1] < 0, h["out_stub"]
assert h["in_stub"][0] > 0 and h["in_stub"][1] < 0, h["in_stub"]
assert abs(h["out_stub"][0] ** 2 + h["out_stub"][1] ** 2 - 1.0) < 1e-3, "a way, of length 1"
# A key with a real slope has no stub.
mid = next(h for h in path["handles"] if h["frame"] == 30) if any(h["frame"] == 30 for h in path["handles"]) else None
assert mid is None or mid["out_stub"] is None

print("Motion path start tests passed")
