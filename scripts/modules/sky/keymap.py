"""Key bindings. A keymap is "Window" for global shortcuts or an editor name such as "Viewport".

Key names follow Blender: "A", "F3", "SPACE", "ESC", "RET", "DEL", "TAB", "LEFT_ARROW", ...
"""

import _sky


def add(keymap, idname, type, *, ctrl=False, shift=False, alt=False):
    """Bind a key to an operator. Binding the same key again replaces the old binding."""
    _sky.keymap_add(keymap, idname, type, ctrl, shift, alt)


def items():
    """All bindings as (keymap, operator idname, key label) tuples."""
    return _sky.keymap_items()
