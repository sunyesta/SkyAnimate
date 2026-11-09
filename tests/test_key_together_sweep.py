"""Key Together's fillers and Auto-Freeze Keys' holds, from a bug sweep: an edit on a filler's frame is the channel's own
even right after opening; a layer that can't key together any more (a group made a switch) lets its fillers go; Key
Whole Drawing's keys for a drawing's other points hold their poses either side too; a hold key plays as the stretch it
splits; what a pose shows is read before a filler there becomes a key of its own; a switch pick holds the poses too; with Smooth keys every pose the bend reaches holds; a key moved alone in
a group keying together takes its frame along.

Run with:  OpenAnimate --background --python tests/test_key_together_sweep.py
"""

import os
import shutil
import tempfile

import oanim

ops, layers, wm = oanim.ops, oanim.data.layers, oanim.wm
TMP = tempfile.mkdtemp(prefix="oanim_together_sweep_")


def fresh():
    assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
    doc = oanim.data.document
    doc.auto_key = True
    doc.auto_freeze_keys = False  # Key Together alone
    return doc


def keys(layer, path, filler=None):
    try:
        return [k["frame"] for k in layer.keyframes(path) if filler is None or bool(k.get("filler")) == filler]
    except KeyError:
        return []


def settle(doc):
    doc.frame_current = doc.frame_current + 1
    doc.frame_current = doc.frame_current - 1


def reopen(name):
    path = os.path.join(TMP, name)
    ops.wm.save_copy(filepath=path)
    assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
    return oanim.data.document


def vec_layer(name, pts=((0.0, 0.0), (1.0, 0.0))):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    layer.key_whole_drawing = True
    layer.add_curve(list(pts))
    return layer


def by_name(name):
    return [l for l in layers.all() if l.name == name][0]


def angle(name, frame):
    """The bone's angle on `frame` (of the `rig` the section made)."""
    doc.frame_current = frame
    return next(x for x in rig.bones if x["name"] == name)["angle"]


def turn(frame, name, value):
    doc.frame_current = frame
    rig.bone_set(name, angle=value)
    doc.frame_current = frame + 1


# --- Right after opening, an edit on a filler's frame is kept (vector layer, a Python edit) ---------------------------
doc = fresh()
hop = vec_layer("Hop")
doc.frame_current = 30
hop.location_x = 3.0
doc.frame_current = 10
hop.rotation = 0.5
doc.frame_current = 20
hop.rotation = 1.0
doc.frame_current = 0
hop.key_together = True
settle(doc)
assert 10 in keys(hop, "position", filler=True), keys(hop, "position", filler=True)
doc.frame_current = 10
doc = reopen("hop.oanim")
hop = by_name("Hop")
hop.location_y = 2.0
doc.frame_current = 0
doc.frame_current = 10
assert abs(hop.location_y - 2.0) < 1e-4, ("the edit on 10 is kept", hop.location_y)
assert 10 not in keys(hop, "position", filler=True), "the key edited is the channel's own"
hop.key_together = False
settle(doc)
doc.frame_current = 10
assert abs(hop.location_y - 2.0) < 1e-4, ("and stays when Key Together is off", hop.location_y)

# --- The same through an operator: R on a bone on a filler pose, right after opening ----------------------------------
doc = fresh()
ops.layer.new_bone()
rig = layers.active
rig.name = "Rig"
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.select_none()
ops.armature.bone_add(name="leg", head_x="0", head_y="-1", tail_x="1", tail_y="-1", parent="")
rig.select_bone("arm")
doc.frame_current = 30
assert ops.armature.rotate(angle="60") == {'FINISHED'}
rig.select_bone("leg")
doc.frame_current = 10
assert ops.armature.rotate(angle="20") == {'FINISHED'}
doc.frame_current = 20
assert ops.armature.rotate(angle="-20") == {'FINISHED'}
doc.frame_current = 0
rig.key_together = True
settle(doc)
doc.frame_current = 10
doc = reopen("rig.oanim")
rig = by_name("Rig")
layers.active = rig


def arm_angle():
    return next(b for b in rig.bones if b["name"] == "arm")["angle"]


rig.select_bone("arm")
assert ops.armature.rotate(angle="30") == {'FINISHED'}
turned = arm_angle()
doc.frame_current = 11
doc.frame_current = 10
assert abs(arm_angle() - turned) < 1e-4, ("R on 10 right after opening is kept", turned, arm_angle())

# --- A group with Key Together made a switch lets its fillers go ------------------------------------------------------
doc = fresh()
a = vec_layer("A")
b = vec_layer("B", ((0.0, 3.0), (1.0, 3.0)))
doc.frame_current = 10
a.location_x = 1.0
doc.frame_current = 20
b.location_y = 1.0
doc.frame_current = 0
for l in layers.all():
    l.selected = l.id in (a.id, b.id)
assert ops.layer.group() == {'FINISHED'}
g = layers.active
assert ops.anim.key_together_set(layer=str(g.id), on="1") == {'FINISHED'}
settle(doc)
assert keys(b, "position", filler=True) == [10] and keys(a, "position", filler=True) == [20]
for l in layers.all():
    l.selected = l.id == g.id
assert ops.layer.convert_to_switch() == {'FINISHED'}
settle(doc)
assert not keys(b, "position", filler=True) and not keys(a, "position", filler=True), \
    ("a switch keys nothing together: its fillers go", keys(b, "position"), keys(a, "position"))
assert not g.key_together, "nor is it left on"
assert keys(a, "position") == [0, 10] and keys(b, "position") == [0, 20], "the own keys stay"
assert ops.ed.undo() == {'FINISHED'}
g, a, b = by_name(g.name), by_name("A"), by_name("B")
assert g.key_together and keys(b, "position", filler=True) == [10], "undo brings the group and its fillers back"

# --- Key Whole Drawing in a Key Together group: one point keyed between poses, the others hold them -------------------
for freeze in (False, True):
    doc = fresh()
    doc.auto_freeze_keys = freeze
    a = vec_layer("A", ((0.0, 0.0), (1.0, 0.0), (2.0, 0.0)))
    b = vec_layer("B", ((0.0, 3.0), (1.0, 3.0)))
    doc.frame_current = 30
    a.set_point(1, (2.0, -1.0))
    doc.frame_current = 45
    a.set_point(1, (0.0, 2.0))
    doc.frame_current = 10
    b.location_y = 1.0
    doc.frame_current = 20
    b.location_y = 0.5
    doc.frame_current = 0
    for l in layers.all():
        l.selected = l.id in (a.id, b.id)
    assert ops.layer.group() == {'FINISHED'}
    g = layers.active
    g.key_together = True
    settle(doc)


    def points_at(frame):
        doc.frame_current = frame
        return [tuple(p) for p in a.points]


    poses = (0, 10, 20, 30, 45)
    before = {f: points_at(f) for f in poses}
    doc.frame_current = 15
    a.set_point(0, (0.0, 1.0))
    doc.frame_current = 0
    settle(doc)
    for f in poses:
        after = points_at(f)
        worst = max(abs(x - y) for p, q in zip(before[f], after) for x, y in zip(p, q))
        assert worst < 1e-4, ("the drawing holds its poses either side", freeze, f, before[f], after)

# --- Auto-Freeze: a hold key plays as the Linear stretch it splits, so the pose before it holds ----------------------
doc = fresh()
ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="b", head_x="0", head_y="1", tail_x="1", tail_y="1", parent="")


turn(30, "a", 0.3)
rig.keyframe_set('bones["a"].angle', 30, interp="LINEAR")
turn(5, "b", 0.1)
turn(10, "b", 0.2)
before = angle("a", 5)
doc.auto_freeze_keys = True
turn(20, "a", 1.0)
assert abs(angle("a", 5) - before) < 1e-5, ("the Linear stretch's pose on 5 holds", before, angle("a", 5))
held = [k for k in rig.keyframes('bones["a"].angle') if k["frame"] == 10]
assert held and held[0]["interp"] == "LINEAR", ("the hold key on 10 plays as the stretch it split", held)

# --- Auto-Freeze with Key Together: the pose before the edit shows what it did, read before the filler is taken -----
doc = fresh()
sa = vec_layer("SA")
sa.key_whole_drawing = False
sb = vec_layer("SB", ((0.0, 3.0), (1.0, 3.0)))
for l in layers.all():
    l.selected = l.id in (sa.id, sb.id)
assert ops.layer.group() == {'FINISHED'}
g = layers.active
for f, y in ((10, 1.0), (40, 4.0), (50, 0.0)):
    doc.frame_current = f
    sa.set_point(1, (1.0, y))
doc.frame_current = 20
sb.set_point(0, (0.0, 4.0))
doc.frame_current = 0
g.key_together = True
settle(doc)
assert 20 in keys(sa, "points[1].pos", filler=True), keys(sa, "points[1].pos", filler=True)
sa.keyframe_insert("visible", 25)  # a pose of sa's on 25, its point not keyed there


def y_at(frame):
    doc.frame_current = frame
    return sa.points[1][1]


before = y_at(25)
doc.auto_freeze_keys = True
doc.frame_current = 30
sa.set_point(1, (1.0, 8.0))
doc.frame_current = 31
assert abs(y_at(25) - before) < 1e-5, ("the pose on 25 holds what it showed", before, y_at(25))

# --- Auto-Freeze: a switch pick between poses holds the child shown on the pose after ---------------------------------
doc = fresh()
ops.layer.new_switch()
sw = layers.active
for name in ("A", "B", "C"):
    layers.active = sw
    ops.layer.new()
    child = layers.active
    child.name = name
    child.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5)], closed=True, fill=True)
    ops.layer.move_to_group(group=str(sw.id))
layers.active = sw
for f, x in ((10, 1.0), (30, 2.0)):
    doc.frame_current = f
    sw.location_x = x
    doc.frame_current = f + 1


def shown():
    out = {}
    for f in (10, 30, 40):
        doc.frame_current = f
        out[f] = sw.switch_active
    return out


before = shown()
doc.auto_freeze_keys = True
doc.frame_current = 20
assert ops.layer.switch_set(layer=sw.name, child="C") == {'FINISHED'}
doc.frame_current = 20
assert sw.switch_active == "C", sw.switch_active
assert shown() == before, ("the poses keep their child", before, shown())
doc.auto_freeze_keys = False
doc.frame_current = 35
assert ops.layer.switch_set(layer=sw.name, child="B") == {'FINISHED'}
assert keys(sw, "switch") == [0, 10, 20, 30, 35], ("without Auto-Freeze only the pick's key", keys(sw, "switch"))

# --- Auto-Freeze with Smooth keys: every pose the edit's bend reaches holds, not just the nearest two ---------------
doc = fresh()
ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="b", head_x="0", head_y="1", tail_x="1", tail_y="1", parent="")
turn(20, "a", 0.2)
turn(40, "a", 0.4)
for f, v in ((10, 0.1), (30, 0.3), (35, 0.35)):
    turn(f, "b", v)
poses = (0, 10, 20, 30, 35, 40)
before = {f: angle("a", f) for f in poses}
doc.auto_freeze_keys = True
turn(25, "a", 1.0)
moved = {f: (before[f], angle("a", f)) for f in poses if abs(angle("a", f) - before[f]) > 1e-5}
assert not moved, ("no pose of the layer moves", moved)
assert keys(rig, 'bones["a"].angle') == [0, 10, 20, 25, 30, 35, 40], keys(rig, 'bones["a"].angle')
# A Linear curve bends nothing past the held poses: only those two.
doc = fresh()
ops.layer.new_bone()
rig = layers.active
ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="b", head_x="0", head_y="1", tail_x="1", tail_y="1", parent="")
turn(40, "a", 0.4)
rig.keyframe_set('bones["a"].angle', 40, interp="LINEAR")
for f, v in ((10, 0.1), (20, 0.2), (30, 0.3), (35, 0.35)):
    turn(f, "b", v)
doc.auto_freeze_keys = True
turn(25, "a", 1.0)
assert keys(rig, 'bones["a"].angle') == [0, 20, 25, 30, 40], ("Linear: only the poses either side", keys(rig, 'bones["a"].angle'))

# --- Key Together alone: a Smooth edit doesn't bend the curve past its fillers' poses --------------------------------
doc = fresh()
hop = vec_layer("Hop2")
doc.frame_current = 30
hop.location_x = 3.0
hop.location_y = 3.0
doc.frame_current = 10
hop.rotation = 0.5
doc.frame_current = 20
hop.rotation = 1.0
doc.frame_current = 0
hop.key_together = True
settle(doc)


def place(frame):
    doc.frame_current = frame
    return hop.location_x, hop.location_y


before = {f: place(f) for f in (0, 10, 20, 30)}
doc.frame_current = 25
hop.location_y = 0.0
doc.frame_current = 0
for f in (0, 10, 20, 30):
    assert max(abs(x - y) for x, y in zip(place(f), before[f])) < 1e-4, ("the poses hold", f, before[f], place(f))

# --- Key Together: moving one channel's key alone takes its frame on every layer of the group along ----------------
doc = fresh()
a = vec_layer("A")
b = vec_layer("B", ((0.0, 3.0), (1.0, 3.0)))
doc.frame_current = 10
a.location_x = 1.0
b.location_y = 1.0
doc.frame_current = 20
a.location_x = 0.0
b.location_y = 0.0
doc.frame_current = 0
for l in layers.all():
    l.selected = l.id in (a.id, b.id)
assert ops.layer.group() == {'FINISHED'}
g = layers.active
g.key_together = True
settle(doc)
doc.frame_current = 10
b10 = b.location_y
ops.anim.keys_deselect_all()
assert ops.anim.keys_select(frame_start="10", layers=str(a.id), path="position") == {'FINISHED'}
assert ops.anim.keys_offset(frames="2") == {'FINISHED'}
settle(doc)
assert keys(a, "position") == [0, 12, 20] and keys(b, "position") == [0, 12, 20], \
    ("the frame moves on every layer", keys(a, "position"), keys(b, "position"))
assert not keys(b, "position", filler=True), "B's own key moved, still its own"
doc.frame_current = 12
assert abs(b.location_y - b10) < 1e-5, ("B shows on 12 what it did on 10", b.location_y, b10)
# Not keying together: only the key picked moves.
g.key_together = False
settle(doc)
ops.anim.keys_deselect_all()
assert ops.anim.keys_select(frame_start="12", layers=str(a.id), path="position") == {'FINISHED'}
assert ops.anim.keys_offset(frames="2") == {'FINISHED'}
assert keys(a, "position") == [0, 14, 20] and keys(b, "position") == [0, 12, 20], \
    ("off: only A's", keys(a, "position"), keys(b, "position"))

# --- Key Together: entering an action and leaving changes nothing (its fillers come once something there is keyed) ---
doc = fresh()
ops.layer.new_bone()
rig = layers.active
rig.name = "ActRig"
ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="b", head_x="0", head_y="1", tail_x="1", tail_y="1", parent="")
assert ops.anim.action_new(name="act") == {'FINISHED'} and doc.action == "act"
turn(10, "a", 0.4)
turn(20, "b", 0.5)
doc.action = ""
doc.frame_current = 0
assert ops.anim.key_together_set(layer=str(rig.id), on="1") == {'FINISHED'}
settle(doc)


def saved():
    path = os.path.join(TMP, "look.oanim")
    ops.wm.save_copy(filepath=path)
    import json
    import zipfile
    with zipfile.ZipFile(path) as z:
        d = json.loads(z.read("document.json"))["document"]
    for k in ("frame_current", "ui_open", "ui_panel_order", "viewport_view", "texts", "name", "action", "action_layer"):
        d.pop(k, None)
    return d


before = saved()
doc.action = "act"
doc.frame_current = 5
doc.frame_current = 0
doc.action = ""
assert saved() == before, "entering the action and leaving changed nothing"
# Keyed there, it keys together: b gets a filler on 10.
doc.action = "act"
turn(30, "a", 0.8)
assert 10 in keys(rig, 'bones["b"].angle', filler=True), keys(rig, 'bones["b"].angle')

shutil.rmtree(TMP, ignore_errors=True)
print("key together sweep: ok")
