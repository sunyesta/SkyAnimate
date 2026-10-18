"""The default keymap, like Blender's blender_default.py.

Each entry is (keymap, operator idname, key, modifiers). Editor keymaps such as
"Viewport" are tried before "Window" when the mouse is over that editor.
"""

import sky

KEYMAP = (
    ("Window", "ed.undo", "Z", {"ctrl": True}),
    ("Window", "ed.redo", "Z", {"ctrl": True, "shift": True}),
    ("Window", "wm.quit", "Q", {"ctrl": True}),
    # Frames and playback, as in Blender's "Frames" keymap.
    ("Window", "screen.animation_play", "SPACE", {}),
    ("Window", "screen.animation_play_reverse", "SPACE", {"ctrl": True, "shift": True}),
    ("Window", "screen.animation_cancel", "ESC", {}),
    ("Window", "screen.frame_offset_backward", "LEFT_ARROW", {}),
    ("Window", "screen.frame_offset_forward", "RIGHT_ARROW", {}),
    ("Window", "screen.frame_jump_start", "LEFT_ARROW", {"shift": True}),
    ("Window", "screen.frame_jump_end", "RIGHT_ARROW", {"shift": True}),
    ("Window", "screen.keyframe_jump_prev", "DOWN_ARROW", {}),
    ("Window", "screen.keyframe_jump_next", "UP_ARROW", {}),
    ("Window", "screen.workspace_cycle_next", "PAGE_DOWN", {"ctrl": True}),
    ("Window", "screen.workspace_cycle_prev", "PAGE_UP", {"ctrl": True}),
    # Text editor, as in Blender. These work while typing in the text.
    ("Text Editor", "text.run_script", "P", {"alt": True}),
    ("Text Editor", "text.save", "S", {"alt": True}),
    ("Text Editor", "text.new", "N", {"alt": True}),
)


def register():
    for keymap, idname, key, modifiers in KEYMAP:
        sky.keymap.add(keymap, idname, key, **modifiers)
