"""Media View (`): every media layer at full opacity with the viewport's overlays hidden. A view only:
no layer's opacity or the overlay settings change, nothing is keyed, saved or undone.

Run with:  OpenAnimate --background --python tests/test_media_view.py
"""

import os

import oanim

HERE = os.path.dirname(os.path.abspath(__file__))
STILL = os.path.join(HERE, "data", "media", "still.png")

doc = oanim.data.document
assert oanim.ops.media.add(filepath=STILL) == {'FINISHED'}
media = oanim.data.layers.active
media.opacity = 0.3
doc.frame_current = 12
history = oanim.wm.undo_history()[0]

assert not oanim.wm.media_view()
assert oanim.wm.event_simulate("ACCENT_GRAVE", area="Viewport"), "` toggles it over the viewport"
assert oanim.wm.media_view()

# Nothing about the document changes: the layer keeps its opacity (no key), the overlays their setting.
doc.frame_current = 13
assert abs(media.opacity - 0.3) < 1e-6
assert media.keyframes("opacity") == [], "no key"
assert oanim.wm.undo_history()[0] == history, "no undo step"

assert oanim.wm.event_simulate("ACCENT_GRAVE", area="Viewport")
assert not oanim.wm.media_view(), "again: back"
oanim.ops.view.media_view()
assert oanim.wm.media_view(), "the operator too"
oanim.ops.view.media_view()

print("media view: ok")
