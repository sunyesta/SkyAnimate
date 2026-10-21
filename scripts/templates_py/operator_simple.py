import oanim


class SimpleOperator(oanim.types.Operator):
    """Tooltip"""
    bl_idname = "doc.simple_operator"
    bl_label = "Simple Operator"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.document is not None

    def execute(self, context):
        print("Hello from", context.document.name)
        return {'FINISHED'}


def register():
    oanim.utils.register_class(SimpleOperator)


def unregister():
    oanim.utils.unregister_class(SimpleOperator)


if __name__ == "__main__":
    register()

    # test call
    oanim.ops.doc.simple_operator()
