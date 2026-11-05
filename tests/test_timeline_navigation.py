"""Timeline navigation: the scroll bar under the keys pans (the bar) and zooms (its ends, never closer than 10
frames); the playback range's bar on the ruler sets Start and End by its ends and moves both by the bar,
Start at 0 or more and before the End, each drag one undo step; playback loops inside it. With the sidebar (N)
closed, a small tab at the keys' right edge shows it's there.

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

# The sidebar's tab: at the keys' right edge, a third of the way down, while the sidebar is closed.
tab = wm.timeline_sidebar_tab((100, 50), (900, 350))
assert tab is not None, "closed: the tab shows"
x0, y0, x1, y1 = tab
assert abs(x1 - 900) < 1e-4 and 6 <= x1 - x0 <= 20, ("at the right edge, narrow", tab)
assert abs((y0 + y1) / 2 - 150) < 1e-4, ("a third of the way down", tab)
assert wm.timeline_sidebar_tab((100, 50), (900, 350), open=True) is None, "open: no tab"
assert wm.timeline_sidebar_tab((100, 50), (130, 60)) is None, "no room: no tab"

print("timeline navigation: ok")
