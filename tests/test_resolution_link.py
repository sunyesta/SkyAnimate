"""The resolution's link (the button between Width and Height): linked, changing one side changes the
other, keeping the proportion they had when linked; saved with the file.

Run with:  OpenAnimate --background --python tests/test_resolution_link.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
assert (doc.width, doc.height) == (1920, 1080) and doc.resolution_linked is False

# Unlinked: each side on its own.
doc.width = 1280
assert (doc.width, doc.height) == (1280, 1080)

# Linked at 1280 x 720: the other side follows, either way.
doc.height = 720
doc.resolution_linked = True
doc.width = 1920
assert (doc.width, doc.height) == (1920, 1080), (doc.width, doc.height)
doc.height = 540
assert (doc.width, doc.height) == (960, 540), (doc.width, doc.height)
# The proportion is the one it was linked at, not worn away by rounding.
doc.width = 1001
doc.width = 1280
assert (doc.width, doc.height) == (1280, 720), (doc.width, doc.height)

# Saved with the file.
path = str(pathlib.Path(tempfile.mkdtemp()) / "resolution.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
doc = oanim.data.document
assert doc.resolution_linked is True
doc.width = 640
assert (doc.width, doc.height) == (640, 360), (doc.width, doc.height)

# Unlinked again: on its own.
doc.resolution_linked = False
doc.width = 800
assert (doc.width, doc.height) == (800, 360)

print("resolution_link: ok")
