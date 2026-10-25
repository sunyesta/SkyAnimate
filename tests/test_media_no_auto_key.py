"""Media layers' Don't Auto Key (on by default): placing, sizing or fading footage never sets keys, whatever
the record button says; keying by hand still does. Saved with the project.

Run with:  OpenAnimate --background --python tests/test_media_no_auto_key.py
"""

import os
import shutil
import tempfile

import oanim

HERE = os.path.dirname(os.path.abspath(__file__))
STILL = os.path.join(HERE, "data", "media", "still.png")

doc = oanim.data.document
assert doc.auto_key
assert oanim.ops.media.add(filepath=STILL) == {'FINISHED'}
media = oanim.data.layers.active
assert media.type == "MEDIA" and media.media_dont_auto_key, "on by default"

# On: moving it on frame 10 keys nothing; unanimated, it moves on every frame.
doc.frame_current = 10
media.location_x = 0.5
media.opacity = 0.4
doc.frame_current = 11
assert media.keyframes("position") == [] and media.keyframes("opacity") == [], "no keys"
doc.frame_current = 0
assert abs(media.location_x - 0.5) < 1e-6 and abs(media.opacity - 0.4) < 1e-6, "the change holds everywhere"

# Keying by hand still keys it.
doc.frame_current = 20
media.keyframe_insert("opacity")
assert 20 in [k["frame"] for k in media.keyframes("opacity")], media.keyframes("opacity")

# Off: auto-key keys it as any layer.
media.media_dont_auto_key = False
doc.frame_current = 30
media.location_x = 0.9
doc.frame_current = 31
assert 30 in [k["frame"] for k in media.keyframes("position")], media.keyframes("position")

# Drawings aren't affected: they still auto-key.
oanim.ops.layer.new()
drawing = oanim.data.layers.active
doc.frame_current = 40
drawing.location_x = 1.0
doc.frame_current = 41
assert 40 in [k["frame"] for k in drawing.keyframes("position")]

# Saved with the project.
folder = tempfile.mkdtemp(prefix="oanim_media_key_")
path = os.path.join(folder, "media.oanim")
oanim.ops.wm.save_as_mainfile(filepath=path)
oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
assert not oanim.data.layers[media.name].media_dont_auto_key
shutil.rmtree(folder, ignore_errors=True)

print("media don't auto key: ok")
