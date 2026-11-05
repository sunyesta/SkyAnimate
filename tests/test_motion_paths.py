"""Motion paths: where the selected bones or points (or a layer's origin) go over the frames around this one, a dot
per frame coloured by speed; the range by keys, what they trace, Relative To, pins, colours, snapshots, saving,
and Alt Space playing just the path's range.

Run with:  OpenAnimate --background --python tests/test_motion_paths.py
"""

import math
import os
import shutil
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
paths = oanim.wm.motion_paths


def close(a, b, eps=1e-4):
    if isinstance(a, (tuple, list)):
        return all(abs(x - y) < eps for x, y in zip(a, b))
    return abs(a - b) < eps


assert not doc.motion_paths, "off by default"
assert doc.path_before == 1 and doc.path_after == 1 and not doc.path_all and doc.path_step == 1
assert doc.path_lines and doc.path_dot_colors == "TIMING_CURRENT" and doc.path_relative == "WORLD"

# --- A layer with nothing picked traces its origin, over the keys either side --------------------------------
doc.auto_key = True
oanim.ops.layer.new()
box = layers.active
box.key_whole_drawing = False  # paths of single points here
box.add_curve([(0.0, 0.0), (1.0, 0.0)])
for frame, x in ((10, 1.0), (20, 2.0), (40, 4.0)):
    doc.frame_current = frame
    box.location_x = x
doc.frame_current = 30
box.location_x = 2.0  # held 20-30 (it was 3 here, between 20 and 40)
doc.frame_current = 15
assert paths() == [], "nothing while it is off"
doc.motion_paths = True
(p,) = paths()
assert p["label"] == box.name and p["layer"] == box.id and p["keys"] == [10, 20], p
assert p["first"] == 10 and len(p["points"]) == 11, (p["first"], len(p["points"]))
assert oanim.wm.motion_path_range() == (10, 20)
assert close(p["points"][0], (1.0, 0.0)) and close(p["points"][10], (2.0, 0.0)), "the origin on its keys"
assert doc.frame_current == 15 and close(box.location_x, box.location_x), "the document is left as it was"

# Two keys back and one ahead; on a key, that key's neighbours.
doc.path_before = 2
assert oanim.wm.motion_path_range() == (1, 20), "short of keys: the animation's start"
doc.frame_current = 25
assert oanim.wm.motion_path_range() == (10, 30), oanim.wm.motion_path_range()
doc.frame_current = 30
doc.path_before = 1
assert oanim.wm.motion_path_range() == (20, 40)
doc.path_all = True
doc.frame_end = 60
assert oanim.wm.motion_path_range() == (1, 60), "All: the whole animation"
doc.path_all = False

# Speed: 0 where it holds still (20-30), 1 on its fastest frame.
(p,) = paths()
speed = dict(zip(range(p["first"], p["first"] + len(p["speed"])), p["speed"]))
assert all(speed[f] < 1e-4 for f in range(21, 31)), "held: still"
assert close(max(speed.values()), 1.0) and max(speed, key=speed.get) > 30, "fastest on the way to 40"
still, fast = p["speed_colors"]
assert fast[0] > fast[2] and still[2] > still[0], "red where fast, blue where still"

# --- A bone traces its tip (an arc as it turns) and its head ---------------------------------------------------
doc.frame_current = 0
oanim.ops.layer.new_bone()
rig = layers.active
rig.key_whole_drawing = False  # paths of single points here
assert oanim.ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="2", tail_y="0", parent="") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("arm", angle=0.2)
doc.frame_current = 20
rig.bone_set("arm", angle=math.pi / 2)
doc.frame_current = 15
rig.select_bone("arm")
p, h = paths()
assert p["label"] == "arm tip" and (p["first"], len(p["points"])) == (10, 11), (p["label"], p["first"])
assert close(p["points"][0], (2 * math.cos(0.2), 2 * math.sin(0.2))) and close(p["points"][10], (0.0, 2.0)), "tip at both keys"
assert all(close(math.hypot(*pt), 2.0) for pt in p["points"]), "turning draws an arc about the head"
assert h["head"] and h["label"] == "arm head" and all(close(pt, (0.0, 0.0)) for pt in h["points"]), "the head stays"
# Each has its own colour: the head's style is its own.
assert oanim.ops.anim.motion_path_style(layer=str(rig.id), bone="arm", head="1", color="1,0,0") == {'FINISHED'}
p, h = paths()
assert h["color"] != p["color"], "the head recoloured alone"
oanim.ops.anim.motion_path_style(layer=str(rig.id), bone="arm", head="1", color="")

# A connected bone's head rides on its parent's tip: only its tip is traced.
doc.frame_current = 0
assert oanim.ops.armature.bone_add(name="hand", head_x="2", head_y="0", tail_x="3", tail_y="0", parent="arm") == {'FINISHED'}
assert oanim.ops.armature.bone_relations_set(bone="hand", connected="1") == {'FINISHED'}
hand = next(b for b in rig.bones if b["name"] == "hand")
assert hand["connected"], hand
doc.frame_current = 15
rig.select_bone("hand")
assert [q["label"] for q in paths()] == ["hand tip"], [q["label"] for q in paths()]

# A bone that only moves: its tip goes as its head does, timed by its position keys (none of its own to turn).
doc.frame_current = 0
assert oanim.ops.armature.bone_add(name="brow", head_x="0", head_y="-1", tail_x="1", tail_y="-1", parent="") == {'FINISHED'}
doc.frame_current = 9
rig.bone_set("brow", position=(0.0, -0.5))
doc.frame_current = 4
rig.select_bone("brow")
timing = {r["label"]: r["stretches"] for r in oanim.wm.motion_path_timing()}
assert [(s["start"], s["end"]) for s in timing["brow tip"]] == [(0, 9)], timing
assert [(s["start"], s["end"]) for s in timing["brow head"]] == [(0, 9)], timing
assert oanim.ops.anim.motion_path_move(frame="9", x="1", y="0", layer=str(rig.id), bone="brow") == {'FINISHED'}
tip = next(q for q in paths() if q["label"] == "brow tip")
assert close(tip["points"][9 - tip["first"]], (1.0, 0.0)), "dragging the tip moves the bone there"
assert all(r["angle"] == 0.0 for r in rig.bones if r["name"] == "brow"), "rather than turning it"
rig.select_bone("arm")
doc.frame_current = 15

# The ghost a dot shows is the bone layer's drawings on that frame (none here) and leaves the document alone.
assert oanim.wm.motion_path_ghost(12) == [] and doc.frame_current == 15

# --- Relative To: the parent's own motion taken out ----------------------------------------------------------
layers.active = box
for l in layers:
    l.selected = l.name == box.name
doc.frame_current = 15
assert oanim.ops.anim.motion_path_relative(to="PARENT") == {'FINISHED'}
assert doc.path_relative == "PARENT"
(p,) = paths()
assert all(close(pt, p["points"][0]) for pt in p["points"]), "relative to itself it stands still"
assert oanim.ops.anim.motion_path_relative(to="WORLD") == {'FINISHED'} and doc.path_relative == "WORLD"
assert oanim.ops.anim.motion_path_relative(to="SIDEWAYS") == {'CANCELLED'}

# --- A selected point traces itself ---------------------------------------------------------------------------
box.select_points([1])
(p,) = paths()
assert p["label"] == box.name + " point 1", p["label"]
assert close(p["points"][0], (2.0, 0.0)) and close(p["points"][10], (3.0, 0.0)), "the point, moved by its layer"
box.select_points([1], False)

# --- Pin keeps a path when it isn't selected; a colour of its own ---------------------------------------------
layers.active = rig
for l in layers:
    l.selected = l.name == rig.name
rig.select_bone("arm")
assert oanim.ops.anim.motion_path_pin() == {'FINISHED'}
assert oanim.ops.anim.motion_path_style(color="0.1,0.9,0.2") == {'FINISHED'}
assert oanim.ops.anim.motion_path_style(color="green") == {'CANCELLED'}
layers.active = box
for l in layers:
    l.selected = l.name == box.name
labels = sorted(q["label"] for q in paths())
assert labels == sorted([box.name, "arm tip", "arm head"]), labels
arm = next(q for q in paths() if q["label"] == "arm tip")
assert arm["pinned"] and close(arm["color"], (0.1, 0.9, 0.2))

# --- A snapshot to compare an edit against --------------------------------------------------------------------
assert oanim.ops.anim.motion_path_snapshot() == {'FINISHED'}
snap = oanim.wm.motion_path_snapshot()
assert len(snap) == 3 and any(close(s[0], (1.0, 0.0)) for s in snap), snap
assert oanim.ops.anim.motion_path_snapshot(clear="1") == {'FINISHED'} and oanim.wm.motion_path_snapshot() == []

# --- Saved with the file --------------------------------------------------------------------------------------
doc.path_before, doc.path_after, doc.path_step = 3, 2, 2
doc.path_lines, doc.path_dot_colors = False, "SPEED"
doc.path_relative = "CAMERA"
folder = tempfile.mkdtemp()
try:
    path = os.path.join(folder, "paths.oanim")
    assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
    assert oanim.ops.wm.read_homefile() == {'FINISHED'}
    assert not oanim.data.document.motion_paths
    assert oanim.ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
    doc = oanim.data.document
    assert doc.motion_paths and (doc.path_before, doc.path_after, doc.path_step) == (3, 2, 2)
    assert not doc.path_lines and doc.path_dot_colors == "SPEED" and doc.path_relative == "CAMERA"
    doc.path_relative = "WORLD"
    arm = next(q for q in paths() if q["label"] == "arm tip")
    assert arm["pinned"] and close(arm["color"], (0.1, 0.9, 0.2)), "pins and colours kept"
finally:
    shutil.rmtree(folder, ignore_errors=True)

# --- Alt Space loops just the path's range; Esc goes back --------------------------------------------------------
doc.path_before, doc.path_after = 1, 1
layers.active = next(l for l in layers if l.name == box.name)
doc.frame_current = 35
assert oanim.wm.motion_path_range() == (30, 40)
assert oanim.wm.event_simulate("SPACE", alt=True)
assert oanim.wm.play_range() == (True, 30, 40) and doc.frame_current == 30, oanim.wm.play_range()
assert oanim.wm.event_simulate("ESC") and not oanim.context.is_playing and doc.frame_current == 35
assert oanim.wm.event_simulate("SPACE", alt=True) and oanim.wm.event_simulate("SPACE")
assert not oanim.context.is_playing, "Space stops it"
assert oanim.wm.event_simulate("SPACE") and oanim.wm.play_range() == (True, 0, 0), "plain Space plays it all"
oanim.wm.event_simulate("SPACE")

print("Motion path tests passed")
