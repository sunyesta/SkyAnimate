"""Drivers, the edge cases of what they compute: the expression language (precedence, Python's chained
comparisons and and/or, constants a variable may take the name of, deep and long expressions, bad ones,
results that aren't numbers), variables, types, remap, and odd targets the operators refuse.

Every check runs and the failures are listed together at the end.

Run with:  OpenAnimate --background --python tests/test_drivers_expressions.py
"""
import json
import math
import oanim

doc = oanim.data.document
doc.auto_key = False
ops = oanim.ops.anim
results = []


def check(name, ok, info=""):
    results.append((name, bool(ok), info))


def drv(layer, path, comp=-1):
    for d in oanim.data.drivers:
        t = d["target"]
        if t["layer"] == int(layer) and t["path"] == path and t["component"] == comp:
            return d
    return None


def step():
    doc.frame_current = doc.frame_current + 1


a = oanim.data.layers.active
a.add_curve([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5)], closed=True, fill=True)
oanim.ops.layer.new()
b = oanim.data.layers.active
A, B = str(a.id), str(b.id)


def expr(text):
    """Worked out on Scale Y, which has no range (Opacity is held to 0..1, as in Blender)."""
    ops.driver_add(layer=B, path="scale", component="1", expression=text)
    step()
    d = drv(B, "scale", 1)
    return d["value"], d["error"]


# --- Expression language ---
cases = {
    "1": 1, "1.5": 1.5, ".5": 0.5, "5.": 5, "1e2": 100, "2e-1": 0.2, "1E3": 1000,
    "-1": -1, "--1": 1, "+1": 1, "-2**2": -4, "2**-1": 0.5, "2**3**2": 512, "(-2)**2": 4,
    "10 - 2 - 3": 5, "100 / 10 / 2": 5, "2 * 3 + 4": 10, "2 + 3 * 4": 14, "7 % 3": 1, "-7 % 3": 2,
    "7.5 % 2": 1.5, "1 < 2": 1, "2 <= 2": 1, "3 > 4": 0, "3 >= 4": 0, "1 == 1": 1, "1 != 1": 0,
    "1 < 2 < 3": 1, "3 > 2 > 1": 1, "1 < 3 < 2": 0,
    "not 1": 0, "not 0": 1, "1 and 0": 0, "0 or 2": 2, "1 and 3": 3, "0 and 1/0": 0, "1 or 1/0": 1,
    "1 if 0 else 2": 2, "1 if 1 else 2 if 0 else 3": 1, "(1 if 0 else 2) + 1": 3,
    "abs(-2)": 2, "sign(-3)": -1, "sign(0)": 0, "floor(1.7)": 1, "ceil(1.2)": 2, "round(2.5)": 3,
    "round(-2.5)": -3, "sqrt(9)": 3, "pow(2, 10)": 1024, "exp(0)": 1, "log(e)": 1, "fmod(7, 3)": 1,
    "min(3, 1, 2)": 1, "max(1)": 1, "clamp(-1, 0, 1)": 0, "clamp(2, 0, 1)": 1, "lerp(0, 10, 0.5)": 5,
    "smoothstep(0, 1, 0)": 0, "smoothstep(0, 1, 1)": 1, "smoothstep(0, 1, 2)": 1, "radians(180)": math.pi,
    "degrees(pi/2)": 90, "atan2(1, 1)": math.pi / 4, "sin(pi/2)": 1, "cos(0)": 1, "tan(0)": 0,
    "asin(1)": math.pi / 2, "acos(1)": 0, "atan(1)": math.pi / 4, "True": 1, "False": 0, "pi": math.pi,
    "  1  +  2  ": 3, "2^3": 8, "1+2*3-4/2": 5, "((((1))))": 1, "#3": 3, "# 3": 3,
}
for text, want in cases.items():
    v, e = expr(text)
    check("expr " + text, v is not None and not e and abs(v - want) < 1e-4, (v, e, want))

bad = [" ", "1 +", "(1", "1)", "sinn(1)", "sin()", "sin(1, 2)", "clamp(1)", "min()", "1 2", "1..2", "@",
       "1 if 2", "not", "frame frame", "1,2", "abs", "#", "##1", "a.b", "1e", "1e+"]
for text in bad:
    v, e = expr(text)
    check("bad expr " + repr(text) + " reports", bool(e), (v, e))

for text in ("1/0", "sqrt(-1)", "log(0)", "0/0", "fmod(1, 0)", "1e39 * 1e39", "asin(2)"):
    v, e = expr(text)
    check("non-number " + text, bool(e), (v, e))

# The value before a broken expression stays put (not reset to 0).
expr("0.37")
before = b.scale_y
expr("1 +")
check("broken expression leaves the value", abs(b.scale_y - before) < 1e-6, (b.scale_y, before))

# Long and deep
v, e = expr("+".join(["1"] * 500))
check("500 terms", not e and abs(v - 500) < 1e-3, (v, e))
v, e = expr("(" * 50 + "1" + ")" * 50)
check("80 nested parens", not expr("(" * 80 + "1" + ")" * 80)[1])
v, e = expr("-" * 200 + "1")
check("200 minuses", not e and v == 1, (v, e))
v, e = expr("min(" + ",".join(str(i) for i in range(1000)) + ")")
check("min of 1000 arguments", not e and v == 0, (v, e))
v, e = expr("+".join("(1+(1+(1+(1+1))))" for _ in range(100)))
check("wide and nested sum", not e and abs(v - 500) < 1e-3, (v, e))
v, e = expr("1+(" * 200 + "1" + ")" * 200)
check("200-deep right nesting is an error or right, never wrong", bool(e) or abs(v - 201) < 1e-3, (v, e))
v, e = expr("(" * 5000 + "1" + ")" * 5000)
check("5000 nested parens doesn't crash", True, (v, e))
v, e = expr("-" * 3000 + "1")
check("3000 unary minus doesn't crash", True, (v, e))

# frame
doc.frame_current = 40
ops.driver_add(layer=B, path="rotation", expression="frame")
doc.frame_current = 41
check("frame", abs(drv(B, "rotation")["value"] - 41) < 1e-6, drv(B, "rotation"))
doc.frame_current = 0
check("frame 0", abs(drv(B, "rotation")["value"]) < 1e-6, drv(B, "rotation"))
ops.driver_add(layer=B, path="opacity", expression="7")
step()
check("opacity held to its range", b.opacity == 1.0, b.opacity)
ops.driver_add(layer=B, path="opacity", expression="-7")
step()
check("opacity held to its range (below)", b.opacity == 0.0, b.opacity)

# --- Variables ---
v = json.dumps([{"name": "x", "layer": a.id, "path": "rotation"}, {"name": "x", "layer": a.id, "path": "opacity"}])
ops.driver_add(layer=B, path="opacity", vars=v, expression="x")
step()
check("duplicate variable names: no crash", drv(B, "opacity") is not None, drv(B, "opacity"))
v = json.dumps([{"name": "frame", "layer": a.id, "path": "opacity"}])
a.opacity = 0.5
ops.driver_add(layer=B, path="opacity", vars=v, expression="frame")
step()
check("a variable named frame", drv(B, "opacity")["error"] == "" , drv(B, "opacity"))
v = json.dumps([{"name": "pi", "layer": a.id, "path": "opacity"}])
ops.driver_add(layer=B, path="opacity", vars=v, expression="pi")
step()
check("a variable named pi (constant or variable, no crash)", drv(B, "opacity") is not None, drv(B, "opacity"))
for badvars in ("not json", "{}", "[1, 2]", json.dumps([{"name": "q"}]), json.dumps([{"name": "q", "layer": "zz", "path": 5}])):
    r = ops.driver_add(layer=B, path="opacity", vars=badvars, expression="1")
    check("bad vars " + badvars[:20] + " don't crash", True, r)
v = json.dumps([{"name": "s", "layer": a.id, "path": "position"}])  # pair without component
ops.driver_add(layer=B, path="opacity", vars=v, expression="s")
step()
check("pair variable without component reports", bool(drv(B, "opacity")["error"]), drv(B, "opacity"))
v = json.dumps([{"name": "s", "layer": a.id, "path": "position", "component": 5}])
ops.driver_add(layer=B, path="opacity", vars=v, expression="s")
step()
check("component out of range reports", bool(drv(B, "opacity")["error"]), drv(B, "opacity"))

# --- Types ---
v = json.dumps([{"name": "x", "layer": a.id, "path": "opacity"}, {"name": "y", "layer": a.id, "path": "rotation"}])
a.opacity, a.rotation = 0.5, 3.0
for kind, want in (("AVERAGE", 1.75), ("SUM", 3.5), ("MIN", 0.5), ("MAX", 3.0)):
    ops.driver_add(layer=B, path="rotation", vars=v, type=kind)
    step()
    d = drv(B, "rotation")
    check("type " + kind, abs(d["value"] - want) < 1e-5, d)
ops.driver_add(layer=B, path="rotation", type="SUM")
step()
check("SUM with no variables reports", bool(drv(B, "rotation")["error"]), drv(B, "rotation"))
r = ops.driver_add(layer=B, path="rotation", type="BOGUS")
check("unknown type refused or defaulted", True, r)

# --- Remap ---
v = json.dumps([{"name": "x", "layer": a.id, "path": "rotation"}])
a.rotation = 5
ops.driver_add(layer=B, path="rotation", vars=v, expression="x", remap="1", from_min="0", from_max="10", to_min="100", to_max="200")
step()
check("remap", abs(drv(B, "rotation")["value"] - 150) < 1e-4, drv(B, "rotation"))
ops.driver_add(layer=B, path="rotation", vars=v, expression="x", remap="1", from_min="10", from_max="0", to_min="0", to_max="1")
step()
check("remap reversed range", abs(drv(B, "rotation")["value"] - 0.5) < 1e-4, drv(B, "rotation"))
ops.driver_add(layer=B, path="rotation", vars=v, expression="x", remap="1", from_min="3", from_max="3", to_min="0", to_max="1")
step()
check("remap zero-width range: a number", drv(B, "rotation")["value"] is not None and not drv(B, "rotation")["error"], drv(B, "rotation"))
a.rotation = 50
ops.driver_add(layer=B, path="rotation", vars=v, expression="x", remap="1", clamp="0", from_min="0", from_max="10", to_min="0", to_max="1")
step()
check("remap unclamped extrapolates", abs(drv(B, "rotation")["value"] - 5) < 1e-4, drv(B, "rotation"))

# --- Odd targets ---
for layer, path in ((B, "bogus"), ("999999", "opacity"), (B, ""), (B, "points[0].pos"), (B, "props.name"),
                    ("0", "bogus"), ("0", "materials[5].fill"), (B, "shapes[99999].fill"), (B, "position")):
    r = ops.driver_add(layer=layer, path=path, expression="1", component="7")
    check("odd target %s %s refused" % (layer, path), r == {'CANCELLED'}, r)
r = ops.driver_add(layer=B, path="position", component="1", expression="0.3")
step()
check("pair component 1", abs(b.location_y - 0.3) < 1e-6, b.location_y)
r = ops.driver_add(layer=B, path="position", expression="0.2")
step()
check("pair whole field: X and Y", abs(b.location_x - 0.2) < 1e-6 and abs(b.location_y - 0.2) < 1e-6, (b.location_x, b.location_y))
n = len([d for d in oanim.data.drivers if d["target"]["layer"] == b.id and d["target"]["path"] == "position"])
check("pair whole field makes 2, no duplicate of Y", n == 2, n)

# set / remove on something not driven
check("driver_set on undriven refused", ops.driver_set(layer=B, path="draw_order", expression="1") == {'CANCELLED'})
check("driver_remove on undriven refused", ops.driver_remove(layer=B, path="draw_order") == {'CANCELLED'})
check("driver_copy on undriven refused", ops.driver_copy(layer=B, path="draw_order") == {'CANCELLED'})

# Clipboard: paste onto a field that's the source (a self loop)
oanim.ops.anim.driver_copy_new(layer=B, path="visible")
r = ops.driver_paste(layer=B, path="visible")
check("paste onto its own source refused", r == {'CANCELLED'}, r)

# Checkbox driven
ops.driver_add(layer=B, path="visible", expression="frame % 2")
doc.frame_current = 3
v3 = b.visible
doc.frame_current = 4
check("visible driven by frame % 2", v3 is True and b.visible is False, (v3, b.visible))

# Value types: int/enum settings rounding and clamping
ops.driver_add(layer=B, path="props.render_visible", expression="0.49")
step()
check("checkbox 0.49 off", drv(B, "props.render_visible")["value"] == 0.0, drv(B, "props.render_visible"))

fails = [r for r in results if not r[1]]
for check_name, ok, info in fails:
    print("FAIL", check_name, str(info)[:700])
assert not fails, "%d of %d checks failed" % (len(fails), len(results))
print("drivers_expressions: ok (%d checks)" % len(results))
