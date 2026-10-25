"""Text layers move, turn and scale as a whole with G, R and S (as Media layers do), and have Don't Auto
Key (on by default): placing, sizing or fading text never sets keys, whatever the record button says;
keying by hand still does. Saved with the project.

Run with:  OpenAnimate --background --python tests/test_text_no_auto_key.py
"""

import os
import shutil
import tempfile

import oanim

doc = oanim.data.document
assert doc.auto_key
assert oanim.ops.layer.new_text(text="Title") == {'FINISHED'}
text = oanim.data.layers.active
assert text.type == "TEXT" and text.text_dont_auto_key, "on by default"

# G, R and S are offered for the text layer itself (it has no points to pick).
for key in ("G", "R", "S"):
    assert oanim.wm.event_simulate(key, area="Viewport"), key

# On: moving it on frame 10 keys nothing; unanimated, it moves on every frame.
doc.frame_current = 10
text.location_x = 0.5
text.opacity = 0.4
doc.frame_current = 11
assert text.keyframes("position") == [] and text.keyframes("opacity") == [], "no keys"
doc.frame_current = 0
assert abs(text.location_x - 0.5) < 1e-6 and abs(text.opacity - 0.4) < 1e-6, "the change holds everywhere"

# Keying by hand still keys it.
doc.frame_current = 20
text.keyframe_insert("opacity")
assert 20 in [k["frame"] for k in text.keyframes("opacity")], text.keyframes("opacity")

# Off: auto-key keys it as any layer.
text.text_dont_auto_key = False
doc.frame_current = 30
text.location_x = 0.9
doc.frame_current = 31
assert 30 in [k["frame"] for k in text.keyframes("position")], text.keyframes("position")

# Saved with the project.
folder = tempfile.mkdtemp(prefix="oanim_text_key_")
path = os.path.join(folder, "text.oanim")
oanim.ops.wm.save_as_mainfile(filepath=path)
oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
assert not oanim.data.layers[text.name].text_dont_auto_key
shutil.rmtree(folder, ignore_errors=True)
print("text don't auto key: ok")
