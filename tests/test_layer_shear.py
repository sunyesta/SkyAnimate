"""A layer's Shear (X and Y, degrees): the layer's X axis tilts by Shear X and its Y axis by Shear Y, between
its rotation and its scale; keyed like the rest of its transform, saved with the file.

Run with:  OpenAnimate --background --python tests/test_layer_shear.py
"""

import math
import os
import tempfile

import oanim

doc = oanim.data.document
oanim.ops.layer.new()
layer = oanim.data.layers.active
assert (layer.shear_x, layer.shear_y) == (0.0, 0.0)


def near(a, b):
    return abs(a[0] - b[0]) < 1e-5 and abs(a[1] - b[1]) < 1e-5


# Shear Y 30: a point up the Y axis leans left by tan 30; one along X stays.
layer.shear_y = 30.0
assert near(layer.to_document((0.0, 1.0)), (-math.sin(math.radians(30)), math.cos(math.radians(30))))
assert near(layer.to_document((1.0, 0.0)), (1.0, 0.0))
# Shear X 20 tilts the X axis up; then rotation and scale on top, in that order (T R Shear S).
layer.shear_y = 0.0
layer.shear_x = 20.0
layer.rotation = 90.0
layer.scale_x = 2.0
x = layer.to_document((1.0, 0.0))
expect = (2 * math.cos(math.radians(110)), 2 * math.sin(math.radians(110)))
assert near(x, expect), (x, expect)

# Keyed as the rest of the transform (auto-key on another frame), and saved with the file.
layer.rotation, layer.scale_x, layer.shear_x = 0.0, 1.0, 0.0
doc.frame_current = 10
layer.shear_y = 15.0
doc.frame_current = 11
# The setup frame's key comes with it, as every layer channel's does.
assert [k["frame"] for k in layer.keyframes("shear")] == [0, 10], layer.keyframes("shear")
doc.frame_current = 0
assert abs(layer.shear_y) < 1e-6, "the setup pose has none"
path = os.path.join(tempfile.mkdtemp(), "shear.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layer = oanim.data.layers.active
doc.frame_current = 10
assert abs(layer.shear_y - 15.0) < 1e-4, layer.shear_y
print("layer shear: ok")
