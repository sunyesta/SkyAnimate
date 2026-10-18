"""SkyAnimate's Python API, shaped like Blender's bpy.

sky.context   the current context: the document and the editor under the mouse
sky.data      the open document: sky.data.document
sky.ops       run operators by name: sky.ops.ed.undo()
sky.types     base classes to subclass: sky.types.Operator
sky.utils     register_class() and unregister_class()
sky.keymap    bind keys to operators
sky.wm        window-manager helpers, such as simulating key presses in tests
"""

import _sky

from . import keymap, ops, types, utils, wm

__all__ = ("context", "data", "keymap", "ops", "types", "utils", "wm")


class _Data:
    """All saved data. For now that is one document."""

    @property
    def document(self):
        return _sky.context().document


data = _Data()


def __getattr__(name):
    # The context changes as the mouse moves between editors, so it is looked up on access.
    if name == "context":
        return _sky.context()
    raise AttributeError(f"module 'sky' has no attribute '{name}'")
