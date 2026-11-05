"""Overlay colours: wires and points are #919191, and only the piece that is hard to see over what is drawn under it
takes #AEAEAE (over something darker) or #6E6E6E (over something lighter). Motion path lines the same with #4D9191,
#5CA8AE and #3A7070 for the first path; a path's own colour makes its own lighter and darker. Selected stays orange; a
segment with one end picked fades from orange there to the other end's colour (as Blender's edit mode). Handles are
red (#C9483B) with a black outline, #68EB50 where picked (fading the same way), their tips orange when picked, round on
smooth points and square on broken ones. A motion path handle keeps its white, fading to orange at a dragged tip."""

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers
wm = oanim.wm

BASE, LIGHTER, DARKER = "#919191", "#AEAEAE", "#6E6E6E"
P_BASE, P_LIGHTER, P_DARKER = "#4D9191", "#5CA8AE", "#3A7070"
GREY = (0x91 / 255, 0x91 / 255, 0x91 / 255)
BLACK, WHITE = (0.0, 0.0, 0.0), (1.0, 1.0, 1.0)


def wires_on(left, right, split, x0=-1.0, x1=1.0):
    """The wire pieces' colours, left to right, of a flat line from x0 to x1."""
    out = wm.overlay_colors(art, left, right, split)
    return [(min(w["a"][0], w["b"][0]), max(w["a"][0], w["b"][0]), w["color"], w["width"]) for w in out["wires"]]


def colors_at(pieces, x):
    return {c for a, b, c, _ in pieces if a - 1e-4 <= x <= b + 1e-4}


ops.wm.read_homefile()
doc.frame_current = 0
ops.layer.new()
art = layers.active
art.key_whole_drawing = False
art.add_curve([(-1.0, 0.0), (1.0, 0.0)])
art.select_points([0, 1], False)

# --- Over black everywhere: plenty of contrast, all the usual grey, 1.5 px -----------------------------------------
pieces = wires_on(BLACK, BLACK, 0.0)
assert pieces and {c for *_, c, _w in pieces} == {BASE}, pieces
assert all(abs(w - 1.5) < 1e-6 for *_, w in pieces), pieces

# --- Grey under the right half only: that half alone switches; lighter, as grey isn't lighter than the wire --------
pieces = wires_on(BLACK, GREY, 0.0)
assert colors_at(pieces, -0.5) == {BASE}, ("the left half keeps its grey", pieces)
assert colors_at(pieces, 0.5) == {LIGHTER}, ("only the hard-to-see half", pieces)

# --- A light grey just above the wire's: the darker one ------------------------------------------------------------
pieces = wires_on(BLACK, (0.62, 0.62, 0.62), 0.0)
assert colors_at(pieces, 0.5) == {DARKER}, pieces
assert colors_at(pieces, -0.5) == {BASE}, pieces

# --- White: clear enough again, the usual grey ---------------------------------------------------------------------
assert {c for *_, c, _w in wires_on(WHITE, WHITE, 0.0)} == {BASE}

# --- Points too, each by what is under it --------------------------------------------------------------------------
pts = wm.overlay_colors(art, BLACK, GREY, 0.0)["points"]
by_x = {round(p["at"][0], 3): p["color"] for p in pts}
assert by_x[-1.0] == BASE and by_x[1.0] == LIGHTER, by_x

# --- Selected stays orange whatever is under it --------------------------------------------------------------------
art.select_points([0, 1])
pieces = wires_on(GREY, GREY, 0.0)
assert BASE not in {c for *_, c, _w in pieces} and LIGHTER not in {c for *_, c, _w in pieces}, pieces
pts = wm.overlay_colors(art, GREY, GREY, 0.0)["points"]
assert all(p["color"] not in (BASE, LIGHTER, DARKER) for p in pts), pts
art.select_points([0, 1], False)

# --- Hysteresis: a stretch just past the switch keeps the alternative, no speckle ----------------------------------
# Under a grey that is borderline (between the switch-in and switch-out contrast), pieces that come after a hard-to-see
# piece stay switched; on their own (nothing hard to see before them) they keep the usual colour.
border = (0x91 / 255 - 0.14, 0x91 / 255 - 0.14, 0x91 / 255 - 0.14)
pieces = wires_on(GREY, border, 0.0)
assert colors_at(pieces, 0.5) == {LIGHTER}, ("held after a hard piece", pieces)
pieces = wires_on(BLACK, border, 0.0)
assert colors_at(pieces, 0.5) == {BASE}, ("borderline on its own: still readable", pieces)

# --- One end picked: orange there, fading along the segment to the other end's colour --------------------------------
ORANGE, H_DARK, H_BRIGHT = "#FF8C00", "#C9483B", "#68EB50"
ops.layer.new()
bend = layers.active
bend.add_curve([(-1.0, 0.0), (0.0, 1.0), (1.0, 0.0)])
bend.select_points([0, 1, 2], False)


def segment(wires, x0, x1):
    """The curve's wire pieces between x0 and x1, in order along it (no handles here)."""
    return [w for w in wires if w["width"] < 2.0 and x0 - 1e-4 <= min(w["a"][0], w["b"][0])
            and max(w["a"][0], w["b"][0]) <= x1 + 1e-4]


def rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def near(a, b, tol=3):
    return all(abs(x - y) <= tol for x, y in zip(rgb(a), rgb(b)))


bend.select_points([0])
wires = wm.overlay_colors(bend, BLACK, BLACK, 0.0)["wires"]
first = segment(wires, -1.0, 0.0)
first.sort(key=lambda w: min(w["a"][0], w["b"][0]))
assert first and all(w["color_b"] for w in first), ("the picked end's segment fades", first[:2])
assert near(first[0]["color"], ORANGE), ("orange at the picked point", first[0])
assert near(first[-1]["color_b"], BASE), ("the usual grey at the other end", first[-1])
mid = first[len(first) // 2]
assert not near(mid["color"], ORANGE) and not near(mid["color"], BASE), ("blended between", mid)
second = segment(wires, 0.0, 1.0)
assert second and all(w["color_b"] is None and w["color"] == BASE for w in second), ("nothing picked: plain", second[:2])

# The unpicked end fades to whatever reads there: the lighter grey over grey.
bend.select_points([0], False)
bend.select_points([2])
second = segment(wm.overlay_colors(bend, BLACK, GREY, 0.0)["wires"], 0.0, 1.0)
second.sort(key=lambda w: min(w["a"][0], w["b"][0]))
assert near(second[0]["color"], LIGHTER), ("fades to the readable grey", second[0])
assert near(second[-1]["color_b"], ORANGE), second[-1]
# Both ends picked: orange all along, no fade.
bend.select_points([1])
second = segment(wm.overlay_colors(bend, BLACK, GREY, 0.0)["wires"], 0.0, 1.0)
assert second and all(w["color_b"] is None and w["color"] == ORANGE for w in second), second[:2]
bend.select_points([0, 1, 2], False)

# --- Handles: green lines outlined in black, tips round on smooth points, square on broken ones ----------------------
here = (0.0, 1.0)
_, h_in, h_out = bend.curve_handles(0)[1]


def handle(out, tip):
    """The middle point's handle line (from the point to `tip`), its black outline under it, and its tip mark."""
    lines = [w for w in out["wires"] if close2(w["a"], here) and close2(w["b"], tip)]
    line = next(w for w in lines if abs(w["width"] - 1.5) < 1e-6)
    outline = next(w for w in lines if w["width"] > 2.0)
    mark = next(p for p in out["points"] if close2(p["at"], tip))
    return line, outline, mark


def close2(a, b):
    return abs(a[0] - b[0]) < 1e-4 and abs(a[1] - b[1]) < 1e-4


out = wm.overlay_colors(bend, BLACK, BLACK, 0.0, handles=True)
for tip in (h_in, h_out):
    line, outline, mark = handle(out, tip)
    assert line["color"] == H_DARK and line["color_b"] == H_DARK, ("unpicked: red", line)
    assert outline["color"] == "#000000", ("a thin black outline", outline)
    assert mark["color"] == H_DARK and mark["outline"] == "#000000" and mark["round"], ("a smooth point's: round", mark)

# The point picked lights both its handles all along; their tips orange.
bend.select_points([1])
out = wm.overlay_colors(bend, BLACK, BLACK, 0.0, handles=True)
for tip in (h_in, h_out):
    line, _, mark = handle(out, tip)
    assert line["color"] == H_BRIGHT and line["color_b"] == H_BRIGHT, line
    assert mark["color"] == ORANGE, mark
# Broken (handles set apart, not kept in line): square tips.
bend.move_handle(0, 1, "OUT", (0.5, 1.3))
_, h_in, h_out = bend.curve_handles(0)[1]
out = wm.overlay_colors(bend, BLACK, BLACK, 0.0, handles=True)
assert not handle(out, h_in)[2]["round"] and not handle(out, h_out)[2]["round"], "a broken point's: square"

# Only one tip picked, not its point: that line fades from bright at the tip to dark at the point; the other stays dark.
bend.select_points([1], False)
bend.select_handle(0, 1, "OUT")
out = wm.overlay_colors(bend, BLACK, BLACK, 0.0, handles=True)
line, _, mark = handle(out, h_out)
assert line["color"] == H_DARK and line["color_b"] == H_BRIGHT and mark["color"] == ORANGE, (line, mark)
line, _, mark = handle(out, h_in)
assert line["color"] == H_DARK and line["color_b"] == H_DARK and mark["color"] == H_DARK, (line, mark)
layers.active = art

# --- Motion path handles: their white as is; a dragged tip fades to orange there ------------------------------------
assert wm.path_handle_colors(False) == [(0.0, 1.0, "#EBEBEB")], wm.path_handle_colors(False)
runs = wm.path_handle_colors(True)
assert len(runs) > 4 and runs[0][0] == 0.0 and runs[-1][1] == 1.0
assert near(runs[0][2], "#EBEBEB", 12) and near(runs[-1][2], "#FFA028", 12), runs
greens = [rgb(r[2])[2] for r in runs]
assert all(a >= b for a, b in zip(greens, greens[1:])), ("an even fade", runs)

# --- Motion paths: the first path #4D9191, switching only where hard to see ----------------------------------------
doc.auto_key = True
doc.frame_current = 10
art.set_point(0, (-1.0, 2.0))
doc.frame_current = 5
doc.motion_paths = True
doc.path_all = True
for l in layers.all():
    l.selected = l.id == art.id
art.select_points([0])
ops.wm.tool_motion_path()
label = art.name + " point 0"
TEAL = (0x4D / 255, 0x91 / 255, 0x91 / 255)


def point_path(left, right, split):
    return next(p for p in wm.overlay_colors(art, left, right, split)["paths"] if p["label"] == label)


assert [p["label"] for p in wm.motion_paths()] == [label], [p["label"] for p in wm.motion_paths()]
pieces = point_path(BLACK, BLACK, 0.0)["pieces"]
assert pieces and {k["color"] for k in pieces} == {P_BASE}, pieces
assert all(abs(k["a"][0] + 1.0) < 1e-3 for k in pieces), "straight up x = -1"
assert tuple(round(c, 3) for c in wm.motion_paths()[0]["color"]) == tuple(round(c, 3) for c in TEAL),     "the first path's colour is the Preferences'"
assert {k["color"] for k in point_path(BLACK, TEAL, -1.5)["pieces"]} == {P_LIGHTER}
assert {k["color"] for k in point_path(BLACK, (0.4, 0.66, 0.66), -1.5)["pieces"]} == {P_DARKER}
assert {k["color"] for k in point_path(TEAL, BLACK, -0.5)["pieces"]} == {P_LIGHTER}, "the left side is what's under it"

# Half the path over teal: only that half switches (the path tilted across x = 0).
doc.frame_current = 10
art.set_point(0, (1.0, 2.0))
doc.frame_current = 5
art.select_points([0])
seen = [(k["a"][0], k["color"]) for k in point_path(BLACK, TEAL, 0.0)["pieces"]]
assert any(c == P_BASE and x < -0.1 for x, c in seen) and any(c == P_LIGHTER and x > 0.1 for x, c in seen), seen

# A path's own colour: its own lighter and darker, not the Preferences'.
assert ops.anim.motion_path_style(color="0.9,0.2,0.2", layer=str(art.id), point=str(art.point_ids[0])) == {'FINISHED'}
own = {k["color"] for k in point_path(BLACK, BLACK, 0.0)["pieces"]}
assert own and not own & {P_BASE, P_LIGHTER, P_DARKER}, own

print("Overlay colours tests passed")
