"""Milestone 1 gate: a Python operator runs from a key press and undoes cleanly.

Run with:  OpenAnimate --background --python tests/test_milestone_1_gate.py
A failed assert makes Open Animate exit with code 1.
"""

import oanim


class TEST_OT_add_ten(oanim.types.Operator):
    """Add ten frames to the end of the frame range"""

    bl_idname = "test.add_ten"
    bl_label = "Add Ten"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        context.document.frame_end += 10
        return {'FINISHED'}


oanim.utils.register_class(TEST_OT_add_ten)
oanim.keymap.add("Viewport", "test.add_ten", "F5")

doc = oanim.data.document
start = doc.frame_end

# The key runs the operator only through the Viewport keymap.
assert not oanim.wm.event_simulate("F5"), "F5 should not be bound in the Window keymap"
assert doc.frame_end == start
assert oanim.wm.event_simulate("F5", area="Viewport"), "F5 over the viewport should run test.add_ten"
assert doc.frame_end == start + 10, doc.frame_end

names, active = oanim.wm.undo_history()
assert names[-1] == "Add Ten" and active == len(names) - 1, (names, active)

# Ctrl+Z undoes it, Ctrl+Shift+Z redoes it.
assert oanim.wm.event_simulate("Z", ctrl=True)
assert doc.frame_end == start, doc.frame_end
assert oanim.wm.event_simulate("Z", ctrl=True, shift=True)
assert doc.frame_end == start + 10, doc.frame_end

# Nothing left to redo: the key press finds no operator whose poll passes.
assert not oanim.wm.event_simulate("Z", ctrl=True, shift=True)

print("Milestone 1 gate passed")
