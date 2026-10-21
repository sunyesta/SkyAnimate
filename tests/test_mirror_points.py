"""Mirror editing of points (X / Y symmetry in the viewport header): a point's mirror image is found
in the setup pose (frame 0), so the pairs hold on every frame, and it snaps to the reflected spot:
the sides become symmetrical again on any frame.

Run with:  OpenAnimate --background --python tests/test_mirror_points.py
"""

import oanim

doc = oanim.data.document
doc.frame_current = 0
oanim.ops.layer.new()
layer = oanim.data.layers.active
layer.add_curve([(-1.0, 0.0), (-0.5, 0.5), (0.5, 0.5), (1.0, 0.0)])
oanim.wm.set_mirror_editing(x=True)


def pos(i):
    return layer.points[i][:2]


def close(a, b):
    return abs(a[0] - b[0]) < 1e-4 and abs(a[1] - b[1]) < 1e-4


def settle():
    """Records the edit (a frame change does) and comes back."""
    here = doc.frame_current
    doc.frame_current = here + 1
    doc.frame_current = here


oanim.ops.mesh.select_none()
layer.select_points([1])

# Frame 0: the image goes to the reflected spot.
assert layer.move_points((0.0, 0.2))
assert close(pos(1), (-0.5, 0.7)) and close(pos(2), (0.5, 0.7)), (pos(1), pos(2))
settle()

# Another frame: still paired, by the setup pose.
doc.frame_current = 10
assert layer.move_points((-0.1, 0.1))
assert close(pos(1), (-0.6, 0.8)) and close(pos(2), (0.6, 0.8)), (pos(1), pos(2))
settle()

# The sides differ on this frame (moved with mirroring off): the pair is still found although the
# points no longer mirror each other here, and the image snaps back to symmetry.
doc.frame_current = 20
oanim.wm.set_mirror_editing()
assert layer.move_points((0.3, 0.0))
settle()
left, right = pos(1), pos(2)
assert not close((-left[0], left[1]), right), "the sides differ now"
oanim.wm.set_mirror_editing(x=True)
assert layer.move_points((0.1, 0.1))
assert close(pos(1), (left[0] + 0.1, left[1] + 0.1)), pos(1)
assert close(pos(2), (-(left[0] + 0.1), left[1] + 0.1)), ("snapped to the reflection", pos(2), right)
settle()

# The setup pose is untouched by all that.
doc.frame_current = 0
assert close(pos(1), (-0.5, 0.7)) and close(pos(2), (0.5, 0.7)), (pos(1), pos(2))
oanim.wm.set_mirror_editing()

print("Mirror point tests passed")
