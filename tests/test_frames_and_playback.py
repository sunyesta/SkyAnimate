"""Frame operators, Blender's frame keys, playback state, and undo leaving the frame alone.

Run with:  SkyAnimate --background --python tests/test_frames_and_playback.py
"""

import sky

doc = sky.data.document
doc.frame_start = 1
doc.frame_end = 100
doc.frame_current = 10

# --- Operators ----------------------------------------------------------------
assert sky.ops.screen.frame_jump_start() == {'FINISHED'} and doc.frame_current == 1
assert sky.ops.screen.frame_jump_end() == {'FINISHED'} and doc.frame_current == 100
assert sky.ops.screen.frame_offset_backward() == {'FINISHED'} and doc.frame_current == 99
assert sky.ops.screen.frame_offset_forward() == {'FINISHED'} and doc.frame_current == 100
doc.frame_current = 0
sky.ops.screen.frame_offset_backward()
assert doc.frame_current == 0, "frames never go below zero"
assert sky.ops.screen.keyframe_jump_next() == {'CANCELLED'}, "no keyframes exist yet"

# --- Keys, as in Blender ------------------------------------------------------
doc.frame_current = 50
assert sky.wm.event_simulate("RIGHT_ARROW") and doc.frame_current == 51
assert sky.wm.event_simulate("LEFT_ARROW") and doc.frame_current == 50
assert sky.wm.event_simulate("LEFT_ARROW", shift=True) and doc.frame_current == 1
assert sky.wm.event_simulate("RIGHT_ARROW", shift=True) and doc.frame_current == 100

# --- Playback state -----------------------------------------------------------
doc.frame_current = 20
assert not sky.context.is_playing
assert not sky.ops.screen.animation_cancel.poll(), "Esc does nothing unless playing"
assert sky.wm.event_simulate("SPACE") and sky.context.is_playing
assert sky.wm.event_simulate("SPACE") and not sky.context.is_playing, "Space pauses"
assert sky.wm.event_simulate("SPACE", ctrl=True, shift=True) and sky.context.is_playing
doc.frame_current = 70  # as if playback moved on
assert sky.wm.event_simulate("ESC") and not sky.context.is_playing
assert doc.frame_current == 20, "Esc returns to the frame playback started from"

# --- Undo doesn't move the playhead -------------------------------------------
doc.frame_current = 5
sky.ops.layer.new()
doc.frame_current = 42
sky.ops.ed.undo()
assert doc.frame_current == 42, doc.frame_current

# --- Current action: mainline by default, and undo leaves it alone -------------
assert doc.action == "", "the timeline starts on the mainline"
doc.action = "head_turn"
sky.ops.layer.new()
sky.ops.ed.undo()
assert doc.action == "head_turn", doc.action
doc.action = ""

print("Frame and playback tests passed")
