"""Base classes for things defined in Python, like bpy.types."""


class Operator:
    """Base class for operators, like bpy.types.Operator.

    Subclasses set bl_idname ("category.name") and bl_label, and define
    execute(self, context), returning {'FINISHED'} or {'CANCELLED'}.
    An optional poll(cls, context) classmethod decides when the operator is available.
    bl_options may contain 'REGISTER' (log it in the Info editor) and 'UNDO' (push an undo step).
    """

    bl_idname = ""
    bl_label = ""
    bl_description = ""
    bl_options = set()

    def execute(self, context):
        raise NotImplementedError(f"{type(self).__name__}.execute() is not defined")
