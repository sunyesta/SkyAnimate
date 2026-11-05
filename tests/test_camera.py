"""The camera: a frame on the infinite canvas that renders show. Its position, zoom and roll are
keyed like any layer's channels (the zoom at a steady rate: its keys hold log2 of it); it is picked
in the Layers editor or by its frame, saved with the project, and files from before it get one where
the page was.

Run with:  OpenAnimate --background --python tests/test_camera.py
"""

import math
import os
import tempfile

import oanim

doc = oanim.data.document
cam = oanim.data.camera
doc.frame_current = 0
doc.auto_key = True


def close(a, b, eps=1e-4):
    if isinstance(a, (tuple, list)):
        return all(abs(x - y) < eps for x, y in zip(a, b))
    return abs(a - b) < eps


# Where the page always was: at the origin, zoom 1, unrolled.
assert close(cam.position, (0.0, 0.0)) and close(cam.zoom, 1.0) and close(cam.roll, 0.0)
assert not cam.selected and close(cam.passepartout, 0.6)
aspect = doc.width / doc.height
assert close(cam.to_frame((aspect, 1.0)), (aspect, 1.0)), "zoom 1 at the origin: the old page"

# Renders look through it: moved, zoomed and rolled, its frame's corner is the picture's corner.
cam.position = (2.0, 1.0)
cam.zoom = 2.0
assert close(cam.to_frame((2.0, 1.0)), (0.0, 0.0)), "its centre is the picture's"
assert close(cam.to_frame((2.0 + aspect / 2, 1.5)), (aspect, 1.0)), "zoom 2 shows half as much"
cam.roll = 90.0
assert close(cam.to_frame((2.0, 1.5)), (1.0, 0.0)), "rolled a quarter left, up in the document is right in the picture"
cam.roll = 0.0
cam.position = (0.0, 0.0)
cam.zoom = 1.0

# Keyed like a layer: auto-keying away from frame 0, and the zoom at a steady rate (1 to 4 passes 2
# halfway), its keys holding log2 of it.
doc.frame_current = 10
cam.zoom = 4.0
cam.position = (1.0, 0.0)
doc.frame_current = 5
assert close(cam.zoom, 2.0), ("halfway between 1 and 4 is 2 at a steady rate", cam.zoom)
assert close(cam.position, (0.5, 0.0)), cam.position
assert [round(k["value"], 5) for k in cam.keyframes("zoom")] == [0.0, 2.0], cam.keyframes("zoom")
assert [k["frame"] for k in cam.keyframes("position")] == [0, 10]
doc.frame_current = 0
assert close(cam.zoom, 1.0)

# Its keys show in the timeline's channels, named for a camera.
assert oanim.ops.anim.keyframe_toggle(layer=str(cam.id), paths="rotation") == {'FINISHED'}
assert [k["frame"] for k in cam.keyframes("rotation")] == [0]

# The timeline lists the camera last, its rows named for a camera, picked or not.
def camera_rows():
    rows = oanim.wm.timeline_rows()
    names = [r[1] for r in rows]
    if "Camera" not in names:
        return []
    return [(r[0], r[1]) for r in rows[names.index("Camera"):]]


oanim.ops.mesh.select_none()
assert camera_rows() == [(0, "Camera"), (1, "Position"), (1, "Roll"), (1, "Zoom")], camera_rows()
cam.selected = True
assert camera_rows() == [(0, "Camera"), (1, "Position"), (1, "Roll"), (1, "Zoom")], camera_rows()
cam.selected = False

# Picked, and saved with the project, keys and all.
cam.selected = True
cam.passepartout = 0.25
folder = tempfile.mkdtemp(prefix="oanim_camera_")
path = os.path.join(folder, "camera.oanim")
assert oanim.ops.wm.save_mainfile(filepath=path) == {'FINISHED'}
cam.selected = False
cam.passepartout = 1.0
assert oanim.ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
cam = oanim.data.camera
assert cam.selected and close(cam.passepartout, 0.25)
assert [k["frame"] for k in cam.keyframes("zoom")] == [0, 10]
doc.frame_current = 5
assert close(cam.zoom, 2.0) and close(cam.position, (0.5, 0.0))
doc.frame_current = 0

# A new document's camera is the default again (as a file from before cameras gets).
assert oanim.ops.wm.read_homefile() == {'FINISHED'}
cam = oanim.data.camera
assert close(cam.position, (0.0, 0.0)) and close(cam.zoom, 1.0) and not cam.selected
assert cam.keyframes("zoom") == []

print("Camera tests passed")
