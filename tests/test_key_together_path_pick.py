"""A folded (Key Together) group's stretch picked with the Motion Path tool: Alt click in the group's row, or a click on
the path of any layer inside it, picks the group's stretch (teal in the timeline, lit on every path in the group), and a
timing edit then reaches every layer's key there (a filler retimed becoming the layer's own). An open group's layers keep
their own stretches. Nested folded groups use the outermost.

Run with:  OpenAnimate --background --python tests/test_key_together_path_pick.py
"""

import oanim

ops, layers, wm = oanim.ops, oanim.data.layers, oanim.wm


def vec(name, y):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    layer.add_curve([(0.0, y), (1.0, y)])
    return layer


def scene(fold=True, nest=False):
    """G holds A (keys 10, 20) and B (keys 10, 30), folded; A and B traced, the tool on, on frame 15."""
    assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
    doc = oanim.data.document
    doc.auto_key = True
    doc.auto_freeze_keys = False
    doc.path_all = True
    a = vec("A", 0.0)
    b = vec("B", 3.0)
    for f, x in ((10, 1.0), (20, 2.0)):
        doc.frame_current = f
        a.location_x = x
    for f, x in ((10, 0.5), (30, 1.5)):
        doc.frame_current = f
        b.location_x = x
    doc.frame_current = 0
    for l in layers.all():
        l.selected = l.id in (a.id, b.id)
    ops.layer.group()
    g = layers.active
    g.name = "G"
    if nest:
        for l in layers.all():
            l.selected = l.id == g.id
        ops.layer.group()
        g = layers.active
        g.name = "Outer"
    if fold:
        assert ops.anim.timeline_fold_together(layer=str(g.id), folded="1") == {'FINISHED'}
    doc.frame_current = 1
    doc.frame_current = 0
    for l in layers.all():
        l.selected = l.id in (a.id, b.id)
    layers.active = a
    doc.frame_current = 15
    assert ops.wm.tool_motion_path() == {'FINISHED'}
    return doc, a, b, g


def rows():
    return wm.timeline_row_details(True)


def row(label):
    return next(i for i, r in enumerate(rows()) if r["label"] == label)


def teal():
    return {r["label"]: r["teal"] for r in rows() if r["teal"]}


def key(layer, frame):
    return next(k for k in layer.keyframes("position") if k["frame"] == frame)


def lit(layer):
    """The frames lit on `layer`'s path by the dots picked (Timing Current lights them and the frame shown's stretch:
    looked at from past every key, so only the picks')."""
    doc = oanim.data.document
    shown = doc.frame_current
    doc.frame_current = 35
    try:
        for p in wm.motion_paths():
            if p["layer"] == layer.id:
                cols = p["dot_colors"]
                return [p["first"] + i for i, c in enumerate(cols) if c != cols[0]]
    finally:
        doc.frame_current = shown
    raise AssertionError("no path for " + layer.name)


def progress(layer, frame):
    """How far through its 10-20 stretch `layer` is on `frame`."""
    doc = oanim.data.document
    shown = doc.frame_current
    at = {}
    for f in (10, 20, frame):
        doc.frame_current = f
        at[f] = layer.location_x
    doc.frame_current = shown
    return (at[frame] - at[10]) / (at[20] - at[10])


def together(g):
    h = wm.timeline_key_header()
    return h["mode"], h["span"], h["targets"], (g.id, 0, False, 0)


# --- Alt click in the folded group's row picks the group's stretch, lit on every path -----------------------------------
doc, a, b, g = scene()
assert lit(a) == [] and lit(b) == []
assert wm.timeline_row_alt_click(row("G"), 15)
mode, span, targets, gt = together(g)
assert mode == "PATH" and span == (10, 20) and targets == [gt], (mode, span, targets)
assert teal() == {"G": (10, 20)}, teal()
assert lit(a) == list(range(11, 20)) and lit(b) == list(range(11, 20)), (lit(a), lit(b))
# The next stretch: the group's 20-30, though A has no key of its own on 30.
assert wm.timeline_row_alt_click(row("G"), 25)
assert teal() == {"G": (20, 30)}, teal()
assert lit(a) == list(range(21, 30)) and lit(b) == list(range(21, 30)), (lit(a), lit(b))
# On a key: the key, on every layer.
assert wm.timeline_row_alt_click(row("G"), 20)
assert key(a, 20)["selected"] and key(b, 20)["selected"]

# --- A click on a dot of A's path picks the group's stretch; Linear reaches B's filler, now B's own key ------------------
doc, a, b, g = scene()
assert key(b, 20).get("filler")
assert ops.anim.motion_path_select(frame="15", layer=str(a.id)) == {'FINISHED'}
mode, span, targets, gt = together(g)
assert mode == "PATH" and span == (10, 20) and targets == [gt], (mode, span, targets)
assert teal() == {"G": (10, 20)}, teal()
assert lit(b) == list(range(11, 20)), "lit on B's path too"
assert wm.timeline_key_header_call("anim.keys_interpolation_set", interp="LINEAR")
assert key(a, 20)["interp"] == "LINEAR" and key(b, 20)["interp"] == "LINEAR", (key(a, 20), key(b, 20))
assert not key(b, 20).get("filler"), "retimed, the filler is B's own"
assert key(a, 10)["interp"] == "SMOOTH" and key(b, 30)["interp"] == "SMOOTH", "only that stretch"

# --- A click on a key of B's path picks the group's key: every layer's on that frame -------------------------------------
doc, a, b, g = scene()
assert ops.anim.motion_path_select(frame="30", layer=str(b.id)) == {'FINISHED'}
assert key(a, 30)["selected"] and key(b, 30)["selected"]
assert not key(a, 20)["selected"] and not key(b, 20)["selected"]
assert teal() == {"G": (20, 30)}, teal()

# --- The timing tools on A's path retime the group: speeds, shapes, Alt S, key types, the timing graph -------------------
for name, edit, want in (
        ("even", lambda a, g: ops.anim.motion_path_even_spacing(frame="15", layer=str(a.id)), ("CUSTOM", None)),
        ("shape", lambda a, g: ops.anim.motion_path_stretch_shape(frame="15", layer=str(a.id), shape="EASE_IN"),
         ("CUSTOM", None)),
        ("alt_s", lambda a, g: ops.anim.motion_path_scale_spacing(frame="15", layer=str(a.id), factor="0.5"),
         ("CUSTOM", None)),
        ("dot_speed", lambda a, g: ops.anim.motion_path_dot_speed(frame="15", layer=str(a.id), speed="2"),
         ("CUSTOM", None)),
        ("graph", lambda a, g: ops.anim.timing_point_add(layer=str(a.id), frame="15", x="0.5", y="0.3"),
         ("CUSTOM", None)),
        ("sculpt", lambda a, g: ops.anim.timing_sculpt(layer=str(a.id), frame="15", brush="GRAB", path="12,0.2;13,0.6",
                                                       radius="3", strength="1"), ("CUSTOM", None)),
        ("breakdown", lambda a, g: ops.anim.motion_path_key_interp(frame="20", layer=str(a.id), type="BREAKDOWN"),
         (None, "BREAKDOWN"))):
    doc, a, b, g = scene()
    assert ops.anim.motion_path_select(frame="15", layer=str(a.id)) == {'FINISHED'}
    assert edit(a, g) == {'FINISHED'}, name
    for l in (a, b):
        k = key(l, 20)
        assert want[0] is None or k["interp"] == want[0], (name, l.name, k["interp"])
        assert want[1] is None or k["type"] == want[1], (name, l.name, k["type"])
    if want[0] == "CUSTOM":
        timed = [[round(progress(l, f), 4) for f in (12, 15, 18)] for l in (a, b)]
        assert timed[0] == timed[1], (name, "timed alike", timed)

# --- Without the tool, Alt click in the group's row selects the stretch's end key on every layer -------------------------
doc, a, b, g = scene()
assert ops.wm.tool_select() == {'FINISHED'}
assert wm.timeline_row_alt_click(row("G"), 15)
assert key(a, 20)["selected"] and key(b, 20)["selected"]

# --- Edit Layer on: a click on A's path still picks the group's stretch --------------------------------------------------
doc, a, b, g = scene()
assert ops.anim.motion_path_brush_set(edit_layer="1") == {'FINISHED'}
assert ops.anim.motion_path_select(frame="15", layer=str(a.id)) == {'FINISHED'}
mode, span, targets, gt = together(g)
assert mode == "PATH" and span == (10, 20) and targets == [gt], (mode, span, targets)
assert [t["label"] for t in wm.motion_path_timing()] == ["G together"], "the timing shown is the group's"
assert wm.timeline_key_header_call("anim.keys_interpolation_set", interp="LINEAR")
assert key(a, 20)["interp"] == "LINEAR" and key(b, 20)["interp"] == "LINEAR"
assert ops.anim.motion_path_select(frame="30", layer=str(b.id)) == {'FINISHED'}
assert key(a, 30)["selected"] and teal() == {"G": (20, 30)}, teal()

# --- Nested: the outermost folded group's stretch ------------------------------------------------------------------------
doc, a, b, outer = scene(nest=True)
assert ops.anim.motion_path_select(frame="15", layer=str(a.id)) == {'FINISHED'}
mode, span, targets, gt = together(outer)
assert targets == [gt] and span == (10, 20), (targets, span)
assert teal() == {"Outer": (10, 20)}, teal()

# --- An open group: A's stretch is its own, B untouched ------------------------------------------------------------------
doc, a, b, g = scene(fold=False)
assert ops.anim.motion_path_select(frame="15", layer=str(a.id)) == {'FINISHED'}
h = wm.timeline_key_header()
assert h["targets"] == [(a.id, 0, False, 0)], h["targets"]
assert lit(b) == [], "B's path not lit"
assert wm.timeline_key_header_call("anim.keys_interpolation_set", interp="LINEAR")
assert key(a, 20)["interp"] == "LINEAR"
assert [k["frame"] for k in b.keyframes("position")] == [0, 10, 30] and key(b, 30)["interp"] == "SMOOTH"

print("key together path pick: ok")
