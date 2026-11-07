"""Where the time goes: oanim.wm.profile times named sections per UI frame once profile_enable is on (off when the app
starts, costing nothing). A path's control points work out where each shows and how it moves (three frames worked out)
but only where the path is on its frame (one more), not a second jacobian.

Run with:  OpenAnimate --background --python tests/test_profile.py
"""

import oanim

ops, doc, L, wm = oanim.ops, oanim.data.document, oanim.data.layers, oanim.wm


def sections():
    return {name: calls for name, mean, slowest, calls in wm.profile(600)}


# --- Off until enabled ---------------------------------------------------------------------------------------------------
ops.wm.read_homefile()
for f in (3, 4):
    doc.frame_current = f
    wm.profile_frame_end()
assert wm.profile(600) == [], "nothing timed while off"

# --- On: frames and what ran in them ----------------------------------------------------------------------------------
wm.profile_enable(True)
for f in range(1, 6):
    doc.frame_current = f
    wm.profile_frame_end()
got = sections()
assert got.get("frame") == 4, got   # (the first frame only starts the clock)
assert got.get("eval_layers", 0) >= 4, got   # each frame worked out
wm.profile_clear()
assert wm.profile(600) == []

# --- A control point's place costs four frames worked out, not six ----------------------------------------------------
doc.auto_key = False
assert ops.wm.tool_motion_path() == {'FINISHED'}
ops.layer.new()
lay = L.active
lay.add_curve([(0.0, 0.0), (0.1, 0.0)])
for other in L:
    other.selected = other.id == lay.id
L.active = lay
args = dict(layer=str(lay.id))
for f, (x, y) in {10: (0.0, 0.0), 30: (4.0, 0.0)}.items():
    assert ops.anim.motion_path_move(frame=str(f), x=str(x), y=str(y), **args) == {'FINISHED'}
assert ops.anim.motion_path_control_add(frame="20", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_move(frame="30", index="0", x="2", y="2", **args) == {'FINISHED'}
wm.profile_clear()
controls = wm.motion_path_controls()
wm.profile_frame_end()
assert len(controls) == 1, controls
c = controls[0]
calls = sections()
assert calls.get("path_position") == 4, ("frames worked out for one control point", calls.get("path_position"))
# Where it is, and the path through its nearest frame, as before.
assert abs(c["at"][0] - 2.0) < 1e-3 and abs(c["at"][1] - 2.0) < 1e-3, c
doc.frame_current = c["near_frame"]
assert abs(c["curve"][0] - lay.location_x) < 1e-3 and abs(c["curve"][1] - lay.location_y) < 1e-3, (c, lay.location_x,
                                                                                                  lay.location_y)
wm.profile_enable(False)

print("test_profile: all passed")
