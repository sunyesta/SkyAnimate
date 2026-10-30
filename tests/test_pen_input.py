"""Pen tablets (Windows Ink, every sample): the Draw tool's prediction, its settings, the pen's state.

Every sample of a pen's touch reaching the Draw tool, and the eraser end not drawing, need the window: they are
checked live with oanim.wm.mouse_simulate(pen=[...]) on a private copy.

Run with:  OpenAnimate --background --python tests/test_pen_input.py
"""

import math

import oanim

wm = oanim.wm

# --- Prediction: the line drawn a moment ahead of the pen, the way it is going ---------------------------------
# Going right at 1000 px/s (a sample every 5 ms): 8 ms on is 8 px further.
line = [(100.0 + 5.0 * i, 50.0, 0.005 * i) for i in range(10)]
x, y = wm.pen_predict(line, 0.008)
assert abs(x - (145.0 + 8.0)) < 1e-3 and abs(y - 50.0) < 1e-3, (x, y)
# Never further than the limit.
x, y = wm.pen_predict(line, 0.5, max_pixels=40.0)
assert abs(x - (145.0 + 40.0)) < 1e-3, x
# Slow (100 px/s): no guess, the last sample itself, since a guess there wobbles.
slow = [(100.0 + 0.5 * i, 50.0, 0.005 * i) for i in range(10)]
assert wm.pen_predict(slow, 0.008) == (104.5, 50.0)
# Too few samples, or none ahead: the last sample.
assert wm.pen_predict(line[:2], 0.008) == line[1][:2]
assert wm.pen_predict(line, 0.0) == line[-1][:2]
# Its speed is the last 30 ms's: a turn shows at once rather than averaged away.
turn = [(100.0 + 5.0 * i, 50.0, 0.005 * i) for i in range(20)]
turn += [(195.0, 50.0 + 5.0 * i, 0.1 + 0.005 * i) for i in range(1, 9)]
x, y = wm.pen_predict(turn, 0.008)
assert abs(x - 195.0) < 1e-3 and y > turn[-1][1] + 7.0, (x, y)
# Samples at the same moment (a pen that reported no clock): no guess.
assert wm.pen_predict([(0.0, 0.0, 0.0), (5.0, 0.0, 0.0), (10.0, 0.0, 0.0)], 0.008) == (10.0, 0.0)

# --- The Draw tool's settings -----------------------------------------------------------------------------------
settings = wm.draw_settings()
assert settings["prediction"] == 8.0 and settings["use_pressure"] and settings["mode"] == "FREEHAND", settings
wm.set_draw(prediction=12, fill=True)
assert wm.draw_settings()["prediction"] == 12.0 and wm.draw_settings()["fill"]
wm.set_draw(prediction=500)
assert wm.draw_settings()["prediction"] == 30.0, "kept to 30 ms at most"
wm.set_draw(prediction=0, fill=False)
assert wm.draw_settings()["prediction"] == 0.0
try:
    wm.set_draw(nonsense=1)
except ValueError:
    pass
else:
    raise AssertionError("an unknown setting is an error")
wm.set_draw(prediction=8)

# --- The pen's state ------------------------------------------------------------------------------------------
pen = wm.pen(samples=True)
for key in ("windows_ink", "near", "touching", "eraser", "pressure", "tilt", "touch", "sample_count", "samples"):
    assert key in pen, (key, pen)
assert not pen["windows_ink"] and not pen["touching"] and pen["touch"] == 0 and pen["samples"] == []
# A pen's samples queue with a simulated mouse event (used on the next frame, which a background run has none of).
wm.mouse_simulate(10, 10, 0, True, pen=[(10, 10, 0.5, 20, -10, 0.0), (12, 11)], eraser=True)
wm.mouse_simulate(12, 11, 0, False, pen=[])

print("pen input: ok")
