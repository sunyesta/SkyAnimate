"""SkyAnimate's Python API, shaped like Blender's bpy.

sky.context   the current context: the document and the editor under the mouse
sky.data      the open document: sky.data.document, sky.data.layers, sky.data.texts, sky.data.filepath
sky.ops       run operators by name: sky.ops.ed.undo()
sky.types     base classes to subclass: sky.types.Operator
sky.utils     register_class() and unregister_class()
sky.keymap    bind keys to operators
sky.wm        window-manager helpers, such as simulating key presses in tests
"""

import _sky

from . import keymap, ops, types, utils, wm

__all__ = ("context", "data", "keymap", "ops", "types", "utils", "wm")


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


def __getattr__(name):
    # The context changes as the mouse moves between editors, so it is looked up on access.
    if name == "context":
        return _sky.context()
    raise AttributeError(f"module 'sky' has no attribute '{name}'")
