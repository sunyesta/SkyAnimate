"""The default keymap, like Blender's blender_default.py.

Each entry is (keymap, operator idname, key, modifiers). Editor keymaps such as
"Viewport" are tried before "Window" when the mouse is over that editor.
"""

import sky

KEYMAP = (
    ("Window", "ed.undo", "Z", {"ctrl": True}),
    ("Window", "ed.redo", "Z", {"ctrl": True, "shift": True}),
    ("Window", "wm.quit", "Q", {"ctrl": True}),
)


def register():
    for keymap, idname, key, modifiers in KEYMAP:
        sky.keymap.add(keymap, idname, key, **modifiers)
