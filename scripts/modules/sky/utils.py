"""Registering Python classes with SkyAnimate, like bpy.utils."""

import _sky

from . import types

_STATUSES = {"FINISHED", "CANCELLED", "PASS_THROUGH"}


def register_class(cls):
    """Make a sky.types.Operator subclass available as sky.ops.<bl_idname>."""
    if not (isinstance(cls, type) and issubclass(cls, types.Operator)):
        raise TypeError(f"register_class expects a sky.types.Operator subclass, got {cls!r}")
    if not cls.bl_idname:
        raise ValueError(f"{cls.__name__} has no bl_idname")

    poll = cls.poll if hasattr(cls, "poll") else None

    def execute(context):
        result = cls().execute(context)
        statuses = set(result) & _STATUSES if isinstance(result, (set, frozenset)) else set()
        if len(statuses) != 1:
            raise ValueError(f"{cls.__name__}.execute() must return {{'FINISHED'}} or {{'CANCELLED'}}, got {result!r}")
        return statuses.pop()

    description = cls.bl_description or (cls.__doc__ or "").strip()
    _sky.register_operator(cls.bl_idname, cls.bl_label or cls.__name__, description, sorted(cls.bl_options), poll, execute)


def unregister_class(cls):
    _sky.unregister_operator(cls.bl_idname)
