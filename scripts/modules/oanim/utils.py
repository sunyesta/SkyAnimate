"""Registering Python classes with Open Animate, like bpy.utils."""

import _oanim

from . import types

_STATUSES = {"FINISHED", "CANCELLED", "PASS_THROUGH"}


def register_class(cls):
    """Make a oanim.types.Operator subclass available as oanim.ops.<bl_idname>."""
    if not (isinstance(cls, type) and issubclass(cls, types.Operator)):
        raise TypeError(f"register_class expects a oanim.types.Operator subclass, got {cls!r}")
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
    _oanim.register_operator(cls.bl_idname, cls.bl_label or cls.__name__, description, sorted(cls.bl_options), poll, execute)


def flip_name(name):
    """The other side's name, as bpy.utils.flip_name: "arm_L" <-> "arm_R", "L_foot" <-> "R_foot", "L arm" <-> "R arm",
    "Eye_Lash_L 2" <-> "Eye_Lash_R 2", "hand.L.001" <-> "hand.R.001", "LeftArm" <-> "RightArm"; a name
    without a side comes back as it is. Bone Symmetrize, X mirror editing and Symmetrize's weights pair
    bones by it."""
    return _oanim.flip_name(str(name))


def unregister_class(cls):
    _oanim.unregister_operator(cls.bl_idname)
