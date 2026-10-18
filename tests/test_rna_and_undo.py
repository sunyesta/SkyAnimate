"""Property access through RNA, operator calls from Python, and undo history.

Run with:  SkyAnimate --background --python tests/test_rna_and_undo.py
"""

import sky


def expect_error(error_type, func):
    try:
        func()
    except error_type:
        return
    raise AssertionError(f"expected {error_type.__name__}")


doc = sky.data.document

# --- Properties ---------------------------------------------------------------
assert "fps" in dir(doc) and "frame_end" in dir(doc)
doc.fps = 30
assert doc.fps == 30.0 and isinstance(doc.fps, float)
doc.frame_end = 10_000_000
assert doc.frame_end == 1048574, "values are clamped to the property's range"
doc.name = "Walk cycle"
assert doc.name == "Walk cycle"
expect_error(TypeError, lambda: setattr(doc, "fps", "fast"))
expect_error(TypeError, lambda: setattr(doc, "frame_end", 2.5))
expect_error(TypeError, lambda: setattr(doc, "counter", True))
expect_error(AttributeError, lambda: doc.missing)
expect_error(AttributeError, lambda: setattr(doc, "missing", 1))

# --- Operators from Python ----------------------------------------------------
assert doc.counter == 0
assert not sky.ops.doc.counter_reset.poll(), "reset is unavailable while the counter is zero"
assert sky.ops.doc.counter_reset() == {'CANCELLED'}
assert sky.ops.doc.counter_add() == {'FINISHED'}
assert doc.counter == 1
assert sky.ops.doc.counter_reset.poll()
expect_error(KeyError, lambda: sky.ops.doc.does_not_exist())
assert "counter_add" in dir(sky.ops.doc)

# --- Undo ---------------------------------------------------------------------
history_before = len(sky.wm.undo_history()[0])
assert sky.ops.ed.undo() == {'FINISHED'}
assert doc.counter == 0
assert sky.ops.ed.redo() == {'FINISHED'}
assert doc.counter == 1


# --- Broken operators cancel and leave no undo step ----------------------------
class TEST_OT_raises(sky.types.Operator):
    bl_idname = "test.raises"
    bl_label = "Raises"
    bl_options = {'UNDO'}

    def execute(self, context):
        context.document.counter = 99
        raise RuntimeError("deliberate failure")


class TEST_OT_bad_return(sky.types.Operator):
    bl_idname = "test.bad_return"
    bl_label = "Bad Return"
    bl_options = {'UNDO'}

    def execute(self, context):
        return "FINISHED"


class TEST_OT_bad_idname(sky.types.Operator):
    bl_idname = "Test.BadName"
    bl_label = "Bad Name"

    def execute(self, context):
        return {'FINISHED'}


for cls in (TEST_OT_raises, TEST_OT_bad_return):
    sky.utils.register_class(cls)
expect_error(ValueError, lambda: sky.utils.register_class(TEST_OT_bad_idname))

assert sky.ops.test.raises() == {'CANCELLED'}
assert sky.ops.test.bad_return() == {'CANCELLED'}
assert len(sky.wm.undo_history()[0]) == history_before, "cancelled operators push no undo step"
doc.counter = 1  # restore what test.raises changed before failing

sky.utils.unregister_class(TEST_OT_raises)
expect_error(KeyError, lambda: sky.ops.test.raises())

print("RNA and undo tests passed")
