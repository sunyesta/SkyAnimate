"""Fix actions (correctives, Maya's combination shapes): one per set of action bone actions playing together, fixing
one or more of their poses, each on a frame of its own. Exactly at a pose its keys play; between poses they
blend by closeness; toward rest each fades as its drivers do. Fixing a pose near one it has opens that one,
elsewhere adds one starting from what played there. A fix for three is keyed on top of the one for two of
them, and fades out while any of its own rests. Action bone actions turned less than 5% don't count.

Run with:  OpenAnimate --background --python tests/test_fix_actions.py
"""

import json
import pathlib
import tempfile
import zipfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True

ops.layer.new_bone()
rig = layers.active
for i, name in enumerate(("a", "b", "c")):
    ops.armature.bone_add(name=name, head_x="0", head_y=str(i), tail_x="1", tail_y=str(i), parent="")
ops.layer.new()
face = layers.active
ops.layer.move_to_group(group=str(rig.id))


def find(layer_id):
    return next(l for l in layers.all() if l.id == layer_id)


def bone(name):
    return next(x for x in find(rig.id).bones if x["name"] == name)


def turn(**amounts):
    """Turn action bones by a share of their action (each action turns its bone 1 over 10 frames)."""
    for name, a in amounts.items():
        find(rig.id).bone_set(name, angle=bone(name)["setup_angle"] + a)
    doc.frame_current = doc.frame_current  # a sync, as the app does every frame


def fix(name=None):
    fixes = find(rig.id).fix_actions
    return fixes[0] if name is None else next(f for f in fixes if f["name"] == name)


def close(a, b, eps=1e-3):
    return abs(a - b) < eps


def rot():
    return find(face.id).rotation


# Action bone actions, linear so shares are easy: "a" moves the face right by 2, "b" up by 1, "c" right by 1 more.
layers.active = rig
for name, axis, amount in (("a", "location_x", 2.0), ("b", "location_y", 1.0), ("c", "location_x", 1.0)):
    assert ops.anim.action_new(name=name, action_bone="1") == {'FINISHED'}
    doc.frame_current = 10
    find(rig.id).bone_set(name, angle=bone(name)["setup_angle"] + 1.0)
    f = find(face.id)
    setattr(f, axis, getattr(f, axis) + amount)
    doc.frame_current = 0
    assert ops.anim.action_set(name="") == {'FINISHED'}
# Linear keys: the shares below are exact.
for name in ("a", "b", "c"):
    ops.anim.action_set(name=name)
    for target, path in ((find(rig.id), f'bones["{name}"].angle'), (find(face.id), "position")):
        for k in target.keyframes(path):
            target.keyframe_set(path, k["frame"], interp="LINEAR")
    ops.anim.action_set(name="")
layers.active = find(rig.id)

# On the mainline, a and b turned all the way: each plays; nothing fixes them yet.
doc.frame_current = 20
turn(a=1.0, b=1.0)
assert close(find(face.id).location_x, 2.0) and close(find(face.id).location_y, 1.0)

# One alone isn't a combination to fix.
turn(b=0.0)
assert ops.anim.fix_action_new() == {'CANCELLED'}
turn(b=1.0)

# Fix This Pose: a new fix named after them, its first pose on frame 10, opened there with them held.
assert ops.anim.fix_action_new() == {'FINISHED'}
name = "a × b"
assert doc.action == name and doc.frame_current == 10, (doc.action, doc.frame_current)
info = fix()
assert info["drivers"] == ["a", "b"] and len(info["poses"]) == 1, info
assert info["poses"][0]["frame"] == 10 and all(close(s, 1.0) for s in info["poses"][0]["shares"]), info
assert {x["name"]: x["kind"] for x in find(rig.id).actions}[name] == "FIX"
assert close(find(face.id).location_x, 2.0), "the drivers held at the pose"
assert close(bone("a")["angle"] - bone("a")["setup_angle"], 1.0), "its bones shown turned"
# The correction: turned 30 degrees.
find(face.id).rotation = 30.0
doc.frame_current = 9
doc.frame_current = 10
assert close(rot(), 30.0)

# Back on the mainline: at the pose, all of it; one alone, none; part way, the shares multiplied.
assert ops.anim.action_set(name="") == {'FINISHED'}
doc.frame_current = 20
turn(a=1.0, b=1.0)
assert close(rot(), 30.0) and close(fix()["strength"], 1.0), rot()
assert find(face.id).field_state("rotation")[1], "pink while the fix plays"
turn(b=0.0)
assert close(rot(), 0.0) and close(fix()["strength"], 0.0), ("one alone: no fix", rot())
assert close(bone("b")["angle"], bone("b")["setup_angle"]), "a fix never turns an action bone"
turn(a=0.5, b=0.5)
assert close(fix()["strength"], 0.25) and close(rot(), 7.5), (fix()["strength"], rot())

# A second pose, a half turned and b all the way: added (not near pose 1) on frame 20, starting from what
# the fix played there (half of it: 15 degrees).
turn(a=0.5, b=1.0)
assert close(rot(), 15.0), rot()
assert ops.anim.fix_action_new() == {'FINISHED'}
assert doc.action == name and doc.frame_current == 20 and len(fix()["poses"]) == 2, (doc.frame_current, fix())
assert close(rot(), 15.0), ("it starts from what played there", rot())
assert close(find(face.id).location_x, 1.0), "held at the new pose"
find(face.id).rotation = -10.0
doc.frame_current = 19
doc.frame_current = 20
# Frame 10 still holds pose 1 (and its correction).
doc.frame_current = 10
assert close(rot(), 30.0) and close(find(face.id).location_x, 2.0), (rot(), find(face.id).location_x)
ops.anim.action_set(name="")

# Each pose exact; between them a blend, by closeness.
doc.frame_current = 20
turn(a=0.5, b=1.0)
assert close(rot(), -10.0), rot()
turn(a=1.0, b=1.0)
assert close(rot(), 30.0), rot()
turn(a=0.75, b=1.0)
assert close(rot(), 10.0, 1e-2), ("half way between: half each", rot())
assert close(fix()["strength"], 1.0)
# Toward rest it fades out.
turn(a=0.25, b=0.5)
assert 0.0 < fix()["strength"] < 1.0 and abs(rot()) < 30.0
turn(a=0.0, b=0.5)
assert close(rot(), 0.0)

# Fixing a pose close to one it has (within 5% each) opens that one.
turn(a=0.52, b=0.98)
assert ops.anim.fix_action_new() == {'FINISHED'}
assert doc.frame_current == 20 and len(fix()["poses"]) == 2, "pose 2 opened, none added"
# Edit (no frame) opens the pose nearest the pose now.
ops.anim.action_set(name="")
turn(a=0.95, b=1.0)
assert ops.anim.action_set(name=name) == {'FINISHED'} and doc.frame_current == 10
assert ops.anim.action_set(name=name, frame="20") == {'FINISHED'} and doc.frame_current == 20
ops.anim.action_set(name="")

# The pose map adds a pose at given shares; a fix keeps at least one pose.
assert ops.anim.fix_pose_add(name=name, shares="0.3,0.3") == {'FINISHED'}
assert len(fix()["poses"]) == 3 and doc.frame_current == 30 and doc.action == name
assert ops.anim.fix_pose_remove(name=name, pose="3") == {'FINISHED'} and len(fix()["poses"]) == 2
ops.anim.action_set(name="")

# A third action bone: a fix for all three is keyed on top of the one for two of them.
doc.frame_current = 20
turn(a=1.0, b=1.0, c=1.0)
assert close(rot(), 30.0), "the pair's fix plays with c turned too"
assert ops.anim.fix_action_new() == {'FINISHED'}
triple = "a × b × c"
assert doc.action == triple and doc.frame_current == 10, doc.action
assert close(rot(), 30.0), ("the pair's fix plays under it while it's edited", rot())
find(face.id).rotation = 40.0
doc.frame_current = 9
doc.frame_current = 10
ops.anim.action_set(name="")
doc.frame_current = 20
turn(a=1.0, b=1.0, c=1.0)
assert close(rot(), 40.0), ("both add up", rot())
assert close(fix(triple)["strength"], 1.0)
turn(c=0.0)
assert close(rot(), 30.0) and close(fix(triple)["strength"], 0.0), ("c at rest: the triple out", rot())

# Choosing the combination: with all three turned, fix only a and b (pose 1 of theirs here).
turn(a=1.0, b=1.0, c=1.0)
assert ops.anim.fix_action_new(drivers="a,b") == {'FINISHED'}
assert doc.action == name and doc.frame_current == 10
ops.anim.action_set(name="")
# An action bone turned less than 5% doesn't count.
turn(a=1.0, b=1.0, c=0.02)
assert [p[0] for p in find(rig.id).action_bone_actions_playing if p[2] >= 0.05] == ["a", "b"]
assert ops.anim.fix_action_new() == {'FINISHED'} and doc.action == name, doc.action
ops.anim.action_set(name="")

# While a fix is edited its action bones are held: picking them lets them go again.
turn(a=1.0, b=1.0, c=1.0)
assert ops.anim.action_set(name=triple) == {'FINISHED'} and doc.frame_current == 10
layers.active = find(rig.id)
ops.armature.select_all()
assert not any(bone(n)["selected"] for n in ("a", "b", "c")), [bone(n)["selected"] for n in "abc"]
assert "held" in oanim.wm.banner_report()[3], oanim.wm.banner_report()
# Deleting a pose's keys removes the pose; the fix goes with its last one.
layers.active = find(face.id)
assert ops.anim.keyframe_delete() == {'FINISHED'}
assert doc.action == "" and all(f["name"] != triple for f in find(rig.id).fix_actions), doc.action
doc.frame_current = 20
turn(a=1.0, b=1.0, c=1.0)
assert close(rot(), 30.0), ("only the pair's fix now", rot())
ops.anim.action_set(name=name, frame="20")
layers.active = find(face.id)
assert ops.anim.keyframe_delete() == {'FINISHED'} and doc.action == name
assert [p["frame"] for p in fix(name)["poses"]] == [10], ("pose 2 removed with its keys", fix(name))
ops.ed.undo()
assert [p["frame"] for p in fix(name)["poses"]] == [10, 20], "undo brings it back"
ops.anim.action_set(name="")

# Saved with the file.
folder = pathlib.Path(tempfile.mkdtemp())
path = str(folder / "fix.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.type == "BONE")
face = next(l for l in layers.all() if l.type == "VECTOR")
assert [p["frame"] for p in fix(name)["poses"]] == [10, 20], fix(name)
doc.frame_current = 20
turn(a=0.5, b=1.0, c=0.0)
assert close(rot(), -10.0), "plays after opening"

# Saved before poses: one pose where it was made ("full"), on its last frame ("length").
with zipfile.ZipFile(path) as z:
    data = json.loads(z.read("document.json"))


def old_style(layers_json):
    for l in layers_json:
        for f in l.get("fix_actions", []):
            first = f.pop("poses")[0]
            f["full"], f["length"], f["combine"] = first["at"], first["frame"], "MULTIPLY"
        old_style(l.get("children", []))


old_style(data["document"]["layers"])
older = str(folder / "older.oanim")
with zipfile.ZipFile(older, "w") as z:
    z.writestr("document.json", json.dumps(data))
assert ops.wm.open_mainfile(filepath=older, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.type == "BONE")
face = next(l for l in layers.all() if l.type == "VECTOR")
assert [p["frame"] for p in fix(name)["poses"]] == [10], fix(name)
doc.frame_current = 20
turn(a=1.0, b=1.0, c=0.0)
assert close(rot(), 30.0), ("an older fix plays as it did", rot())

# Renaming a driver's bone renames its action bone action, and the fixes named after their drivers follow.
layers.active = rig
import _oanim
_oanim.layer_bone_set(rig.id, "a", {"name": "arm"})  # bone_set's own first argument is called name
assert fix("arm × b")["drivers"] == ["arm", "b"]
doc.frame_current = 21
doc.frame_current = 20
assert close(rot(), 30.0), "still plays"
assert ops.anim.action_rename(name="arm × b", to="Reach fix") == {'FINISHED'}
assert fix("Reach fix")

# Deleting a driver breaks it (kept, with its keys, saying why); it plays nothing.
assert ops.anim.action_delete(name="b") == {'FINISHED'}
assert fix("Reach fix")["broken"] == '"b" is gone'
assert close(rot(), 0.0), "broken: nothing"
assert ops.anim.action_delete(name="Reach fix") == {'FINISHED'}
assert all(f["name"] != "Reach fix" for f in find(rig.id).fix_actions)

print("fix actions: ok")
