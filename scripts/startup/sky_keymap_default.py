"""The default keymap, like Blender's blender_default.py.

Each entry is (keymap, operator idname, key, modifiers). Editor keymaps such as
"Viewport" are tried before "Window" when the mouse is over that editor.
"""

import sky

KEYMAP = (
    ("Window", "ed.undo", "Z", {"ctrl": True}),
    ("Window", "ed.redo", "Z", {"ctrl": True, "shift": True}),
    ("Window", "wm.quit", "Q", {"ctrl": True}),
    # Files, as in Blender.
    ("Window", "wm.read_homefile", "N", {"ctrl": True}),
    ("Window", "wm.open_mainfile", "O", {"ctrl": True}),
    ("Window", "wm.save_mainfile", "S", {"ctrl": True}),
    ("Window", "wm.save_as_mainfile", "S", {"ctrl": True, "shift": True}),
    ("Window", "wm.save_incremental", "S", {"ctrl": True, "alt": True}),
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
    # Viewport: editing the active vector layer, as in Blender's Grease Pencil edit mode.
    ("Viewport", "transform.translate", "G", {}),
    ("Viewport", "transform.rotate", "R", {}),
    ("Viewport", "transform.resize", "S", {}),
    ("Viewport", "mesh.extrude", "E", {}),
    ("Viewport", "mesh.duplicate_move", "D", {"shift": True}),
    # Shape stacking order, as Blender's Arrange for Grease Pencil strokes.
    ("Viewport", "mesh.shape_raise", "PAGE_UP", {}),
    ("Viewport", "mesh.shape_lower", "PAGE_DOWN", {}),
    ("Viewport", "mesh.shape_raise_to_front", "PAGE_UP", {"shift": True}),
    ("Viewport", "mesh.shape_lower_to_back", "PAGE_DOWN", {"shift": True}),
    ("Viewport", "mesh.select_all", "A", {}),
    ("Viewport", "mesh.select_none", "A", {"alt": True}),
    ("Viewport", "mesh.select_invert", "I", {"ctrl": True}),
    ("Viewport", "mesh.select_linked_pick", "L", {}),
    ("Viewport", "mesh.deselect_linked_pick", "L", {"shift": True}),
    ("Viewport", "mesh.select_mode_point", "1", {}),
    ("Viewport", "mesh.select_mode_edge", "2", {}),
    ("Viewport", "mesh.select_mode_stroke", "3", {}),
    ("Viewport", "mesh.delete", "X", {}),
    ("Viewport", "mesh.delete", "DEL", {}),
    ("Viewport", "mesh.create_shape", "F", {}),
    ("Viewport", "mesh.create_line", "F", {"shift": True}),
    ("Viewport", "wm.tool_split_curve", "K", {}),
    # Layers editor.
    ("Layers", "layer.delete", "X", {}),
    ("Layers", "layer.delete", "DEL", {}),
    ("Layers", "layer.duplicate", "D", {"shift": True}),
)


def register():
    for keymap, idname, key, modifiers in KEYMAP:
        sky.keymap.add(keymap, idname, key, **modifiers)
