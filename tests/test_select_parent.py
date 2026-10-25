"""Select Parent (=): the group holding the active layer becomes the active, picked layer; up and up to
the top of the tree.

Run with:  OpenAnimate --background --python tests/test_select_parent.py
"""

import oanim

layers = oanim.data.layers

drawing = layers.active
oanim.ops.layer.group()
inner = layers.active
oanim.ops.layer.group()
outer = layers.active
assert drawing.parent.id == inner.id and inner.parent.id == outer.id and outer.parent is None

layers.active = drawing
assert oanim.wm.event_simulate("EQUAL", area="Viewport"), "= over the viewport"
assert layers.active.id == inner.id, layers.active.name
assert oanim.wm.undo_history()[0][-1] == "Select " + inner.name

assert oanim.wm.event_simulate("EQUAL", area="Layers"), "and over the Layers editor"
assert layers.active.id == outer.id

assert not oanim.wm.event_simulate("EQUAL", area="Viewport"), "at the top: nothing above"
assert layers.active.id == outer.id

oanim.ops.ed.undo()
assert layers.active.id == inner.id, "one undo step each"

print("select parent: ok")
