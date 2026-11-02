"""Drivers on a rig: driven bones really turn and move, the setup frame (0) stays at rest (no driven pose
leaks into the setup), keys come back when a driver goes, a driven action bone plays its action bone action at
once, loops and long chains, references, the camera, the simulation, and speed.

Every check runs and the failures are listed together at the end.

Run with:  OpenAnimate --background --python tests/test_drivers_rig.py
"""
import json
import math
import time
import traceback

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


def sync():
    doc.frame_current = doc.frame_current


def section(fn):
    try:
        fn()
    except Exception:  # noqa
        check("section %s raised" % fn.__name__, False, traceback.format_exc()[-700:])


doc.frame_current = 0
oanim.ops.layer.new_bone()
rig = layers.active
R = str(rig.id)
oanim.ops.armature.bone_add(name="dial", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
oanim.ops.armature.bone_add(name="arm", head_x="0", head_y="2", tail_x="1", tail_y="2", parent="")
oanim.ops.layer.new()
face = layers.active
face.add_curve([(0, 0), (1, 0), (1, 1)], closed=True, fill=True)
oanim.ops.layer.move_to_group(group=str(rig.id))
layers.active = rig


def bone(name):
    sync()
    return [b for b in rig.bones if b["name"] == name][0]


def direction(b):
    return math.atan2(b["tail"][1] - b["head"][1], b["tail"][0] - b["head"][0])


def bones_really_move():
    doc.frame_current = 10
    rest = direction(bone("arm"))
    ops.driver_add(layer=R, path='bones["arm"].angle', expression="0.5")
    doc.frame_current = 11
    turned = direction(bone("arm"))
    check("driven bone angle turns the bone", abs((turned - rest) - 0.5) < 1e-3, (rest, turned))
    # The artwork bound to it follows: weights or parent? Only check no crash.
    ops.driver_add(layer=R, path='bones["arm"].position', component="0", expression="0.25")
    doc.frame_current = 12
    b = bone("arm")
    check("driven bone position moves the head", abs(b["head"][0] - 0.25) < 1e-3 or abs(b["head"][0]) > 1e-3, b)
    ops.driver_remove(layer=R, path='bones["arm"].position', component="0")
    ops.driver_remove(layer=R, path='bones["arm"].angle')
    doc.frame_current = 13


def setup_frame():
    """Driving a bone doesn't move its setup pose, even when frame 0 is shown."""
    doc.frame_current = 0
    rest = 0.0  # the arm was added lying along x
    ops.driver_add(layer=R, path='bones["arm"].angle', expression="0.7")
    doc.frame_current = 0
    doc.frame_current = 5
    doc.frame_current = 0
    ops.driver_remove(layer=R, path='bones["arm"].angle')
    doc.frame_current = 7
    doc.frame_current = 0
    after = direction(bone("arm"))
    check("setup pose unchanged by a driver shown on frame 0", abs(after - rest) < 1e-3, (rest, after))
    doc.frame_current = 9
    check("other frames back to rest after removing", abs(direction(bone("arm")) - rest) < 1e-3, direction(bone("arm")))


def keys_then_driver():
    oanim.ops.layer.new()
    l = layers.active
    L = str(l.id)
    doc.frame_current = 1
    l.opacity = 0.2
    l.keyframe_insert("opacity")
    doc.frame_current = 20
    l.opacity = 0.8
    l.keyframe_insert("opacity")
    keys_before = l.keyframes("opacity")
    ops.driver_add(layer=L, path="opacity", expression="0.5")
    for f in (1, 10, 20):
        doc.frame_current = f
        check("driver overrides keys at %d" % f, abs(l.opacity - 0.5) < 1e-6, l.opacity)
    doc.auto_key = True
    doc.frame_current = 15
    doc.frame_current = 16
    doc.auto_key = False
    check("keys untouched while driven", l.keyframes("opacity") == keys_before, (keys_before, l.keyframes("opacity")))
    ops.driver_remove(layer=L, path="opacity")
    doc.frame_current = 1
    check("keys back after removing", abs(l.opacity - 0.2) < 1e-6, l.opacity)
    doc.frame_current = 20
    check("keys back after removing (20)", abs(l.opacity - 0.8) < 1e-6, l.opacity)
    # Keying a driven channel by hand: I on a driven value (Blender keys the driven value).
    ops.driver_add(layer=L, path="opacity", expression="0.4")
    doc.frame_current = 30
    r = oanim.ops.anim.keyframe_toggle(layer=L, paths="opacity")
    check("keying a driven channel doesn't crash", True, r)
    ops.driver_remove(layer=L, path="opacity")


def action_bone_action():
    layers.active = rig
    rig.select_bone("dial")
    doc.frame_current = 0
    doc.auto_key = True
    assert oanim.ops.anim.action_new(name="dial", action_bone="1") == {'FINISHED'}
    doc.frame_current = 10
    rig.bone_set("dial", angle=1.0)
    face.location_x = 2.0
    doc.frame_current = 0
    assert oanim.ops.anim.action_set(name="") == {'FINISHED'}
    doc.auto_key = False
    doc.frame_current = 20
    ops.driver_add(layer=R, path='bones["dial"].angle', expression="1.0")
    doc.frame_current = 21
    check("a driven action bone drives its action bone action", abs(face.location_x - 2.0) < 1e-3, face.location_x)
    ops.driver_set(layer=R, path='bones["dial"].angle', expression="0.5")
    sync()
    sync()
    check("changing it on the same frame follows", abs(face.location_x - 1.0) < 1e-3, face.location_x)
    ops.driver_remove(layer=R, path='bones["dial"].angle')
    doc.frame_current = 22


def loops_and_chains():
    oanim.ops.layer.new()
    ls = []
    for i in range(3):
        oanim.ops.layer.new()
        ls.append(layers.active)
    a, b, c = ls
    va = json.dumps([{"name": "v", "layer": b.id, "path": "rotation"}])
    vb = json.dumps([{"name": "v", "layer": c.id, "path": "rotation"}])
    vc = json.dumps([{"name": "v", "layer": a.id, "path": "rotation"}])
    ops.driver_add(layer=str(a.id), path="rotation", vars=va, expression="v + 1")
    ops.driver_add(layer=str(b.id), path="rotation", vars=vb, expression="v + 1")
    ops.driver_add(layer=str(c.id), path="rotation", vars=vc, expression="v + 1")
    sync()
    errs = [drv(x.id, "rotation")["error"] for x in ls]
    check("3-cycle all report a loop", all("Loop" in e for e in errs), errs)
    ops.driver_remove(layer=str(c.id), path="rotation")
    c.rotation = 10
    sync()
    check("chain resolves in order (a = c + 2)", abs(a.rotation - 12) < 1e-4, (a.rotation, b.rotation))
    # Self loop through a variable
    v = json.dumps([{"name": "v", "layer": c.id, "path": "rotation"}])
    ops.driver_add(layer=str(c.id), path="rotation", vars=v, expression="v + 1")
    sync()
    check("self loop reports", "Loop" in drv(c.id, "rotation")["error"], drv(c.id, "rotation"))
    ops.driver_remove(layer=str(c.id), path="rotation")
    # A long chain in reverse order of creation
    chain = []
    for i in range(60):
        oanim.ops.layer.new()
        chain.append(layers.active)
    for i in range(59, 0, -1):
        v = json.dumps([{"name": "v", "layer": chain[i - 1].id, "path": "opacity"}])
        ops.driver_add(layer=str(chain[i].id), path="opacity", vars=v, expression="v * 0.99")
    chain[0].opacity = 1.0
    sync()
    check("60-long chain resolves in one evaluation", abs(chain[-1].opacity - 0.99 ** 59) < 1e-4, chain[-1].opacity)
    # Partial components: X of a driven by Y of b, Y of b driven by X of a (no real loop per component)
    va = json.dumps([{"name": "v", "layer": b.id, "path": "position", "component": 1}])
    vb = json.dumps([{"name": "v", "layer": a.id, "path": "position", "component": 0}])
    ops.driver_add(layer=str(a.id), path="position", component="0", vars=va, expression="v + 1")
    ops.driver_add(layer=str(b.id), path="position", component="1", vars=vb, expression="v + 1")
    sync()
    check("component cross reference is a loop", "Loop" in drv(a.id, "position", 0)["error"], drv(a.id, "position", 0))


def reference_layer():
    oanim.ops.layer.new()
    orig = layers.active
    orig.add_curve([(0, 0), (1, 0), (1, 1)], closed=True, fill=True)
    r = oanim.ops.layer.make_reference()
    ref = layers.active
    check("made a reference", ref.id != orig.id, r)
    ops.driver_add(layer=str(ref.id), path="opacity", expression="0.3")
    sync()
    sync()
    check("driven reference opacity holds", abs(ref.opacity - 0.3) < 1e-6, ref.opacity)
    ops.driver_add(layer=str(orig.id), path="opacity", expression="0.6")
    sync()
    sync()
    check("driven original opacity holds", abs(orig.opacity - 0.6) < 1e-6, orig.opacity)
    check("reference keeps its own driven value", abs(ref.opacity - 0.3) < 1e-6, ref.opacity)


def camera_and_simulation():
    cam = oanim.data.camera
    ops.driver_add(layer=str(cam.id), path="zoom", expression="1")
    sync()
    check("camera zoom driver (log2 1 = 2x)", abs(cam.zoom - 2.0) < 1e-3, cam.zoom)
    sim = oanim.data.simulation
    sid = None
    for d in drivers():
        pass
    try:
        sid = sim.id
    except Exception:
        sid = None
    if sid is not None:
        ops.driver_add(layer=str(sid), path="gravity", expression="3")
        sync()
        check("simulation gravity driven", abs(doc.gravity - 3) < 1e-4, doc.gravity)


def speed():
    oanim.ops.layer.new()
    base = layers.active
    many = []
    for i in range(300):
        oanim.ops.layer.new()
        many.append(layers.active)
    for l in many:
        v = json.dumps([{"name": "r", "layer": base.id, "path": "rotation"}])
        ops.driver_add(layer=str(l.id), path="rotation", vars=v, expression="sin(r + frame / 10) * 2 + r ** 2 / 3")
    sync()
    t = time.perf_counter()
    for f in range(30):
        doc.frame_current = 100 + f
    dt = (time.perf_counter() - t) / 30 * 1000
    check("300+ drivers: frame change under 100 ms (was %.1f ms)" % dt, dt < 100, dt)


for fn in (bones_really_move, setup_frame, keys_then_driver, action_bone_action, loops_and_chains, reference_layer,
           camera_and_simulation, speed):
    section(fn)

fails = [r for r in results if not r[1]]
for check_name, ok, info in fails:
    print("FAIL", check_name, str(info)[:700])
assert not fails, "%d of %d checks failed" % (len(fails), len(results))
print("drivers_rig: ok (%d checks)" % len(results))
