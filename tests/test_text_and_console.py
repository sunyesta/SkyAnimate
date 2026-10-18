"""Text blocks, running them as scripts, the interactive console, and workspace switching.

Run with:  SkyAnimate --background --python tests/test_text_and_console.py
"""

import sky
from sky import _console

texts = sky.data.texts
doc = sky.data.document

# --- Text blocks --------------------------------------------------------------
assert len(texts) == 0
first = texts.new()
second = texts.new()
assert (first.name, second.name) == ("Text", "Text.001"), (first.name, second.name)
assert texts.active == first, "the first text becomes the one the editor shows"
first.body = "import sky\nsky.data.document.frame_end = 77\nprint('ran', sky.data.document.frame_end)\n"

# --- Running a text, as Alt P does --------------------------------------------
assert sky.ops.text.run_script() == {'FINISHED'}
assert doc.frame_end == 77
console = _console.get()
assert ("OUTPUT", "ran 77") in console.lines, console.lines[-5:]

second.body = "raise ValueError('broken script')"
texts.active = second
assert sky.ops.text.run_script() == {'CANCELLED'}
assert any(kind == "ERROR" and "broken script" in text for kind, text in console.lines)

# Undo of other work leaves typed text alone.
sky.ops.layer.new()
first.body = "edited after"
sky.ops.ed.undo()
assert first.body == "edited after"

texts.remove(second)
assert "Text.001" not in texts and texts.active == first

# --- The console ----------------------------------------------------------------
console.push("1 + 1")
assert console.lines[-1] == ("OUTPUT", "2"), console.lines[-1]
console.push("x = 5")
console.push("x * 3")
assert console.lines[-1] == ("OUTPUT", "15")
console.push("def f():")
assert console.prompt() == "... ", "an open block asks for more"
console.push("    return D.document.frame_end")
console.push("")
console.push("f()")
assert console.lines[-1] == ("OUTPUT", str(doc.frame_end))
console.push("1 / 0")
assert console.lines[-1][0] == "ERROR" and "ZeroDivisionError" in console.lines[-1][1]
assert console.complete("sky.dat") == "sky.data"
assert console.history[-1] == "1 / 0"

# --- Keys while typing in the text editor ------------------------------------------
# Keys a text field uses itself stay with the text...
doc.frame_current = 10
assert not sky.wm.event_simulate("RIGHT_ARROW", area="Text Editor", typing=True)
assert doc.frame_current == 10
# ...but other global shortcuts still work, as Ctrl Q does in Blender.
sky.keymap.add("Window", "screen.frame_jump_end", "F9", ctrl=True)
assert sky.wm.event_simulate("F9", ctrl=True, area="Text Editor", typing=True)
assert doc.frame_current == doc.frame_end
# Alt P belongs to the text editor and runs the active text.
first.body = "import sky\nsky.data.document.frame_end = 99\n"
texts.active = first
assert sky.wm.event_simulate("P", alt=True, area="Text Editor", typing=True)
assert doc.frame_end == 99

# --- Workspaces --------------------------------------------------------------
# Background mode has one workspace slot, so cycling is unavailable there.
assert not sky.ops.screen.workspace_cycle_next.poll()

print("Text and console tests passed")
