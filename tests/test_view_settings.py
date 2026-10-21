"""How the project is shown, saved with it: the viewports' overlays and which panels and folding
rows are open. Undo leaves them as they are (as Blender's undo leaves its views); a new file starts
from the defaults.

Run with:  SkyAnimate --background --python tests/test_view_settings.py
"""

import pathlib
import tempfile

import sky

wm = sky.wm
defaults = wm.overlays()
assert defaults["show"] and not defaults["grid"] and defaults["handles"] == "SHAPE" and defaults["armature"] == "ON"
assert defaults["mask_outlines"] and defaults["relationship_lines"]
assert wm.ui_open() == {}, "every panel at its default"

wm.set_overlay("grid", True)
wm.set_overlay("mask_outlines", False)
wm.set_overlay("handles", "ALL")
wm.set_overlay("armature", "SELECTED")
wm.set_ui_open("Properties/Transform", False)
wm.set_ui_open("Timeline/All", False)
wm.set_ui_open("Viewport/Adjust Last Operation", True)

# Undo leaves them alone.
assert sky.ops.layer.new() == {'FINISHED'}
sky.ops.ed.undo()
ov = wm.overlays()
assert ov["grid"] and not ov["mask_outlines"] and ov["handles"] == "ALL" and ov["armature"] == "SELECTED", ov
assert wm.ui_open()["Properties/Transform"] is False and wm.ui_open()["Viewport/Adjust Last Operation"] is True

# Saved with the project; a new file has the defaults; opening brings them back.
path = str(pathlib.Path(tempfile.mkdtemp(prefix="sky_view_settings_")) / "view.sky")
assert sky.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert sky.ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
assert wm.overlays() == defaults and wm.ui_open() == {}
assert sky.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
assert wm.overlays() == ov, wm.overlays()
assert wm.ui_open() == {"Properties/Transform": False, "Timeline/All": False,
                        "Viewport/Adjust Last Operation": True}, wm.ui_open()

print("View settings tests passed")
