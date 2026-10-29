"""A group moving a cage-deformed layer and its cage together (a character walked across, its top group scaled)
changes nothing about the bend: the layer's points, in its own space, come out exactly the same on every frame, so
the deform's build (and the drawing's triangles) are kept rather than worked out again for a difference of rounding."""
import oanim

ops = oanim.ops
doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0


def new_layer(name):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    return layer


cage = new_layer("Cage")
cage.add_curve([(-1, -0.9), (1.0, -1.0), (0.9, 1.0), (-1.0, 0.95)], closed=True, fill=True)
rest = [tuple(p[:2]) for p in cage.points]
art = new_layer("Art")
art.add_curve([(-0.6, -0.5), (0.5, -0.4), (0.4, 0.6), (-0.5, 0.5)], closed=True, fill=True)
art.add_curve([(-1.4, 0.1), (0.0, 0.2), (1.4, 0.1)])
layers.active = art
assert ops.object.modifier_add(type="CAGE_DEFORM") == {'FINISHED'}
art.modifier_set(0, layer=cage.name, shape=cage.shapes[0][0], cage_method="ENVELOPE", precision=0.002)

# Both into one group, and that into another (the shared groups the transform is worked out below).
layers.active = cage
art.select = True
assert ops.layer.group() == {'FINISHED'}
inner = layers.active
assert cage.parent is not None and art.parent is not None and cage.parent.name == art.parent.name
layers.active = inner
assert ops.layer.group() == {'FINISHED'}
outer = layers.active

# The cage bent (held from frame 1 on), and the groups moving on every frame in amounts rounding shows in.
doc.frame_current = 1
cage.set_point(2, (rest[2][0] + 0.25, rest[2][1] + 0.3))
for frame in range(1, 8):
    doc.frame_current = frame
    outer.location_x = 0.37 * frame
    outer.scale_x = 1.0 + 0.13 * frame
    outer.scale_y = 1.0 + 0.11 * frame
    inner.rotation = 7.3 * frame
    inner.location_y = -0.21 * frame

doc.frame_current = 1
first = art.evaluated()[0]
assert max(abs(p[0] - q[0]) + abs(p[1] - q[1]) for p, q in zip(first, art.points)) > 1e-3, "the cage bends nothing"
for frame in range(2, 8):
    doc.frame_current = frame
    now = art.evaluated()[0]
    assert now == first, ("frame %d: the bend moved with the groups" % frame,
                          max(abs(p[0] - q[0]) + abs(p[1] - q[1]) for p, q in zip(first, now)))
print("deform under moving groups: exact on 6 frames")
