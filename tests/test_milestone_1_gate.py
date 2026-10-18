"""Milestone 1 gate: a Python operator runs from a key press and undoes cleanly.

Run with:  SkyAnimate --background --python tests/test_milestone_1_gate.py
A failed assert makes SkyAnimate exit with code 1.
"""

import sky


class TEST_OT_add_ten(sky.types.Operator):
    """Add ten to the test counter"""

    bl_idname = "test.add_ten"
    bl_label = "Add Ten"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        context.document.counter += 10
        return {'FINISHED'}


sky.utils.register_class(TEST_OT_add_ten)
sky.keymap.add("Viewport", "test.add_ten", "F5")

doc = sky.data.document
assert doc.counter == 0, doc.counter

# The key runs the operator only through the Viewport keymap.
assert not sky.wm.event_simulate("F5"), "F5 should not be bound in the Window keymap"
assert doc.counter == 0
assert sky.wm.event_simulate("F5", area="Viewport"), "F5 over the viewport should run test.add_ten"
assert doc.counter == 10, doc.counter

names, active = sky.wm.undo_history()
assert names[-1] == "Add Ten" and active == len(names) - 1, (names, active)

# Ctrl+Z undoes it, Ctrl+Shift+Z redoes it.
assert sky.wm.event_simulate("Z", ctrl=True)
assert doc.counter == 0, doc.counter
assert sky.wm.event_simulate("Z", ctrl=True, shift=True)
assert doc.counter == 10, doc.counter

# Nothing left to redo: the key press finds no operator whose poll passes.
assert not sky.wm.event_simulate("Z", ctrl=True, shift=True)

print("Milestone 1 gate passed")
