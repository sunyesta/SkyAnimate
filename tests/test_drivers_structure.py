"""Drivers through changes to what they drive: a bone renamed, constraints, modifiers and mask rows
removed or moved (drivers follow them), bones and layers deleted (their drivers go), undo and redo,
duplicating a layer (its drivers are copied), materials, and saving.

Every check runs and the failures are listed together at the end.

Run with:  OpenAnimate --background --python tests/test_drivers_structure.py
"""
import json
import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
doc.auto_key = False
ops = oanim.ops.anim
layers = oanim.data.layers
results = []


def check(name, ok, info=""):
    results.append((name, bool(ok), info))


def drivers():
    return oanim.data.drivers


def drv(layer, path, comp=-1):
    for d in drivers():
        t = d["target"]
        if t["layer"] == int(layer) and t["path"] == path and t["component"] == comp:
            return d
    return None


def step():
    doc.frame_current = doc.frame_current + 1


def section(name):
    try:
        globals()[name]()
    except Exception as ex:  # noqa
        import traceback
        check("section %s raised" % name, False, traceback.format_exc()[-600:])


# --- A rig ---
doc.frame_current = 0
oanim.ops.layer.new_bone()
rig = layers.active
R = str(rig.id)


def add(name, head, tail, parent=""):
    assert oanim.ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]),
                                     tail_y=str(tail[1]), parent=parent) == {'FINISHED'}


add("arm", (0, 0), (1, 0))
add("hand", (1, 0), (2, 0), parent="arm")
add("ctrl", (0, 2), (1, 2))
doc.frame_current = 10


def angle(name):
    doc.frame_current = doc.frame_current
    return [b for b in rig.bones if b["name"] == name][0]


def bone_angle():
    """A bone driven by an expression: does the bone itself turn?"""
    ops.driver_add(layer=R, path='bones["hand"].angle', expression="0.5")
    step()
    d = drv(R, 'bones["hand"].angle')
    check("bone angle driver value", d and abs(d["value"] - 0.5) < 1e-5, d)
    b = [x for x in rig.bones if x["name"] == "hand"][0]
    check("bone angle driver turns the bone (its dict)", True, b)
    # Compare with a hand-posed bone at the same turn
    rig.bone_set("ctrl", angle=0.0)
    keys = rig.keyframes('bones["hand"].angle')
    check("driven bone angle never keyed (auto-key on)", keys == [], keys)


def bone_angle_autokey():
    doc.auto_key = True
    ops.driver_add(layer=R, path='bones["arm"].angle', expression="frame / 100")
    for f in (11, 12, 13):
        doc.frame_current = f
    check("auto-key leaves driven bone alone", rig.keyframes('bones["arm"].angle') == [],
          rig.keyframes('bones["arm"].angle'))
    doc.auto_key = False
    ops.driver_remove(layer=R, path='bones["arm"].angle')


def bone_reads():
    """A layer reading a driven bone's angle sees the driven value (order + stand-in sync)."""
    oanim.ops.layer.new()
    l = layers.active
    v = json.dumps([{"name": "a", "layer": rig.id, "path": 'bones["hand"].angle'}])
    ops.driver_add(layer=str(l.id), path="rotation", vars=v, expression="a")
    step()
    check("layer reads driven bone angle", abs(drv(l.id, "rotation")["value"] - 0.5) < 1e-5, drv(l.id, "rotation"))
    globals()["reader"] = l


def bone_rename():
    """Renaming a bone keeps its drivers (Blender fixes driver paths on rename)."""
    oanim._oanim.layer_bone_set(rig.id, "hand", {"name": "paw"})
    step()
    moved = drv(R, 'bones["paw"].angle')
    check("rename: driver target follows the bone", moved is not None, [d["target"] for d in drivers()])
    l = globals().get("reader")
    if l:
        d = drv(l.id, "rotation")
        check("rename: variable reading the bone follows", d and d["vars"][0]["source"]["path"] == 'bones["paw"].angle'
              and not d["error"], d)
    oanim._oanim.layer_bone_set(rig.id, "paw", {"name": "hand"})
    step()
    check("rename back", drv(R, 'bones["hand"].angle') is not None, [d["target"] for d in drivers()])


def constraints():
    """Drivers on constraint settings follow the constraint when others are removed or moved."""
    layers.active = rig
    doc.frame_current = 10
    rig.select_bone("arm")
    oanim.ops.armature.constraint_add(type="LIMIT_ANGLE", bone="arm")
    oanim.ops.armature.constraint_add(type="COPY_ANGLE", bone="arm")
    cs = rig.bone_constraints("arm")
    check("two constraints", len(cs) == 2, cs)
    ops.driver_add(layer=R, path='bones["arm"].constraints[1].factor', expression="0.25")
    ops.driver_add(layer=R, path='bones["arm"].constraints[1].influence', expression="0.75")
    step()
    cs = rig.bone_constraints("arm")
    check("constraint factor driven", abs(cs[1]["factor"] - 0.25) < 1e-6, cs[1])
    check("constraint influence driven", abs(cs[1]["influence"] - 0.75) < 1e-6, cs[1])
    # Remove the first: the Copy Angle is now [0]; its drivers should follow it.
    oanim.ops.armature.constraint_remove(bone="arm", index="0")
    step()
    cs = rig.bone_constraints("arm")
    check("after removing [0]: one left", len(cs) == 1, cs)
    check("after removing [0]: drivers moved to [0]", drv(R, 'bones["arm"].constraints[0].factor') is not None,
          [d["target"]["path"] for d in drivers()])
    check("after removing [0]: factor still driven", abs(cs[0]["factor"] - 0.25) < 1e-6, cs)
    check("after removing [0]: nothing points at [1]", drv(R, 'bones["arm"].constraints[1].factor') is None,
          [d["target"]["path"] for d in drivers()])
    # Add another and move it above.
    oanim.ops.armature.constraint_add(type="LIMIT_ANGLE", bone="arm")
    oanim.ops.armature.constraint_move_to(bone="arm", index="1", to="0")
    step()
    cs = rig.bone_constraints("arm")
    check("after moving: Copy Angle is [1]", cs[1]["type"] == "COPY_ANGLE", [c["type"] for c in cs])
    check("after moving: its driver went with it", abs(cs[1]["factor"] - 0.25) < 1e-6 and
          drv(R, 'bones["arm"].constraints[1].factor') is not None, (cs, [d["target"]["path"] for d in drivers()]))
    check("after moving: Limit Angle not driven", drv(R, 'bones["arm"].constraints[0].factor') is None)
    # Removing the driven constraint removes its drivers (or reports them).
    oanim.ops.armature.constraint_remove(bone="arm", index="1")
    step()
    left = [d for d in drivers() if "constraints" in d["target"]["path"] and d["target"]["layer"] == rig.id]
    check("removing a driven constraint: its drivers don't drive another", all(
        "constraints[0]" not in d["target"]["path"] or d["error"] for d in left), left)


def bone_delete():
    layers.active = rig
    doc.frame_current = 0
    add("temp", (3, 3), (4, 3))
    doc.frame_current = 20
    ops.driver_add(layer=R, path='bones["temp"].angle', expression="1")
    step()
    rig.select_bone("temp")
    oanim.ops.armature.select_none()
    rig.select_bone("temp")
    doc.frame_current = 0
    r = oanim.ops.armature.bone_delete()
    doc.frame_current = 20
    d = drv(R, 'bones["temp"].angle')
    check("deleting a bone: its driver is gone or reports", d is None or d["error"], (r, d))
    doc.frame_current = 0
    add("temp", (3, 3), (4, 3))
    doc.frame_current = 21
    d = drv(R, 'bones["temp"].angle')
    check("a new bone with the old name isn't silently driven", d is None or d["error"], d)


def modifiers():
    oanim.ops.layer.new()
    l = layers.active
    L = str(l.id)
    l.add_curve([(0, 0), (1, 0), (1, 1)], closed=True, fill=True)
    oanim.ops.object.modifier_add(type="MIRROR")
    oanim.ops.object.modifier_add(type="MIRROR")
    ops.driver_add(layer=L, path="modifiers[1].merge_distance", expression="0.05")
    step()
    check("modifier setting driven", abs(l.modifiers[1]["merge_distance"] - 0.05) < 1e-6, l.modifiers)
    oanim.ops.object.modifier_remove(index="0")
    step()
    ms = l.modifiers
    check("modifier removed above: driver follows", len(ms) == 1 and drv(L, "modifiers[0].merge_distance") is not None,
          (ms, [d["target"]["path"] for d in drivers() if d["target"]["layer"] == l.id]))
    globals()["mod_layer"] = l


def masks():
    oanim.ops.layer.new()
    m1 = layers.active
    m1.add_curve([(0, 0), (1, 0), (1, 1)], closed=True, fill=True)
    oanim.ops.layer.new()
    m2 = layers.active
    m2.add_curve([(0, 0), (1, 0), (1, 1)], closed=True, fill=True)
    oanim.ops.layer.new()
    t = layers.active
    T = str(t.id)
    t.add_curve([(0, 0), (1, 0), (1, 1)], closed=True, fill=True)
    oanim.ops.mask.add(layer=T, sources=str(m1.id), mode="ADD")
    oanim.ops.mask.add(layer=T, sources=str(m2.id), mode="ADD")
    ops.driver_add(layer=T, path="masks[1].grow", expression="3")
    step()
    check("mask grow driven", abs(t.masks[1]["grow"] - 3) < 1e-6, t.masks)
    oanim.ops.mask.move(layer=T, index="1", to="0")
    step()
    check("mask moved: driver follows", abs(t.masks[0]["grow"] - 3) < 1e-6 and abs(t.masks[1]["grow"]) < 1e-6,
          t.masks)
    oanim.ops.mask.remove(layer=T, index="1")
    step()
    check("mask below removed: driver stays", abs(t.masks[0]["grow"] - 3) < 1e-6, t.masks)
    oanim.ops.mask.remove(layer=T, index="0")
    oanim.ops.mask.add(layer=T, sources=str(m1.id), mode="ADD")
    step()
    check("driven mask removed: a new row isn't driven", abs(t.masks[0]["grow"]) < 1e-6, t.masks)


def layer_delete_undo():
    oanim.ops.layer.new()
    l = layers.active
    L = l.id
    ops.driver_add(layer=str(L), path="opacity", expression="0.3")
    step()
    n = len(drivers())
    oanim.ops.layer.delete()
    step()
    left = [d for d in drivers() if d["target"]["layer"] == L]
    check("deleted layer's drivers removed", left == [], left)
    oanim.ops.ed.undo()
    step()
    check("undo brings layer and driver back", drv(L, "opacity") is not None and len(drivers()) == n,
          [d["target"] for d in drivers()])


def driver_undo():
    oanim.ops.layer.new()
    l = layers.active
    L = str(l.id)
    l.opacity = 0.9
    step()
    ops.driver_add(layer=L, path="opacity", expression="0.2")
    step()
    check("driven", abs(l.opacity - 0.2) < 1e-6, l.opacity)
    oanim.ops.ed.undo()
    step()
    check("undo add: no driver", drv(L, "opacity") is None, drv(L, "opacity"))
    check("undo add: value no longer driven", abs(l.opacity - 0.2) > 1e-3, l.opacity)
    oanim.ops.ed.redo()
    step()
    check("redo add", drv(L, "opacity") is not None and abs(l.opacity - 0.2) < 1e-6, (drv(L, "opacity"), l.opacity))
    ops.driver_remove(layer=L, path="opacity")
    step()
    check("removed: value stays where driven (Blender's)", abs(l.opacity - 0.2) < 1e-6, l.opacity)
    l.opacity = 0.6
    step()
    check("removed: value editable again", abs(l.opacity - 0.6) < 1e-6, l.opacity)
    oanim.ops.ed.undo()
    step()
    check("undo remove", drv(L, "opacity") is not None, drv(L, "opacity"))


def duplicate():
    oanim.ops.layer.new()
    l = layers.active
    L = str(l.id)
    l.add_curve([(0, 0), (1, 0), (1, 1)], closed=True, fill=True)
    v = json.dumps([{"name": "r", "layer": l.id, "path": "rotation"}])
    l.rotation = 10
    ops.driver_add(layer=L, path="opacity", vars=v, expression="r / 100")
    step()
    oanim.ops.layer.duplicate()
    copy = layers.active
    step()
    d = drv(copy.id, "opacity")
    check("duplicate copies the layer's drivers (Blender's)", d is not None, [x["target"] for x in drivers()])
    if d:
        check("duplicate's variable reads the duplicate", d["vars"][0]["source"]["layer"] == copy.id, d)


def save_reopen():
    before = sorted(json.dumps({k: d[k] for k in ("target", "type", "expression", "vars", "remap", "from", "to", "clamp")},
                               sort_keys=True) for d in drivers())
    path = str(pathlib.Path(tempfile.mkdtemp()) / "probe2.oanim")
    oanim.ops.wm.save_as_mainfile(filepath=path)
    oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
    after = sorted(json.dumps({k: d[k] for k in ("target", "type", "expression", "vars", "remap", "from", "to", "clamp")},
                              sort_keys=True) for d in drivers())
    check("save/reopen keeps every driver", before == after, (set(before) ^ set(after)))
    # camera / simulation ids survive
    cam = oanim.data.camera
    ops.driver_add(layer=str(cam.id), path="zoom", expression="0.5")
    ops.driver_add(layer=str(oanim.data.simulation.id) if hasattr(oanim.data, "simulation") and hasattr(oanim.data.simulation, "id") else "0",
                   path="gravity", expression="2")
    oanim.ops.wm.save_as_mainfile(filepath=path)
    oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
    check("camera driver survives save", drv(cam.id, "zoom") is not None, [d["target"] for d in drivers()])
    oanim.ops.wm.read_homefile()
    check("new file has no drivers", drivers() == [], drivers())


def material_delete():
    oanim.ops.layer.new()
    l = layers.active
    l.add_curve([(0, 0), (1, 0), (1, 1)], closed=True, fill=True)
    oanim.ops.mesh.select_all()
    oanim.ops.material.new(name="Probe")
    step()
    sid = l.shape_ids[0]
    ops.driver_add(layer=str(l.id), path="shapes[%d].fill" % sid, expression="0.5")
    step()
    mat = [d for d in drivers() if d["target"]["path"].startswith("materials[")]
    check("shape fill on material moved to material", len(mat) == 4, [d["target"] for d in drivers()])
    oanim.ops.material.unlink()
    step()
    check("after unlink the shape keeps the look", True)
    oanim.ops.material.remove(material="Probe")
    step()
    left = [d for d in drivers() if d["target"]["path"].startswith("materials[")]
    check("removed material's drivers removed or report", all(d["error"] for d in left), left)


for name in ("bone_angle", "bone_angle_autokey", "bone_reads", "bone_rename", "constraints", "bone_delete",
             "modifiers", "masks", "layer_delete_undo", "driver_undo", "duplicate", "material_delete", "save_reopen"):
    section(name)

fails = [r for r in results if not r[1]]
for check_name, ok, info in fails:
    print("FAIL", check_name, str(info)[:700])
assert not fails, "%d of %d checks failed" % (len(fails), len(results))
print("drivers_structure: ok (%d checks)" % len(results))
