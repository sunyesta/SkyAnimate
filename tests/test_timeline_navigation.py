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

# The playback range's bar.
doc.frame_start = 1
doc.frame_end = 100
assert wm.timeline_range_drag("START", 9) and doc.frame_start == 10 and doc.frame_end == 100
assert wm.undo_history()[0][-1] == "Change Start"
assert wm.timeline_range_drag("END", -20) and doc.frame_end == 80
assert wm.undo_history()[0][-1] == "Change End"
assert wm.timeline_range_drag("BAR", 5) and (doc.frame_start, doc.frame_end) == (15, 85), "both, its length kept"
assert wm.undo_history()[0][-1] == "Move Range"
assert wm.timeline_range_drag("BAR", -100) and (doc.frame_start, doc.frame_end) == (0, 70), "never before 0"
assert not wm.timeline_range_drag("BAR", -5), "already at 0"
assert wm.timeline_range_drag("START", 500) and doc.frame_start == 69, ("Start stays before the End", doc.frame_start)
assert not wm.timeline_range_drag("END", -500) and doc.frame_end == 70, ("End stays after the Start", doc.frame_end)
oanim.ops.ed.undo()
assert (doc.frame_start, doc.frame_end) == (0, 70), "each drag one undo step"
oanim.ops.ed.undo()
assert (doc.frame_start, doc.frame_end) == (15, 85)

print("timeline navigation: ok")
