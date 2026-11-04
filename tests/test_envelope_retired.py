"""Envelope Deform retired into Curve and Cage Deform's Envelope method (2026-11-03).

- New Curve and Cage Deforms bend by Envelope; Mesh and Follow Deform keep their methods.
- type="ENVELOPE_DEFORM" (Add Modifier and Change Type) gives Curve or Cage Deform with Envelope, by the shape, and a
  new shape makes an Envelope one whichever of the two takes it; Change Type between them keeps Envelope.
- Files: an ENVELOPE_DEFORM opens as Curve Deform on a line and Cage Deform on a loop, with Envelope and every setting
  it had, bending as before; a Cage Deform's "ENVELOPE" before format 5 is Outline (its old name), from format 5 the
  Envelope method; a Mesh Deform's "ENVELOPE" is Outline.

Run with:  OpenAnimate --background --python tests/test_envelope_retired.py
"""
import json
import math
import os
import tempfile
import zipfile

import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
ART = [(-0.25, 0.1), (-0.1, 0.12), (0.05, -0.08), (0.2, 0.05), (0.1, 0.3), (-0.15, -0.3)]


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


def art(name, kind, **settings):
    layer = new_layer(name)
    layer.add_curve(ART)
    assert ops.object.modifier_add(type=kind) == {'FINISHED'}
    if settings:
        layer.modifier_set(0, **settings)
    return layer


def mod(layer):
    return layer.modifiers[0]


ops.wm.read_homefile(confirmed="1")
doc.frame_current = 0
line = new_layer("Line")
line.add_curve([(-0.35, 0.0), (0.0, 0.06), (0.35, 0.0)])
loop = new_layer("Loop")
loop.add_curve([(-0.3, -0.3), (0.3, -0.3), (0.3, 0.3), (-0.3, 0.3)], closed=True)
LINE, LOOP = line.shapes[0][0], loop.shapes[0][0]

# --- New deforms' methods ------------------------------------------------------------------------------------------
curve = art("Curve", "CURVE_DEFORM", layer="Line", shape=LINE, precision=0.0)
cage = art("Cage", "CAGE_DEFORM", layer="Loop", shape=LOOP, precision=0.0)
assert mod(curve)["type"] == "CURVE_DEFORM" and mod(curve)["curve_method"] == "ENVELOPE", mod(curve)
assert mod(cage)["type"] == "CAGE_DEFORM" and mod(cage)["cage_method"] == "ENVELOPE", mod(cage)
assert mod(curve)["kind"] == mod(cage)["kind"] == "Envelope", (mod(curve), mod(cage))
mesh = art("Mesh", "MESH_DEFORM")
assert mod(mesh)["mesh_method"] == "CELLS", mod(mesh)
follow = art("Follow", "FOLLOW_DEFORM")
assert mod(follow)["cage_method"] == "OUTLINE" and mod(follow)["curve_method"] == "SMOOTH", mod(follow)

# --- ENVELOPE_DEFORM: Curve or Cage by the shape -------------------------------------------------------------------
env_loop = art("Env loop", "ENVELOPE_DEFORM", layer="Loop", shape=LOOP, precision=0.0)
env_line = art("Env line", "ENVELOPE_DEFORM", layer="Line", shape=LINE, precision=0.0)
assert mod(env_loop)["type"] == "CAGE_DEFORM" and mod(env_loop)["cage_method"] == "ENVELOPE", mod(env_loop)
assert mod(env_loop)["kind"] == "Envelope" and mod(env_loop)["name"] == "Cage Deform", mod(env_loop)
assert mod(env_line)["type"] == "CURVE_DEFORM" and mod(env_line)["curve_method"] == "ENVELOPE", mod(env_line)
# A new shape: the one of the two that takes it.
env_line.modifier_set(0, layer="Loop", shape=LOOP)
assert mod(env_line)["type"] == "CAGE_DEFORM" and mod(env_line)["cage_method"] == "ENVELOPE", mod(env_line)
env_line.modifier_set(0, layer="Line", shape=LINE)
assert mod(env_line)["type"] == "CURVE_DEFORM" and mod(env_line)["curve_method"] == "ENVELOPE", mod(env_line)
# A Smooth Curve Deform given a loop stays a Curve Deform (it wants a Part of it).
smooth = art("Smooth", "CURVE_DEFORM", curve_method="SMOOTH", layer="Loop", shape=LOOP)
assert mod(smooth)["type"] == "CURVE_DEFORM", mod(smooth)
# Change Type keeps Envelope across; type="ENVELOPE_DEFORM" makes a Mesh Deform the Envelope one that fits.
swap = art("Swap", "CAGE_DEFORM", cage_method="MEAN_VALUE", layer="Loop", shape=LOOP)
swap.modifier_set(0, type="CURVE_DEFORM")
assert mod(swap)["curve_method"] == "ENVELOPE", mod(swap)
swap.modifier_set(0, type="CAGE_DEFORM")
assert mod(swap)["cage_method"] == "ENVELOPE" and mod(swap)["name"] == "Cage Deform", mod(swap)
swap.modifier_set(0, cage_method="MEAN_VALUE", type="CURVE_DEFORM", curve_method="SMOOTH")
swap.modifier_set(0, type="CAGE_DEFORM")
assert mod(swap)["cage_method"] == "MEAN_VALUE", mod(swap)
swap.modifier_set(0, type="MESH_DEFORM")
assert mod(swap)["type"] == "MESH_DEFORM", mod(swap)
swap.modifier_set(0, type="ENVELOPE_DEFORM")
assert mod(swap)["type"] == "CAGE_DEFORM" and mod(swap)["cage_method"] == "ENVELOPE", mod(swap)
assert mod(swap)["kind"] == "Envelope", mod(swap)
try:
    swap.modifier_set(0, type="MIRROR_DEFORM")
    raise AssertionError("a bad type was taken")
except ValueError:
    pass

# --- Old files -----------------------------------------------------------------------------------------------------
for layer in (curve, cage, env_line, env_loop, mesh, follow, smooth, swap):
    layer.modifier_set(0, show_viewport=False)
weighted = art("Weighted", "CAGE_DEFORM", layer="Loop", shape=LOOP, envelope_method="BLENDED", pre_blend=0.5,
               post_blend=0.2, precision=0.0)
capsule = art("Capsule", "CURVE_DEFORM", layer="Line", shape=LINE, influence="CAPSULE", influence_left=0.05,
              influence_begin=0.08, influence_fade=0.04, precision=0.0)
outline = art("Outline", "CAGE_DEFORM", cage_method="OUTLINE", blend=0.3, layer="Loop", shape=LOOP, precision=0.0)
mesh_outline = art("Mesh outline", "MESH_DEFORM", mesh_method="OUTLINE", layer="Loop", shape=LOOP)
for layer in (curve, cage):
    layer.modifier_set(0, show_viewport=True)
doc.frame_current = 8
line.set_point(1, (0.05, 0.2))
line.set_point(2, (0.3, -0.1))
loop.set_point(1, (0.4, -0.25))
loop.set_point(2, (0.25, 0.4))
doc.frame_current = 9
doc.frame_current = 8
ENVELOPES = ("Curve", "Cage", "Weighted", "Capsule")
before = {l.name: l.evaluated()[0] for l in layers.all() if l.name in ENVELOPES + ("Outline",)}
for name in ENVELOPES + ("Outline",):
    rest = ART
    assert max(math.dist(p, q) for p, q in zip(before[name], rest)) > 0.01, (name, "bends nothing")

folder = tempfile.mkdtemp()
path = os.path.join(folder, "new.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
with zipfile.ZipFile(path) as z:
    data = json.loads(z.read("document.json"))
assert data["version"] >= 5, data["version"]


def modifier_json(name):
    def find(list_json):
        for l in list_json:
            if l.get("name") == name:
                return l["modifiers"][0]
            got = find(l.get("children", []))
            if got:
                return got
        return None
    return find(data["document"]["layers"])


# From format 5 a Cage Deform's "ENVELOPE" is the Envelope method.
assert modifier_json("Cage")["cage_method"] == "ENVELOPE" and modifier_json("Outline")["cage_method"] == "OUTLINE"
assert modifier_json("Mesh outline")["mesh_method"] == "OUTLINE"
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
doc.frame_current = 8
for name in ENVELOPES + ("Outline",):
    got = next(l for l in layers.all() if l.name == name).evaluated()[0]
    assert max(math.dist(p, q) for p, q in zip(got, before[name])) < 1e-6, (name, "saved and opened")

# As Envelope Deform saved them, in a format 4 file: no curve or cage method, type ENVELOPE_DEFORM; and Outline as
# "ENVELOPE", Mesh's Outline as "ENVELOPE".
data["version"] = 4
for name in ENVELOPES:
    mj = modifier_json(name)
    mj["type"] = "ENVELOPE_DEFORM"
    mj["name"] = "Envelope Deform"
    mj.pop("curve_method", None)
    mj.pop("cage_method", None)
    mj.pop("blend", None)
    mj.pop("range", None)
modifier_json("Outline")["cage_method"] = "ENVELOPE"
# Blended was Weighted then.
assert modifier_json("Weighted")["envelope_method"] == "BLENDED"
modifier_json("Weighted")["envelope_method"] = "WEIGHTED"
modifier_json("Mesh outline")["mesh_method"] = "ENVELOPE"
old = os.path.join(folder, "old.oanim")
with zipfile.ZipFile(old, "w") as z:
    z.writestr("document.json", json.dumps(data))
assert ops.wm.open_mainfile(filepath=old, confirmed="1") == {'FINISHED'}
doc.frame_current = 8
by_name = {l.name: l for l in layers.all()}
for name, kind in (("Curve", "CURVE_DEFORM"), ("Capsule", "CURVE_DEFORM"), ("Cage", "CAGE_DEFORM"),
                   ("Weighted", "CAGE_DEFORM")):
    m = mod(by_name[name])
    method = m["curve_method"] if kind == "CURVE_DEFORM" else m["cage_method"]
    assert m["type"] == kind and method == "ENVELOPE" and m["name"] == "Envelope Deform", (name, m)
    got = by_name[name].evaluated()[0]
    assert max(math.dist(p, q) for p, q in zip(got, before[name])) < 1e-6, (name, "opened from Envelope Deform")
m = mod(by_name["Weighted"])
assert m["envelope_method"] == "BLENDED" and abs(m["pre_blend"] - 0.5) < 1e-6 and abs(m["post_blend"] - 0.2) < 1e-6, m
m = mod(by_name["Capsule"])
assert m["influence"] == "CAPSULE" and abs(m["influence_left"] - 0.05) < 1e-6 and abs(m["influence_fade"] - 0.04) < 1e-6, m
m = mod(by_name["Outline"])
assert m["cage_method"] == "OUTLINE" and abs(m["blend"] - 0.3) < 1e-6, m
got = by_name["Outline"].evaluated()[0]
assert max(math.dist(p, q) for p, q in zip(got, before["Outline"])) < 1e-6, "Outline opened from format 4"
assert mod(by_name["Mesh outline"])["mesh_method"] == "OUTLINE", mod(by_name["Mesh outline"])

print("Envelope retired tests passed")
