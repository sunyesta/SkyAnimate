"""The Stroke Pen's engine (data/ink_stroke): smoothing, pressure, tapers and the Bezier fit, on strokes made up here.

The Draw tool using it (the preview at real width, nothing jumping on release) needs the window: checked live on a
private copy.

Run with:  OpenAnimate --background --python tests/test_ink_stroke.py
"""

import math
import random

import oanim

wm = oanim.wm
random.seed(7)
RATE = 200.0


def sample(path, seconds, pressure=lambda u: 0.6, tilt=(0.0, 0.0)):
    """A pen following path(u) for u 0-1 over `seconds`, at the tablet's rate."""
    count = int(seconds * RATE) + 1
    out = []
    for i in range(count):
        u = i / (count - 1)
        x, y = path(u)
        out.append((x, y, pressure(u), tilt[0], tilt[1], i / RATE))
    return out


def along_bezier(a, b, t):
    p0, p1, p2, p3 = a[:2], (a[0] + a[4][0], a[1] + a[4][1]), (b[0] + b[3][0], b[1] + b[3][1]), b[:2]
    u = 1.0 - t
    return tuple(u * u * u * p0[k] + 3 * u * u * t * p1[k] + 3 * u * t * t * p2[k] + t * t * t * p3[k] for k in (0, 1))


def knots_place(knots, closed=False):
    """Places along the Bezier points' curve."""
    places = []
    pairs = list(zip(knots, knots[1:] + (knots[:1] if closed else [])))
    for a, b in pairs:
        places += [along_bezier(a, b, i / 24.0) for i in range(25)]
    return places


def worst_off(points, places):
    """How far the furthest point of `points` is from the line through `places`."""

    def off(p, a, b):
        dx, dy = b[0] - a[0], b[1] - a[1]
        t = max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / max(dx * dx + dy * dy, 1e-12)))
        return math.hypot(p[0] - a[0] - dx * t, p[1] - a[1] - dy * t)

    return max(min(off(p, a, b) for a, b in zip(places, places[1:])) for p in points[::4])


# --- Smart smoothing ---------------------------------------------------------------------------------------------
# A slow line with a hand's wobble of 2 pixels, each way, comes out with a fraction of it.
slow = sample(lambda u: (100.0 + 300.0 * u, 200.0 + 2.0 * math.sin(u * 300.0)), 3.0)
line = wm.ink_stroke(slow, line_width=4.0)
middle = line["points"][20:-20]
assert max(abs(p[1] - 200.0) for p in middle) < 0.9, max(abs(p[1] - 200.0) for p in middle)
assert len(line["knots"]) <= 6, len(line["knots"])

# Fast, it follows the hand: a fast curve is not cut short.
fast = sample(lambda u: (100.0 + 800.0 * u, 300.0 + 150.0 * math.sin(u * math.pi * 2.0)), 0.3)
line = wm.ink_stroke(fast, line_width=4.0)
assert worst_off(fast, [p[:2] for p in line["points"]]) < 6.0

# Off: the samples as they are.
off = wm.ink_stroke(slow, stabilizer="OFF", line_width=4.0)
assert max(abs(p[1] - 200.0) for p in off["points"]) > 1.9
try:
    wm.ink_stroke(slow, stabilizer="SOMETIMES")
except ValueError:
    pass
else:
    raise AssertionError("an unknown stabilizer is an error")

# --- What's drawn while the pen is down is what stays --------------------------------------------------------------
for settings in ({}, {"stabilizer": "OFF"}, {"stabilizer": "PULLED_STRING"}):
    line = wm.ink_stroke(fast, line_width=4.0, **settings)
    assert line["points"][: len(line["live"])] == line["live"], settings
    joined = line["live"] + line["tail"]
    assert len(joined) == len(line["points"]), settings
    assert all(a[:2] == b[:2] for a, b in zip(joined, line["points"])), settings
    assert math.hypot(line["points"][-1][0] - fast[-1][0], line["points"][-1][1] - fast[-1][1]) < 1e-3, ("ends at the pen", settings)
    assert math.hypot(line["points"][0][0] - fast[0][0], line["points"][0][1] - fast[0][1]) < 1e-3, ("starts at the pen", settings)

# A mouse's few samples a second still give a smooth line: places between them, not a polygon.
mouse = [(100.0 + 200.0 * math.cos(a * 0.1), 300.0 + 200.0 * math.sin(a * 0.1), 1.0, 0.0, 0.0, a / 60.0) for a in range(32)]
line = wm.ink_stroke(mouse, line_width=4.0)
assert len(line["points"]) > 3 * len(mouse), len(line["points"])

# --- Pulled String -------------------------------------------------------------------------------------------------
short = sample(lambda u: (100.0 + 15.0 * u, 100.0), 0.5)
line = wm.ink_stroke(short, stabilizer="PULLED_STRING", string_length=20.0, catch_up=False)
assert len(line["points"]) == 1, "inside the string's length the line stays put"
line = wm.ink_stroke(short, stabilizer="PULLED_STRING", string_length=20.0, catch_up=True)
assert line["points"][-1][:2] == (115.0, 100.0), "Catch Up ends it at the pen"
long = sample(lambda u: (100.0 + 100.0 * u, 100.0), 0.5)
line = wm.ink_stroke(long, stabilizer="PULLED_STRING", string_length=20.0, catch_up=False)
assert abs(line["points"][-1][0] - 180.0) < 0.5, "a string's length behind the pen"

# --- Pressure ------------------------------------------------------------------------------------------------------
# Jitter in a steady pressure is filtered out; a press still comes through.
level = lambda u: 0.5 + 0.08 * math.sin(u * 900.0)
plain = lambda u: (100.0 + 400.0 * u, 100.0)
raw = wm.ink_stroke(sample(plain, 2.0, level), pressure_smoothing=0.0, stabilizer="OFF", min_size=0.0)
smooth = wm.ink_stroke(sample(plain, 2.0, level), stabilizer="OFF", min_size=0.0)
spread = lambda line: max(p[2] for p in line["points"][40:]) - min(p[2] for p in line["points"][40:])
assert spread(smooth) < 0.5 * spread(raw), (spread(smooth), spread(raw))
pressed = wm.ink_stroke(sample(plain, 1.0, lambda u: 0.2 if u < 0.5 else 0.9), stabilizer="OFF", min_size=0.0)
assert pressed["points"][-1][2] > 0.85, "a real press isn't held back"

# Size: from min_size to the full width, and the pressure's curve.
even = sample(plain, 0.5, lambda u: 0.0)
assert abs(wm.ink_stroke(even, min_size=0.25)["points"][10][2] - 0.25) < 1e-3
assert abs(wm.ink_stroke(sample(plain, 0.5, lambda u: 1.0))["points"][10][2] - 1.0) < 1e-3
half = sample(plain, 0.5, lambda u: 0.5)
assert abs(wm.ink_stroke(half, min_size=0.0)["points"][10][2] - 0.5) < 0.01
soft = wm.ink_stroke(half, min_size=0.0, pressure_curve=(0.0, 0.8, 0.2, 1.0))["points"][10][2]
assert soft > 0.8, soft
hard = wm.ink_stroke(half, min_size=0.0, pressure_curve=(0.8, 0.0, 1.0, 0.2))["points"][10][2]
assert hard < 0.2, hard
assert abs(wm.ink_stroke(half, use_pressure=False)["points"][10][2] - 1.0) < 1e-3

# Speed and tilt.
quick = wm.ink_stroke(sample(plain, 0.05, lambda u: 1.0), speed_width=1.0, min_size=0.1, stabilizer="OFF")
assert quick["points"][-1][2] < 0.2, quick["points"][-1]
upright = wm.ink_stroke(sample(plain, 0.5, lambda u: 1.0), tilt_width=1.0, min_size=0.2)
leaning = wm.ink_stroke(sample(plain, 0.5, lambda u: 1.0, tilt=(60.0, 0.0)), tilt_width=1.0, min_size=0.2)
assert abs(upright["points"][10][2] - 0.2) < 1e-3 and abs(leaning["points"][10][2] - 1.0) < 1e-3

# A pen flicking off as it lifts: the hook isn't kept.
hook = sample(lambda u: (100.0 + 300.0 * u, 100.0), 1.0, lambda u: 0.8)
hook += [(400.0, 100.0 + 1.5 * i, 0.8 - 0.14 * i, 0.0, 0.0, 1.0 + i / RATE) for i in range(1, 5)]
line = wm.ink_stroke(hook, pressure_smoothing=0.0)
assert abs(line["points"][-1][1] - 100.0) < 1.0, line["points"][-1]
# A line that really turns stays.
turn = hook[:-4] + [(400.0, 100.0 + 1.5 * i, 0.8, 0.0, 0.0, 1.0 + i / RATE) for i in range(1, 5)]
assert wm.ink_stroke(turn, pressure_smoothing=0.0)["points"][-1][1] > 104.0

# --- Tapers --------------------------------------------------------------------------------------------------------
full = lambda u: 1.0
line = wm.ink_stroke(sample(plain, 1.0, full), taper_start=40.0, taper_end=60.0, line_width=8.0, stabilizer="OFF")
knots = line["knots"]
assert knots[0][2] == 0.0 and knots[-1][2] == 0.0, (knots[0], knots[-1])
assert max(k[2] for k in knots) > 0.95, [k[2] for k in knots]
assert len(knots) <= 8, len(knots)
# Points the width of a straight run needs, and no others.
assert len(wm.ink_stroke(sample(plain, 1.0, full), line_width=8.0)["knots"]) == 2
# A line shorter than its tapers narrows the whole way.
line = wm.ink_stroke(sample(lambda u: (100.0 + 30.0 * u, 100.0), 0.3, full), taper_start=40.0, taper_end=40.0)
assert line["knots"][0][2] == 0.0 and line["knots"][-1][2] == 0.0

# --- The Bezier fit ------------------------------------------------------------------------------------------------
# A straight line: its two ends.
straight = wm.ink_stroke(sample(lambda u: (100.0 + 400.0 * u, 100.0 + 200.0 * u), 0.6), line_width=4.0)
assert len(straight["knots"]) == 2, len(straight["knots"])

# A circle: few points, close to the line. (Four to five Bezier points make a circle; more is wasted.)
circle = lambda u: (500.0 + 200.0 * math.cos(u * 2.0 * math.pi), 300.0 + 200.0 * math.sin(u * 2.0 * math.pi))
line = wm.ink_stroke(sample(circle, 1.2), line_width=4.0)
assert len(line["knots"]) <= 8, len(line["knots"])
assert worst_off(line["points"], knots_place(line["knots"])) < 1.5
assert not any(k[5] for k in line["knots"][1:-1]), "no corners on a circle"

# Fidelity: closer and more points, or fewer and looser.
wobbly = sample(lambda u: (100.0 + 600.0 * u, 200.0 + 40.0 * math.sin(u * 14.0) + 12.0 * math.sin(u * 37.0)), 1.5)
tight = wm.ink_stroke(wobbly, fidelity=1.0, line_width=4.0)
loose = wm.ink_stroke(wobbly, fidelity=0.0, line_width=4.0)
assert len(loose["knots"]) < len(tight["knots"]), (len(loose["knots"]), len(tight["knots"]))
assert worst_off(tight["points"], knots_place(tight["knots"])) < 0.8
assert worst_off(loose["points"], knots_place(loose["knots"])) < 3.5
assert wm.ink_stroke(slow, fidelity=0.0, line_width=4.0)


# Corners: a zigzag with the pen slowing into each turn gets a corner at each, the rest straight.
def zigzag(u):
    corners = [(100.0, 100.0), (400.0, 300.0), (150.0, 450.0), (500.0, 500.0)]
    legs = len(corners) - 1
    leg = min(int(u * legs), legs - 1)
    f = u * legs - leg
    f = f * f * (3.0 - 2.0 * f)
    a, b = corners[leg], corners[leg + 1]
    return (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f)


line = wm.ink_stroke(sample(zigzag, 1.5), line_width=4.0)
knots = line["knots"]
corners = [k for k in knots[1:-1] if k[5]]
assert len(corners) == 2, [k[:2] for k in knots]
for k, x, y in zip(corners, (400.0, 150.0), (300.0, 450.0)):
    assert math.hypot(k[0] - x, k[1] - y) < 7.0, k
assert len(knots) <= 8, len(knots)
# A pen that stops at the corner gets it exactly, and fewer points: the average settles there.
stops = [(100.0 + 300.0 * i / 60.0, 100.0, 0.6, 0.0, 0.0, i / RATE) for i in range(61)]
stops += [(400.0, 100.0, 0.6, 0.0, 0.0, (60 + i) / RATE) for i in range(1, 41)]
stops += [(400.0, 100.0 + 300.0 * i / 60.0, 0.6, 0.0, 0.0, (100 + i) / RATE) for i in range(1, 61)]
line = wm.ink_stroke(stops, line_width=4.0)
corners = [k for k in line["knots"][1:-1] if k[5]]
assert len(corners) == 1 and math.hypot(corners[0][0] - 400.0, corners[0][1] - 100.0) < 1.0, line["knots"]
assert len(line["knots"]) <= 5, len(line["knots"])
# A corner's handles go their own ways; a smooth point's stay in line.
for k in line["knots"]:
    if k[5]:
        continue
    in_x, in_y, out_x, out_y = k[3][0], k[3][1], k[4][0], k[4][1]
    if math.hypot(in_x, in_y) > 1e-3 and math.hypot(out_x, out_y) > 1e-3:
        assert abs(in_x * out_y - in_y * out_x) / (math.hypot(in_x, in_y) * math.hypot(out_x, out_y)) < 0.01

# A small circle at an even speed turns as sharply as a corner, and isn't one.
small = lambda u: (300.0 + 6.0 * math.cos(u * 12.0 * math.pi), 300.0 + 6.0 * math.sin(u * 12.0 * math.pi))
assert not any(k[5] for k in wm.ink_stroke(sample(small, 3.0), line_width=4.0)["knots"][1:-1])

# Width counts: a line that swells gets a point where it does, though it runs straight.
swell = wm.ink_stroke(sample(plain, 1.0, lambda u: 0.2 + 0.8 * math.sin(u * math.pi)), line_width=40.0)
assert 3 <= len(swell["knots"]) <= 14, len(swell["knots"])
for point in swell["points"][::4]:
    nearest = min(zip(swell["knots"], swell["knots"][1:]), key=lambda pair: abs(pair[0][0] + pair[1][0] - 2.0 * point[0]))
    a, b = nearest
    blend = (point[0] - a[0]) / max(b[0] - a[0], 1e-6)
    assert abs((a[2] + (b[2] - a[2]) * blend - point[2]) * 20.0) < 2.0, (point, a, b)
# And a thin line doesn't: the same swell is under a pixel wide.
assert len(wm.ink_stroke(sample(plain, 1.0, lambda u: 0.2 + 0.8 * math.sin(u * math.pi)), line_width=0.5)["knots"]) == 2

# Closed: the last point joins the first, smoothly.
line = wm.ink_stroke(sample(circle, 1.2), line_width=4.0, closed=True)
knots = line["knots"]
assert 3 <= len(knots) <= 8, len(knots)
assert not knots[0][5] and math.hypot(*knots[0][3]) > 1.0
in_x, in_y, out_x, out_y = knots[0][3][0], knots[0][3][1], knots[0][4][0], knots[0][4][1]
assert abs(in_x * out_y - in_y * out_x) / (math.hypot(in_x, in_y) * math.hypot(out_x, out_y)) < 0.02
assert worst_off(line["points"], knots_place(knots, closed=True)) < 2.0

# A tap is a dot: two points half a pixel apart, the widest width (a taper doesn't take it away).
line = wm.ink_stroke([(100.0, 100.0, 0.7, 0.0, 0.0, 0.0), (100.1, 100.0, 0.8, 0.0, 0.0, 0.01)], taper_start=10.0, min_size=0.0)
assert len(line["knots"]) == 2 and abs(line["knots"][0][2] - 0.7) < 0.15, line["knots"]
assert abs(line["knots"][1][0] - line["knots"][0][0] - 0.5) < 1e-4
assert wm.ink_stroke([])["knots"] == []

# A smaller pixel (zoomed in): the same sizes on screen make the same line, in smaller units.
big = wm.ink_stroke([(x * 0.01, y * 0.01, p, a, b, t) for x, y, p, a, b, t in wobbly], pixel=0.01, line_width=0.04)
near = wm.ink_stroke(wobbly, line_width=4.0)
assert abs(len(big["knots"]) - len(near["knots"])) <= 1, (len(big["knots"]), len(near["knots"]))

# --- The settings --------------------------------------------------------------------------------------------------
settings = wm.draw_settings()
assert settings["stabilizer"] == "SMART" and settings["smoothing"] == 0.5 and settings["fidelity"] == 0.5, settings
assert abs(settings["min_size"] - 0.15) < 1e-6 and settings["taper_start"] == 0.0 and settings["catch_up"]
wm.set_draw(stabilizer="PULLED_STRING", string_length=33, taper_end=12, pressure_curve=(0.1, 0.2, 0.3, 0.4))
settings = wm.draw_settings()
assert settings["stabilizer"] == "PULLED_STRING" and settings["string_length"] == 33.0 and settings["taper_end"] == 12.0
assert abs(settings["pressure_curve"][2] - 0.3) < 1e-6
wm.set_draw(smoothing=5.0)
assert wm.draw_settings()["smoothing"] == 1.0, "kept to 1 at most"
wm.set_draw(stabilizer="SMART", smoothing=0.5, string_length=20, taper_end=0, pressure_curve=(1 / 3, 1 / 3, 2 / 3, 2 / 3))

print("ink stroke: ok")
