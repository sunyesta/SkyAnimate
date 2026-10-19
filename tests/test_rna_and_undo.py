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
assert (doc.viewport_quality, doc.render_quality) == (12, 32)
doc.render_quality = 500
assert doc.render_quality == 64, "quality is clamped to 1-64"
doc.render_quality = 32
assert (doc.viewport_samples, doc.render_samples) == (4, 16)
doc.render_samples = 64
assert doc.render_samples == 16, "samples are clamped to 1-16"
assert not sky.ops.render.render.poll(), "rendering needs the window"
assert doc.name == "Walk cycle"
expect_error(TypeError, lambda: setattr(doc, "fps", "fast"))
expect_error(TypeError, lambda: setattr(doc, "frame_end", 2.5))
expect_error(TypeError, lambda: setattr(doc, "width", True))
expect_error(AttributeError, lambda: doc.missing)
expect_error(AttributeError, lambda: setattr(doc, "missing", 1))

# --- Operators from Python ----------------------------------------------------
layers = sky.data.layers
assert len(layers) == 1 and layers[0].name == "Layer 1", "a new document has one layer"
assert sky.ops.layer.break_reference() == {'CANCELLED'}, "unavailable when the active layer is no reference"
assert sky.ops.layer.new() == {'FINISHED'}
assert len(layers) == 2 and layers.active.name == "Layer 2"
assert sky.ops.layer.make_reference.poll()
expect_error(KeyError, lambda: sky.ops.doc.does_not_exist())
assert "new" in dir(sky.ops.layer)

# --- Undo ---------------------------------------------------------------------
history_before = len(sky.wm.undo_history()[0])
assert sky.ops.ed.undo() == {'FINISHED'}
assert len(layers) == 1
assert sky.ops.ed.redo() == {'FINISHED'}
assert len(layers) == 2


# --- Broken operators cancel and leave no undo step ----------------------------
class TEST_OT_raises(sky.types.Operator):
    bl_idname = "test.raises"
    bl_label = "Raises"
    bl_options = {'UNDO'}

    def execute(self, context):
        context.document.frame_end = 99
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
doc.frame_end = 250  # restore what test.raises changed

sky.utils.unregister_class(TEST_OT_raises)
expect_error(KeyError, lambda: sky.ops.test.raises())

print("RNA and undo tests passed")
