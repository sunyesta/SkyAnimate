"""SkyAnimate's Python API, shaped like Blender's bpy.

sky.context   the current context: the document and the editor under the mouse
sky.data      the open document and its texts: sky.data.document, sky.data.texts
sky.ops       run operators by name: sky.ops.ed.undo()
sky.types     base classes to subclass: sky.types.Operator
sky.utils     register_class() and unregister_class()
sky.keymap    bind keys to operators
sky.wm        window-manager helpers, such as simulating key presses in tests
"""

import _sky

from . import keymap, ops, types, utils, wm

__all__ = ("context", "data", "keymap", "ops", "types", "utils", "wm")


class _Text:
    """A text block in the document, like bpy.types.Text."""

    def __init__(self, name):
        self._name = name

    @property
    def name(self):
        return self._name

    @property
    def body(self):
        return _sky.text_body(self._name)

    @body.setter
    def body(self, value):
        _sky.text_set_body(self._name, value)

    @property
    def filepath(self):
        return _sky.text_filepath(self._name)

    def __eq__(self, other):
        return isinstance(other, _Text) and other._name == self._name

    def __repr__(self):
        return f"sky.data.texts[{self._name!r}]"


class _Texts:
    """The document's text blocks, like bpy.data.texts."""

    def new(self, name="Text"):
        return _Text(_sky.text_new(name))

    def remove(self, text):
        _sky.text_remove(text.name)

    def keys(self):
        return _sky.text_names()

    def __getitem__(self, name):
        if name not in _sky.text_names():
            raise KeyError(name)
        return _Text(name)

    def __contains__(self, name):
        return name in _sky.text_names()

    def __iter__(self):
        return iter([_Text(name) for name in _sky.text_names()])

    def __len__(self):
        return len(_sky.text_names())

    @property
    def active(self):
        """The text shown in the text editor, or None."""
        name = _sky.text_active()
        return _Text(name) if name else None

    @active.setter
    def active(self, text):
        _sky.text_set_active(text.name if text else "")


class _Data:
    """All saved data: the document and its text blocks."""

    @property
    def document(self):
        return _sky.context().document

    texts = _Texts()


data = _Data()


def __getattr__(name):
    # The context changes as the mouse moves between editors, so it is looked up on access.
    if name == "context":
        return _sky.context()
    raise AttributeError(f"module 'sky' has no attribute '{name}'")
