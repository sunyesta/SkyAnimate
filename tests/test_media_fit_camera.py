"""Fit to Camera Frame (right-click on a Media layer in the viewport): media.fit_to_camera lays the
picture over the camera's frame as it is now, wherever the camera has gone, zoomed or turned, and
through a moved group above the layer; fit= sizes it (Stretch: its corners on the frame's). One undo step.

Run with:  OpenAnimate --background --python tests/test_media_fit_camera.py
"""

import os

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
cam = oanim.data.camera
here = os.path.dirname(os.path.abspath(__file__))
still = os.path.join(here, "data", "media", "still.png")

assert ops.layer.new_group() == {'FINISHED'}
group = layers.active
group.location_x = 0.7
group.rotation = 20.0
assert ops.media.add(filepath=still) == {'FINISHED'}
media = layers.active
assert ops.layer.move_to_group(group=str(group.id)) == {'FINISHED'}
media = next(l for l in layers.all() if l.id == media.id)
layers.active = media
media.location_x = -0.4
media.rotation = 33.0

cam.position = (1.5, -0.6)
cam.zoom = 0.5
cam.roll = 15.0

assert ops.media.fit_to_camera(fit="STRETCH") == {'FINISHED'}
assert oanim.wm.undo_history()[0][-1] == "Fit to Camera Frame"
w, h = doc.width / doc.height, 1.0


def on_frame():
    """Its stretched picture's corners (layer space) land on the camera frame's."""
    for corner in ((-w, -h), (w, -h), (w, h), (-w, h)):
        framed = cam.to_frame(media.to_document(corner))
        assert abs(framed[0] - corner[0]) < 1e-3 and abs(framed[1] - corner[1]) < 1e-3, (corner, framed)


on_frame()

# Fit (the default) keeps the picture's shape, inside the frame, centred.
assert ops.media.fit_to_camera() == {'FINISHED'}
centre = cam.to_frame(media.to_document((0.0, 0.0)))
assert abs(centre[0]) < 1e-3 and abs(centre[1]) < 1e-3, centre

# Each fit is an undo step: undoing the second leaves the first (stretched over the frame).
ops.ed.undo()
assert oanim.wm.undo_history()[0][-1] == "Fit to Camera Frame"
on_frame()

# Not on other layers; but told which Media layer (layer=), it runs whatever layer is active (its poll
# sees the argument too).
ops.layer.new()
assert ops.media.fit_to_camera() == {'CANCELLED'}
assert ops.media.fit_to_camera(layer=str(media.id)) == {'FINISHED'}
on_frame()

print("media_fit_camera: ok")
