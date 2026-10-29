"""Viewport Shading: its renderer (GPU Geometry by default, or Classic) and Performance options, saved with the
project and set from Python."""
import json
import os
import tempfile
import zipfile

import oanim

assert oanim.wm.shading() == {"layer_caching": True, "zoom_preview": True, "gpu_geometry": True}, \
    "GPU Geometry, both Performance options on in a new project"
oanim.wm.set_shading("layer_caching", False)
oanim.wm.set_shading("zoom_preview", False)
oanim.wm.set_shading("gpu_geometry", False)
assert oanim.wm.shading() == {"layer_caching": False, "zoom_preview": False, "gpu_geometry": False}
try:
    oanim.wm.set_shading("wireframe", True)
    raise AssertionError("an unknown option is refused")
except ValueError:
    pass

folder = tempfile.mkdtemp(prefix="oanim_test_")
path = os.path.join(folder, "shading.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
oanim.wm.set_shading("layer_caching", True)
oanim.wm.set_shading("gpu_geometry", True)
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
assert oanim.wm.shading() == {"layer_caching": False, "zoom_preview": False, "gpu_geometry": False}, \
    "Classic kept by the file"

# A file saved while Classic was the default says "gpu_geometry": false, not a renderer: it opens in GPU Geometry.
old = os.path.join(folder, "old.oanim")
with zipfile.ZipFile(path) as src, zipfile.ZipFile(old, "w", zipfile.ZIP_DEFLATED) as dst:
    for item in src.infolist():
        data = src.read(item.filename)
        if item.filename.endswith(".json") and b"viewport_shading" in data:
            saved = json.loads(data)
            shading = saved["document"]["viewport_shading"]
            shading.pop("renderer")
            shading["gpu_geometry"] = False
            data = json.dumps(saved).encode()
        dst.writestr(item, data)
assert oanim.ops.wm.open_mainfile(filepath=old, confirmed="1") == {'FINISHED'}
assert oanim.wm.shading()["gpu_geometry"], "an older file opens in GPU Geometry"
print("viewport shading ok")
