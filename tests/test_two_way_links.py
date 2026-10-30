"""Two-way links (Switch layers stage 5): a driver with one variable and an expression like var,
-var or var * 0.5 made 2-Way Linked. Its expression becomes a Factor and a Flip; the keys are stored
once, the variable's; either side can be edited, an edit to the driven side going back into the
variable; each side keeps its own rest (the change from it is shared). Only drivers that can run
backwards can link; a property is in one group only; a smart bone can't link with what its own action
moves. Unlink gives the driven side keys of its own. Saved with the file. Link All Channels links a
bone swap's angle, position and scale at factor 1.

Run with:  OpenAnimate --background --python tests/test_two_way_links.py
"""

import json
import pathlib
import tempfile

import oanim

doc = oanim.data.document
doc.auto_key = True
ops = oanim.ops.anim
layers = oanim.data.layers
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_two_way_"))
doc.frame_current = 0

dial = layers.active
dial.name = "Dial"
oanim.ops.layer.new()
head = layers.active
head.name = "Head"
D, H = str(dial.id), str(head.id)


def goto(frame):
    doc.frame_current = frame


def keyed(layer, path):
    return [k["frame"] for k in layer.keyframes(path)]


def driver(layer, path, component=-1):
    for d in oanim.data.drivers:
        t = d["target"]
        if t["layer"] == int(layer) and t["path"] == path and t["component"] == component:
            return d
    return None


def var(layer, path, name="var"):
    return json.dumps([{"name": name, "layer": int(layer), "path": path, "component": -1}])


# The head rests turned 10 degrees; the dial at 0.
head.rotation = 10.0

# --- Only drivers that can run backwards link ---------------------------------------------------
assert ops.driver_add(layer=H, path="rotation", vars=var(D, "rotation"), expression="var * var") == {'FINISHED'}
assert ops.driver_two_way(layer=H, path="rotation") == {'CANCELLED'}, "var * var can't run backwards"
assert ops.driver_set(layer=H, path="rotation", expression="var * 0") == {'FINISHED'}
assert ops.driver_two_way(layer=H, path="rotation") == {'CANCELLED'}, "a factor of 0"
assert ops.driver_remove(layer=H, path="rotation") == {'FINISHED'}
head.rotation = 10.0

# The dial is keyed: 0 on frame 0, 40 on frame 20.
goto(20)
dial.rotation = 40.0
goto(21)
goto(0)
assert keyed(dial, "rotation") == [0, 20], keyed(dial, "rotation")

# --- Link: -var * 0.5 becomes Factor 0.5 with Flip; the change from rest is shared ---------------
assert ops.driver_add(layer=H, path="rotation", vars=var(D, "rotation", "d"), expression="-d * 0.5") == {'FINISHED'}
assert ops.driver_two_way(layer=H, path="rotation") == {'FINISHED'}
d = driver(H, "rotation")
assert d["two_way"] and abs(d["factor"] + 0.5) < 1e-6, d
goto(20)
assert abs(head.rotation - (10.0 - 20.0)) < 1e-4, ("rest 10, minus half of 40", head.rotation)

# --- Edit either side -----------------------------------------------------------------------------
# The driven side, on frame 10: the edit goes back into the dial, keyed there; the head keeps it.
goto(10)
head.rotation = -30.0  # a change of -40 from its rest: the dial's 80
goto(11)
goto(10)
assert abs(head.rotation + 30.0) < 1e-3, head.rotation
assert abs(dial.rotation - 80.0) < 1e-3, dial.rotation
assert keyed(dial, "rotation") == [0, 10, 20], keyed(dial, "rotation")
assert keyed(head, "rotation") == [], "the keys are stored once: the dial's"
# The side read: the head follows.
goto(20)
dial.rotation = 60.0
goto(21)
goto(20)
assert abs(head.rotation - (10.0 - 30.0)) < 1e-3, head.rotation

# --- One group per property; Flip and Factor ---------------------------------------------------------
oanim.ops.layer.new()
other = layers.active
other.name = "Other"
O = str(other.id)
assert ops.driver_add(layer=D, path="rotation", vars=var(O, "rotation")) == {'FINISHED'}
assert ops.driver_two_way(layer=D, path="rotation") == {'CANCELLED'}, "the dial is read by a link already"
assert ops.driver_remove(layer=D, path="rotation") == {'FINISHED'}
# A second link reading the dial: a group of three.
assert ops.driver_add(layer=O, path="rotation", vars=var(D, "rotation")) == {'FINISHED'}
assert ops.driver_two_way(layer=O, path="rotation") == {'FINISHED'}
goto(20)
assert abs(other.rotation - dial.rotation) < 1e-3
assert ops.driver_set(layer=O, path="rotation", factor="2") == {'FINISHED'}
goto(21)
goto(20)
assert abs(other.rotation - 2.0 * dial.rotation) < 1e-3, (other.rotation, dial.rotation)

# --- Saved with the file -------------------------------------------------------------------------
path = folder / "links.oanim"
assert oanim.ops.wm.save_as_mainfile(filepath=str(path)) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=str(path)) == {'FINISHED'}
doc = oanim.data.document
dial = next(l for l in layers.all() if l.name == "Dial")
head = next(l for l in layers.all() if l.name == "Head")
D, H = str(dial.id), str(head.id)
d = driver(H, "rotation")
assert d["two_way"] and abs(d["factor"] + 0.5) < 1e-6, d
goto(20)
assert abs(head.rotation - (10.0 - 30.0)) < 1e-3, head.rotation

# --- Unlink: the head gets keys of its own that move it as the link did ------------------------------
assert ops.driver_unlink(layer=H, path="rotation") == {'FINISHED'}
assert driver(H, "rotation") is None
assert keyed(head, "rotation") == keyed(dial, "rotation"), (keyed(head, "rotation"), keyed(dial, "rotation"))
for frame in (0, 10, 20):
    goto(frame)
    assert abs(head.rotation - (10.0 - 0.5 * dial.rotation)) < 1e-3, (frame, head.rotation, dial.rotation)
# Editing it now is its own.
goto(20)
head.rotation = 0.0
goto(21)
goto(20)
assert abs(head.rotation) < 1e-4 and abs(dial.rotation - 60.0) < 1e-3

# --- Link All Channels on a bone swap -------------------------------------------------------------
assert oanim.ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
doc = oanim.data.document
doc.auto_key = True
goto(0)
legs = {}
for name in ("Straight", "Bent"):
    oanim.ops.layer.new_bone()
    leg = layers.active
    leg.name = name
    oanim.ops.armature.bone_add(name="thigh", head_x="0", head_y="0", tail_x="0", tail_y="-1", parent="")
    legs[name] = leg
for l in legs.values():
    l.selected = True
assert oanim.ops.layer.wrap_in_switch() == {'FINISHED'}
sw = layers.active
assert oanim.ops.armature.swaps_auto_link(layer=sw.name) == {'FINISHED'}
first = next(c for c in sw.children if c.bones[0]["stands_in_for"] is None)
second = next(c for c in sw.children if c.id != first.id)
layers.active = second
second.select_bone("thigh")
assert oanim.ops.armature.bone_link_all(bone="thigh") == {'FINISHED'}
links = [x for x in oanim.data.drivers if x["two_way"] and x["target"]["layer"] == second.id]
assert len(links) == 5, [x["target"] for x in links]  # angle, position X and Y, scale X and Y
goto(10)
first.bone_set("thigh", angle=0.7)
goto(11)
goto(10)
assert abs(second.bones[0]["angle"] - first.bones[0]["angle"]) < 1e-4, (second.bones[0], first.bones[0])

print("two way links OK")
