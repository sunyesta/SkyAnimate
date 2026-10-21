"""Edges shown or hidden on a reference layer (Moho's Hide Edge): the reference's own, kept by the
ids of the points at either end. The original's edges stay as they are, the original's later changes
come through on the other edges, Revert Looks drops them, and they are saved.

Run with:  OpenAnimate --background --python tests/test_reference_edges.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers

orig = layers.active
orig.add_curve([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], closed=True, fill=True)
oanim.ops.mesh.select_none()
assert oanim.ops.layer.make_reference() == {'FINISHED'}
ref = layers.active
ref_id, orig_id = ref.id, orig.id
assert ref.hidden_edges == [] and orig.hidden_edges == []

# Hide the first edge on the reference only (points 0 and 1 selected: the edge between them).
ref.select_points([0, 1])
assert ref.set_selected_edges_visible(False) == 1
assert ref.hidden_edges == [(0, 0)], "hidden on the reference"
assert orig.hidden_edges == [], "the original's edge stays"
doc.frame_current = 0
assert ref.hidden_edges == [(0, 0)], "kept when the reference is built again"

# The original hiding another edge comes through; the reference's own stays its own.
layers.active = orig
orig.select_points([2, 3])
orig.set_selected_edges_visible(False)
assert orig.hidden_edges == [(0, 2)]
doc.frame_current = 0
assert sorted(ref.hidden_edges) == [(0, 0), (0, 2)]

# Showing the original's hidden edge on the reference: its own again.
layers.active = ref
oanim.ops.mesh.select_none()
ref.select_points([2, 3])
ref.set_selected_edges_visible(True)
assert ref.hidden_edges == [(0, 0)] and orig.hidden_edges == [(0, 2)]

# Saved and opened again.
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_ref_edges_")) / "edges.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
layers = oanim.data.layers
ref = next(l for l in layers if l.id == ref_id)
orig = next(l for l in layers if l.id == orig_id)
assert ref.hidden_edges == [(0, 0)] and orig.hidden_edges == [(0, 2)]

# Revert Looks: the edges follow the original again.
layers.active = ref
assert oanim.ops.layer.reference_revert(kind="LOOKS") == {'FINISHED'}
assert ref.hidden_edges == [(0, 2)]

print("test_reference_edges: OK")
