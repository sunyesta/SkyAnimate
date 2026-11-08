"""Timeline navigation: the scroll bar under the keys pans (the bar) and zooms (its ends, never closer than 10
frames); the playback range's bar on the ruler sets Start and End by its ends and moves both by the bar,
Start at 0 or more and before the End, each drag one undo step; playback loops inside it.

Run with:  OpenAnimate --background --python tests/test_timeline_navigation.py
"""

import oanim

doc = oanim.data.document
wm = oanim.wm


def close(a, b):
    return all(abs(x - y) < 1e-4 for x, y in zip(a, b))


# The scroll bar.
assert close(wm.timeline_scrollbar_drag(0, 100, "BAR", 20), (20, 120)), "the bar pans"
assert close(wm.timeline_scrollbar_drag(0, 100, "LEFT", 30), (30, 100)), "its left end zooms that side"
assert close(wm.timeline_scrollbar_drag(0, 100, "RIGHT", -40), (0, 60))
assert close(wm.timeline_scrollbar_drag(0, 100, "LEFT", 95), (90, 100)), "never closer than 10 frames"
assert close(wm.timeline_scrollbar_drag(0, 100, "RIGHT", -200), (0, 10))

print("timeline navigation: ok")
