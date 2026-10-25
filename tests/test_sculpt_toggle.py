"""Ctrl Tab (anywhere, beside Tab) toggles the Sculpt tool, as Blender toggles sculpt mode: into Sculpt from
any tool, and back to that tool.

Run with:  OpenAnimate --background --python tests/test_sculpt_toggle.py
"""

import oanim

wm = oanim.wm
ops = oanim.ops
oanim.data.document.frame_current = 0


def ctrl_tab():
    wm.event_simulate("TAB", ctrl=True, area="Viewport")


ops.layer.new()
assert ops.wm.tool_transform_points() == {'FINISHED'}
assert wm.tool() == "TRANSFORM_POINTS"
ctrl_tab()
assert wm.tool() == "SCULPT", wm.tool()
ctrl_tab()
assert wm.tool() == "TRANSFORM_POINTS", ("back to the tool before", wm.tool())

# From the Select tool, and back.
ops.wm.tool_select()
ctrl_tab()
assert wm.tool() == "SCULPT"
ctrl_tab()
assert wm.tool() == "SELECT"

# Sculpt picked from the tool bar: Ctrl Tab goes back to the tool before the toggle last went in from.
ops.wm.tool_cursor()
ctrl_tab()
ctrl_tab()
assert wm.tool() == "CURSOR"
ops.wm.tool_sculpt()
ctrl_tab()
assert wm.tool() == "CURSOR", wm.tool()

# Anywhere, not only over the viewport (as plain Tab), and not the setup frame toggle.
ops.wm.tool_select()
oanim.data.document.frame_current = 10
wm.event_simulate("TAB", ctrl=True, area="Timeline")
assert wm.tool() == "SCULPT" and oanim.data.document.frame_current == 10, (wm.tool(), oanim.data.document.frame_current)
wm.event_simulate("TAB", ctrl=True, area="Properties")
assert wm.tool() == "SELECT", wm.tool()

# Plain Tab still toggles the setup frame.
oanim.data.document.frame_current = 10
wm.event_simulate("TAB", area="Viewport")
assert oanim.data.document.frame_current == 0, oanim.data.document.frame_current

print("sculpt toggle: ok")
