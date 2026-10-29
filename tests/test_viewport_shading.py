"""Viewport Shading: its renderer (Classic or GPU Geometry) and Performance options, saved with the project and set
from Python."""
import os
import tempfile

import oanim

assert oanim.wm.shading() == {"layer_caching": True, "zoom_preview": True, "gpu_geometry": False},     "Classic, both Performance options on in a new project"
oanim.wm.set_shading("layer_caching", False)
oanim.wm.set_shading("zoom_preview", False)
oanim.wm.set_shading("gpu_geometry", True)
assert oanim.wm.shading() == {"layer_caching": False, "zoom_preview": False, "gpu_geometry": True}
try:
    oanim.wm.set_shading("wireframe", True)
    raise AssertionError("an unknown option is refused")
except ValueError:
    pass

path = os.path.join(tempfile.mkdtemp(prefix="oanim_test_"), "shading.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
oanim.wm.set_shading("layer_caching", True)
oanim.wm.set_shading("gpu_geometry", False)
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
assert oanim.wm.shading() == {"layer_caching": False, "zoom_preview": False, "gpu_geometry": True}, "kept by the file"
print("viewport shading ok")
