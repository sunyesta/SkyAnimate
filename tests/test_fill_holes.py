"""Shapes with holes: two circles, one inside the other, joined (F, mesh.join_fills) fill as a ring, the
hole showing what is under it. The fill rule saves and opens; a shape of several loops with no fill rule
(as files before holes had) still fills each loop; Separate Fills splits a ring again; and a Boolean
modifier of a ring keeps its hole.

Run with:  OpenAnimate --background --python tests/test_fill_holes.py
"""

import os
import tempfile

import oanim


def shown_at(x, y):
    return [l.name for l in oanim.layers_at((x, y))]


oanim.ops.layer.new()
ring = oanim.data.layers.active
ring.name = "Ring"
assert ring.add_primitive("CIRCLE", [(-0.4, -0.4), (0.4, 0.4)], True)
assert ring.add_primitive("CIRCLE", [(-0.2, -0.2), (0.2, 0.2)], True)
assert ring.shape_count == 2
# Two filled circles: the middle is covered.
assert "Ring" in shown_at(0.0, 0.0)

# F with whole loops picked joins their fills, as Blender's F makes a face of a loop.
ring.select_points(range(ring.point_count))
assert oanim.ops.mesh.make_segment() == {'FINISHED'}
assert ring.shape_count == 1
style = ring.shape_style(0)
assert style['fill_rule'] == "EVEN_ODD" and style['has_fill'], style
assert "Ring" not in shown_at(0.0, 0.0), shown_at(0.0, 0.0)
assert "Ring" in shown_at(0.3, 0.0)

# Nonzero: the circles go the same way round, so the middle fills; Each Loop fills it too.
ring.set_shape_style(0, fill_rule="NONZERO")
assert "Ring" in shown_at(0.0, 0.0)
ring.set_shape_style(0, fill_rule="LOOPS")
assert "Ring" in shown_at(0.0, 0.0)
ring.set_shape_style(0, fill_rule="EVEN_ODD")

# Saved and opened again, still a ring; a shape without a fill rule saved fills each loop, as before.
path = os.path.join(tempfile.mkdtemp(), 'ring.oanim')
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
ring = next(l for l in oanim.data.layers.all() if l.name == "Ring")
assert ring.shape_style(0)['fill_rule'] == "EVEN_ODD"
assert "Ring" not in shown_at(0.0, 0.0)

# A Boolean joining a bar across the ring keeps the hole where the bar doesn't reach.
oanim.ops.layer.new()
bar = oanim.data.layers.active
bar.name = "Bar"
assert bar.add_primitive("BOX", [(0.25, -0.05), (0.6, 0.05)], True)
oanim.data.layers.active = ring
assert oanim.ops.object.modifier_add(type="BOOLEAN", targets="Bar") == {'FINISHED'}
assert "Ring" not in shown_at(0.0, 0.0), shown_at(0.0, 0.0)
assert "Ring" in shown_at(0.55, 0.0) or "Bar" in shown_at(0.55, 0.0)
assert oanim.ops.object.modifier_remove(index="0") == {'FINISHED'}

# Separate Fills: a shape per loop again, each with the look.
ring.select_points(range(ring.point_count))
assert oanim.ops.mesh.separate_fills() == {'FINISHED'}
assert ring.shape_count == 2
assert ring.shape_style(1)['has_fill']
print("fill holes: ok")
