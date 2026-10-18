"""Temporary operators for milestone 1: they change the document's test counter so that
operators, keymaps and undo can be tried by hand. Remove once layers exist (milestone 2).
"""

import sky


class DOC_OT_counter_add(sky.types.Operator):
    """Add one to the document's test counter"""

    bl_idname = "doc.counter_add"
    bl_label = "Add to Counter"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        context.document.counter += 1
        return {'FINISHED'}


class DOC_OT_counter_reset(sky.types.Operator):
    """Set the document's test counter back to zero"""

    bl_idname = "doc.counter_reset"
    bl_label = "Reset Counter"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.document.counter != 0

    def execute(self, context):
        context.document.counter = 0
        return {'FINISHED'}


classes = (DOC_OT_counter_add, DOC_OT_counter_reset)


def register():
    for cls in classes:
        sky.utils.register_class(cls)
    sky.keymap.add("Window", "doc.counter_add", "T")
