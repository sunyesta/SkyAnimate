"""SkyAnimate's Python API, shaped like Blender's bpy.

sky.context   the current context: the document and the editor under the mouse
sky.data      the open document: sky.data.document, sky.data.layers, sky.data.texts, sky.data.filepath
sky.assets    the asset library: sky.assets.library, sky.assets.brushes()
sky.ops       run operators by name: sky.ops.ed.undo()
sky.types     base classes to subclass: sky.types.Operator
sky.utils     register_class() and unregister_class()
sky.keymap    bind keys to operators
sky.wm        window-manager helpers, such as simulating key presses in tests
"""

import _sky

from . import keymap, ops, types, utils, wm

__all__ = ("assets", "context", "data", "keymap", "ops", "types", "utils", "wm")


class _Text:
    """A text block in the document, like bpy.types.Text."""

    def __init__(self, name):
        self._name = name

    @property
    def name(self):
        return self._name

    @property
    def body(self):
        return _sky.text_body(self._name)

    @body.setter
    def body(self, value):
        _sky.text_set_body(self._name, value)

    @property
    def filepath(self):
        return _sky.text_filepath(self._name)

    def __eq__(self, other):
        return isinstance(other, _Text) and other._name == self._name

    def __repr__(self):
        return f"sky.data.texts[{self._name!r}]"


class _Texts:
    """The document's text blocks, like bpy.data.texts."""

    def new(self, name="Text"):
        return _Text(_sky.text_new(name))

    def remove(self, text):
        _sky.text_remove(text.name)

    def keys(self):
        return _sky.text_names()

    def __getitem__(self, name):
        if name not in _sky.text_names():
            raise KeyError(name)
        return _Text(name)

    def __contains__(self, name):
        return name in _sky.text_names()

    def __iter__(self):
        return iter([_Text(name) for name in _sky.text_names()])

    def __len__(self):
        return len(_sky.text_names())

    @property
    def active(self):
        """The text shown in the text editor, or None."""
        name = _sky.text_active()
        return _Text(name) if name else None

    @active.setter
    def active(self, text):
        _sky.text_set_active(text.name if text else "")


class _Layer:
    """A layer, like Moho's MohoLayer. Properties (name, visible, opacity, mask_mode, ...) come
    from the layer's RNA description; dir() lists them."""

    def __init__(self, id):
        object.__setattr__(self, "_id", id)

    @property
    def id(self):
        return self._id

    @property
    def children(self):
        return [_Layer(i) for i in _sky.layer_children(self._id)]

    @property
    def parent(self):
        parent = _sky.layer_parent(self._id)
        return _Layer(parent) if parent else None

    @property
    def reference_source(self):
        """The layer this reference shows, or None."""
        source = _sky.layer_reference_source(self._id)
        return _Layer(source) if source else None

    @property
    def points(self):
        """[(x, y, selected), ...] for the drawing the layer shows."""
        return _sky.layer_points(self._id)

    @property
    def radii(self):
        """Each point's stroke radius, 0-1, as a fraction of its shape's line width."""
        return _sky.layer_radii(self._id)

    @property
    def mean_radius(self):
        """Mean radius of the selected points (None if none are selected). Setting it moves each
        selected point's radius by the same amount, like Blender's Mean Radius field."""
        return _sky.layer_mean_radius(self._id)

    @mean_radius.setter
    def mean_radius(self, value):
        _sky.layer_set_mean_radius(self._id, value)

    @property
    def mean_position(self):
        """Average position of the selected points (None if none); setting it moves them all."""
        p = _sky.layer_mean_position(self._id, -1)
        return tuple(p) if p is not None else None

    @mean_position.setter
    def mean_position(self, value):
        _sky.layer_set_mean_position(self._id, -1, tuple(value))

    def shape_position(self, index):
        """The middle of shape `index`'s points (the Shape panel's Position)."""
        p = _sky.layer_mean_position(self._id, index)
        return tuple(p) if p is not None else None

    def set_shape_position(self, index, value):
        """Move shape `index` so the middle of its points is at `value`."""
        _sky.layer_set_mean_position(self._id, index, tuple(value))

    @property
    def shapes(self):
        """[(name, [curve indices]), ...] in stacking order, bottom first."""
        return [(name, list(curves)) for name, curves in _sky.layer_shapes(self._id)]

    @property
    def point_count(self):
        return _sky.layer_counts(self._id)[0]

    @property
    def curve_count(self):
        return _sky.layer_counts(self._id)[1]

    @property
    def shape_count(self):
        return _sky.layer_counts(self._id)[2]

    def add_curve(self, points, closed=False, fill=False):
        """Add a curve through points [(x, y), ...] with an outline shape (and a fill if closed and fill)."""
        return _sky.layer_add_curve(self._id, points, closed, fill)

    def add_primitive(self, kind, points, fill=False):
        """Add a box or circle as the Shape tool draws them: kind "BOX" or "CIRCLE", points = the
        drag's two corners. The new points become the selection. False if too small to draw."""
        return _sky.layer_add_primitive(self._id, kind, points, fill)

    def shape_outline(self, index, steps=16):
        """(points, closed): shape `index`'s outline as drawn, `steps` samples per curve segment."""
        points, closed = _sky.layer_shape_outline(self._id, index, steps)
        return list(points), closed

    @property
    def hidden_edges(self):
        """[(curve, segment), ...] whose outline is hidden, as with Moho's Hide Edge."""
        return [tuple(e) for e in _sky.layer_hidden_edges(self._id)]

    def set_selected_edges_visible(self, visible):
        """Show or hide the outline along the selected edges (both points selected), like the
        Drawing tab's Edge > Visible. Returns how many edges changed."""
        return _sky.layer_set_selected_edges_visible(self._id, visible)

    @property
    def corners(self):
        """Each point's corner type, "ROUND" or "SHARP": how strokes turn and end there."""
        return _sky.layer_corners(self._id)

    def set_selected_corner(self, corner):
        """Set the selected points' corner type to "ROUND" or "SHARP", like the Point panel."""
        _sky.layer_set_selected_corner(self._id, corner)

    @property
    def bones(self):
        """A bone layer's bones: dicts with name, parent, position, angle (radians), length,
        scale, strength, selected, select_head, select_tail, connected, active, and head and tail (where it runs now, in the bone
        layer's space)."""
        return _sky.layer_bones(self._id)

    def bone_set(self, name, **values):
        """Pose or edit a bone: position=(x, y), angle, scale, length, strength, name."""
        _sky.layer_bone_set(self._id, name, values)

    def select_bone(self, name, extend=False, part="BONE"):
        """Select bone `name` and make it active ("" deselects all). part: "BONE" (the body and
        both ends), "HEAD" or "TAIL" (one end, as clicking its circle does)."""
        _sky.layer_select_bone(self._id, name, extend, part)

    def transform_bones(self, offset=(0.0, 0.0), scale=(1.0, 1.0), angle=0.0, pivot=None):
        """The Transform Bones gizmo: scale and turn the selected bones about pivot (default: the
        middle of their bounds), then move them by offset. Unselected children stay put."""
        if pivot is None:
            bounds = self.selected_bone_bounds
            if bounds is None:
                return False
            pivot = ((bounds[0][0] + bounds[1][0]) / 2, (bounds[0][1] + bounds[1][1]) / 2)
        return _sky.layer_map_bones(self._id, offset[0], offset[1], scale[0], scale[1], angle, pivot[0], pivot[1])

    @property
    def selected_bone_bounds(self):
        """((min x, min y), (max x, max y)) around the selected bones' ends, or None."""
        return _sky.layer_picked_bounds(self._id)

    def box_select_bones(self, corner_a, corner_b, whole=False, extend=False, deselect=False):
        """Box-select bones (bone layer space). whole=False picks the ends inside, as the Select
        tool does; whole=True picks any bone touching the box, as the Transform Bones tool does."""
        _sky.layer_box_select_bones(self._id, corner_a[0], corner_a[1], corner_b[0], corner_b[1], whole, extend,
                                    deselect)

    def rotate_bones(self, angle):
        """R on the selected bones by angle (radians), each about its base; children come along.
        A picked child of a picked parent doesn't turn twice."""
        return _sky.layer_rotate_bones(self._id, angle)

    def scale_bones(self, factor):
        """S on the selected bones: each grows by factor, children along with it (once)."""
        return _sky.layer_scale_bones(self._id, factor)

    def move_bone_ends(self, offset):
        """G on the selected bones by offset=(dx, dy): a bone picked by its body or base moves,
        one picked by its tip turns to follow it (and on frame 0 stretches). Children come along.
        Returns False when nothing is selected."""
        return _sky.layer_move_bone_ends(self._id, offset[0], offset[1])

    @property
    def point_bones(self):
        """The bone each point is bound to, "" for flexi-binding."""
        return list(_sky.layer_point_bones(self._id))

    @property
    def parent_bone(self):
        """The bone this whole layer follows (Bind Layer), or ""."""
        return _sky.layer_parent_bone(self._id)

    def bone_weights(self, bone):
        """How strongly bone `bone` moves each point, 0-1 (painted, bound or flexi)."""
        return list(_sky.layer_bone_weights(self._id, bone))

    @property
    def painted_weights(self):
        """Each point's painted weights, {bone name: weight} ({} when not painted)."""
        return list(_sky.layer_painted_weights(self._id))

    def weight_paint(self, center, radius, bone, weight=1.0, strength=1.0, mode="DRAW", normalize=True):
        """One dab of the Weight Paint brush at `center` (layer space): mode "DRAW", "ADD" or
        "SUBTRACT". Returns how many points changed."""
        return _sky.layer_weight_paint(self._id, tuple(center), radius, bone, weight, strength, mode, normalize)

    @property
    def modifiers(self):
        """The modifier stack, top first: dicts with type ("MIRROR" or "ARMATURE") and settings."""
        return _sky.layer_modifiers(self._id)

    def modifier_set(self, index, **values):
        _sky.layer_modifier_set(self._id, index, values)

    def evaluated(self, render=False):
        """The drawing as it shows after its modifiers: (points [(x, y)], curve count,
        shapes [(name, closed, connected)])."""
        points, curves, shapes = _sky.layer_evaluated(self._id, render)
        return [tuple(p) for p in points], curves, [tuple(s) for s in shapes]

    def keyframes(self, path):
        """The keys of channel `path` ("position", "rotation", "scale", "opacity",
        "points[3].pos", "points[3].radius", "curves[0].points[2].curvature", ...): dicts with
        "frame", "value", "interp" and the key's settings."""
        return _sky.layer_keyframes(self._id, path)

    def keyframe_insert(self, path="", frame=None):
        """Key channel `path` (or, with no path, every channel of the layer) at `frame` (default:
        the current frame) with its current value, like Blender's I."""
        _sky.layer_keyframe_insert(self._id, path, -1 if frame is None else frame)

    def keyframe_delete(self, path, frame):
        return _sky.layer_keyframe_delete(self._id, path, frame)

    def keyframe_set(self, path, frame, **settings):
        """Change one key: interp="LINEAR"|"SMOOTH"|"EASE_IN_OUT"|"EASE_IN"|"EASE_OUT"|"STEP"|
        "BEZIER"|"BACK"|"BOUNCE"|"ELASTIC"|"NOISY"|"CYCLE"|"POSE", hold, interval, overshoot,
        noise_amplitude, noise_frequency, cycle_back."""
        _sky.layer_keyframe_set(self._id, path, frame, settings)

    @property
    def key_frames(self):
        """Frames where the layer or its drawing has keys."""
        return list(_sky.layer_key_frames(self._id))

    def curve_handles(self, curve):
        """[(alignment, (in_x, in_y), (out_x, out_y)), ...] for each point of curve `curve`:
        "ALIGNED" (its handles stay in line) or "FREE", and its handle positions in layer space."""
        return [(t, tuple(i), tuple(o)) for t, i, o in _sky.layer_curve_handles(self._id, curve)]

    def move_handle(self, curve, index, side, pos):
        """Drag one handle ("IN" or "OUT") of a curve point to `pos`, as in the viewport: an
        aligned point's other handle turns to stay in line."""
        _sky.layer_move_handle(self._id, curve, index, side, tuple(pos))

    def select_handle(self, curve, index, side):
        _sky.layer_select_handle(self._id, curve, index, side)

    def shape_brush(self, index):
        """Shape `index`'s brush settings as a dict with Moho's .mohobrush keys; "name" is ""
        for a plain line."""
        return _sky.layer_shape_brush(self._id, index)

    def set_point(self, index, pos):
        """Move point `index` to `pos` (layer space), as dragging it in the viewport does."""
        _sky.layer_set_point(self._id, index, tuple(pos))

    def select_points(self, indices, select=True):
        _sky.layer_select_points(self._id, list(indices), select)

    def __getattr__(self, name):
        return getattr(_sky.layer_pointer(self._id), name)

    def __setattr__(self, name, value):
        # Properties defined here (mean_radius) set themselves; the rest are RNA properties.
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fset:
            prop.fset(self, value)
            return
        setattr(_sky.layer_pointer(self._id), name, value)

    def __dir__(self):
        return sorted(set(dir(_sky.layer_pointer(self._id))) | {"id", "children", "parent", "reference_source",
                                                                 "points", "radii", "mean_radius", "shapes", "point_count", "curve_count", "shape_count",
                                                                 "add_curve", "add_primitive", "shape_outline", "select_points"})

    def __eq__(self, other):
        return isinstance(other, _Layer) and other._id == self._id

    def __hash__(self):
        return hash(self._id)

    def __repr__(self):
        return f"<sky Layer {self.name!r}>"


class _Layers:
    """The document's layers, like Moho's layer list. Iterating gives the top level, top first."""

    def __iter__(self):
        return iter([_Layer(i) for i in _sky.layer_ids()])

    def __len__(self):
        return len(_sky.layer_ids())

    def __getitem__(self, key):
        if isinstance(key, int):
            return _Layer(_sky.layer_ids()[key])
        for layer in self.all():
            if layer.name == key:
                return layer
        raise KeyError(key)

    def all(self):
        """Every layer, depth first, top first."""
        result = []

        def walk(ids):
            for i in ids:
                result.append(_Layer(i))
                walk(_sky.layer_children(i))

        walk(_sky.layer_ids())
        return result

    @property
    def active(self):
        active = _sky.layer_active()
        return _Layer(active) if active else None

    @active.setter
    def active(self, layer):
        _sky.layer_set_active(layer.id)


class _Data:
    """All saved data: the document, its layers and its text blocks."""

    @property
    def document(self):
        return _sky.context().document

    @property
    def filepath(self):
        """The .sky file the document was opened from or saved to; "" for a new document."""
        return _sky.file_state()[0]

    @property
    def is_dirty(self):
        """True when there are changes since the document was saved or opened."""
        return _sky.file_state()[1]

    layers = _Layers()
    texts = _Texts()


data = _Data()


class _Assets:
    """The asset library: the folder (set in Preferences) holding the app's brushes."""

    @property
    def library(self):
        """The asset library folder; "" until one is chosen."""
        return _sky.asset_library()

    def brushes(self):
        """The library's brushes, by name: dicts of their default settings plus "images", the
        number of images (more than one for a multi-brush)."""
        return _sky.brushes()

    def brush_dab_count(self, name, length, width):
        """How many dabs brush `name` lays along a straight line `length` long and `width` wide
        (document units), with its default settings."""
        return _sky.brush_dab_count(name, length, width)


assets = _Assets()


def __getattr__(name):
    # The context changes as the mouse moves between editors, so it is looked up on access.
    if name == "context":
        return _sky.context()
    raise AttributeError(f"module 'sky' has no attribute '{name}'")
