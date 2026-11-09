"""Clear Timeline (the timeline's right-click menu off any key; anim.timeline_clear): every key of the timeline being
edited goes, whatever is picked or shown, frame 0's setup pose staying. On the mainline that is every layer's, the
camera's and the Simulation's, the actions left alone; in an action only that action's, the mainline left alone. It
asks first with how many keys go, and is one undo step.

Run with:  OpenAnimate --background --python tests/test_timeline_clear.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm
doc.frame_current = 0
doc.auto_key = True


def frames(layer, path):
    try:
        return [k["frame"] for k in layer.keyframes(path)]
    except KeyError:
        return []


def close(a, b):
    return math.dist(a, b) < 1e-4


# A drawing: its points reshaped on 10, moved on 20.
art = layers.active
art.name = "art"
art.add_curve([(0.0, 0.0), (1.0, 0.0)])
setup = [tuple(p[:2]) for p in art.points]
doc.frame_current = 10
art.set_point(1, (1.0, 1.0))
doc.frame_current = 20
art.location_x = 2.0
doc.frame_current = 0
# A bone layer: the arm turned on 15 in the mainline, and on 5 in an action "wave".
ops.layer.new_bone()
rig = layers.active
rig.name = "rig"
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
doc.frame_current = 15
rig.bone_set("arm", angle=0.5)
doc.frame_current = 0
assert ops.anim.action_new(name="wave") == {'FINISHED'}
doc.frame_current = 5
rig.bone_set("arm", angle=1.0)
doc.frame_current = 6
doc.action = ""
doc.frame_current = 0
# The camera turned on 30.
cam = oanim.data.camera
doc.frame_current = 30
assert ops.anim.keyframe_toggle(layer=str(cam.id), paths="rotation") == {'FINISHED'}
doc.frame_current = 0
assert frames(rig, 'bones["arm"].angle') == [0, 15], frames(rig, 'bones["arm"].angle')

# Only the rig picked, Only Selected on: the drawing's and camera's keys go all the same.
for l in layers:
    l.selected = l.id == rig.id
layers.active = rig
assert doc.timeline_only_selected
q = wm.timeline_clear_question()
assert q.startswith("Delete all ") and q.endswith(" keys on Mainline?"), q
n = int(q.split()[2])
assert n >= 5, ("the drawing's, the rig's and the camera's keys after frame 0", q)

doc.frame_current = 20
assert ops.anim.timeline_clear() == {'FINISHED'}
assert wm.undo_history()[0][-1] == "Clear Timeline", wm.undo_history()[0][-1]
for layer, path in ((art, "points[1].pos"), (art, "position"), (rig, 'bones["arm"].angle'), (cam, "rotation")):
    assert len([f for f in frames(layer, path) if f != 0]) == 0, (layer.name, path, frames(layer, path))
assert close(art.points[1][:2], setup[1]) and abs(art.location_x) < 1e-4, ("the setup pose, on every frame",
                                                                          art.points, art.location_x)
assert ops.anim.timeline_clear() == {'CANCELLED'}, "nothing left to clear"
assert wm.timeline_clear_question() == "Delete all 0 keys on Mainline?", wm.timeline_clear_question()

# The action wasn't touched.
doc.action = "wave"
assert 5 in frames(rig, 'bones["arm"].angle'), ("the action's keys stay", frames(rig, 'bones["arm"].angle'))
doc.action = ""

# Undo brings it all back.
assert ops.ed.undo() == {'FINISHED'}
assert frames(rig, 'bones["arm"].angle') == [0, 15] and 10 in frames(art, "points[1].pos") and 30 in frames(
    cam, "rotation"), (frames(rig, 'bones["arm"].angle'), frames(art, "points[1].pos"), frames(cam, "rotation"))

# In the action: only its keys; the mainline left alone.
doc.action = "wave"
q = wm.timeline_clear_question()
assert q.endswith(" on wave?"), q
assert ops.anim.timeline_clear() == {'FINISHED'}
assert [f for f in frames(rig, 'bones["arm"].angle') if f != 0] == [], frames(rig, 'bones["arm"].angle')
doc.action = ""
assert frames(rig, 'bones["arm"].angle') == [0, 15], ("the mainline stays", frames(rig, 'bones["arm"].angle'))
assert 10 in frames(art, "points[1].pos") and 30 in frames(cam, "rotation")

print("timeline clear: ok")
