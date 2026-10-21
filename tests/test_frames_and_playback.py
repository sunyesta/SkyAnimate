"""Frame operators, Blender's frame keys, playback state, and undo leaving the frame alone.

Run with:  OpenAnimate --background --python tests/test_frames_and_playback.py
"""

import oanim

doc = oanim.data.document
doc.frame_start = 1
doc.frame_end = 100
doc.frame_current = 10

# --- Operators ----------------------------------------------------------------
assert oanim.ops.screen.frame_jump_start() == {'FINISHED'} and doc.frame_current == 1
assert oanim.ops.screen.frame_jump_end() == {'FINISHED'} and doc.frame_current == 100
assert oanim.ops.screen.frame_offset_backward() == {'FINISHED'} and doc.frame_current == 99
assert oanim.ops.screen.frame_offset_forward() == {'FINISHED'} and doc.frame_current == 100
doc.frame_current = 0
oanim.ops.screen.frame_offset_backward()
assert doc.frame_current == 0, "frames never go below zero"
assert oanim.ops.screen.keyframe_jump_next() == {'CANCELLED'}, "no keyframes exist yet"

# --- Keys, as in Blender ------------------------------------------------------
doc.frame_current = 50
assert oanim.wm.event_simulate("RIGHT_ARROW") and doc.frame_current == 51
assert oanim.wm.event_simulate("LEFT_ARROW") and doc.frame_current == 50
assert oanim.wm.event_simulate("LEFT_ARROW", shift=True) and doc.frame_current == 1
assert oanim.wm.event_simulate("RIGHT_ARROW", shift=True) and doc.frame_current == 100

# --- Playback state -----------------------------------------------------------
doc.frame_current = 20
assert not oanim.context.is_playing
assert not oanim.ops.screen.animation_cancel.poll(), "Esc does nothing unless playing"
assert oanim.wm.event_simulate("SPACE") and oanim.context.is_playing
assert oanim.wm.event_simulate("SPACE") and not oanim.context.is_playing, "Space pauses"
assert oanim.wm.event_simulate("SPACE", ctrl=True, shift=True) and oanim.context.is_playing
doc.frame_current = 70  # as if playback moved on
assert oanim.wm.event_simulate("ESC") and not oanim.context.is_playing
assert doc.frame_current == 20, "Esc returns to the frame playback started from"

# --- Undo doesn't move the playhead -------------------------------------------
doc.frame_current = 5
oanim.ops.layer.new()
doc.frame_current = 42
oanim.ops.ed.undo()
assert doc.frame_current == 42, doc.frame_current

# --- Current action: mainline by default ---------------------------------------
assert doc.action == "", "the timeline starts on the mainline"
doc.action = "head_turn"
assert doc.action == "", "there is no such action (see test_actions.py)"

# Shift Z over the viewport toggles its overlays (the viewport flips them as it draws).
assert oanim.wm.event_simulate("Z", shift=True, area="Viewport")

# Tab: to the setup frame (0) and back to the frame you came from.
doc.frame_current = 17
assert oanim.wm.event_simulate("TAB", area="Viewport")
assert doc.frame_current == 0
assert oanim.wm.event_simulate("TAB", area="Viewport")
assert doc.frame_current == 17
assert not oanim.wm.event_simulate("TAB", area="Viewport", typing=True) or doc.frame_current == 17, "a text field keeps Tab"

print("Frame and playback tests passed")
